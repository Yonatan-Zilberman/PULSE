#include "../include/TransitionPlanner.h"
#include "../include/AudioDecoder.h"
#include "../include/WavWriter.h"
#include <iostream>
#include <cassert>
#include <cmath>
#include <filesystem>

int main() {
    std::cout << "Running PULSE TransitionPlanner Unit Tests..." << std::endl;

    std::filesystem::path tempDir = std::filesystem::temp_directory_path() / "pulse_planner_tests";
    std::filesystem::create_directories(tempDir);

    std::string trackA16BarsPath = (tempDir / "track_a_16bars.wav").string();
    std::string trackB16BarsPath = (tempDir / "track_b_16bars.wav").string();
    std::string trackA6BarsPath = (tempDir / "track_a_6bars.wav").string();

    constexpr uint32_t kSampleRate = 48000;
    constexpr uint32_t kChannels = 2;

    // 1. Generate test fixtures
    // 16 bars at 120 BPM = 32.0s
    assert(pulse::audio::WavWriter::createPhraseFixture(trackA16BarsPath, 440.0, 16, 120.0, 0.80f, kSampleRate));
    assert(pulse::audio::WavWriter::createPhraseFixture(trackB16BarsPath, 523.25, 16, 120.0, 0.80f, kSampleRate));

    // 6 bars at 120 BPM = 12.0s
    assert(pulse::audio::WavWriter::createPhraseFixture(trackA6BarsPath, 440.0, 6, 120.0, 0.80f, kSampleRate));

    // Decode tracks
    pulse::audio::DecodedAudio trackA16, trackB16, trackA6;
    assert(pulse::audio::AudioDecoder::decodeFile(trackA16BarsPath, trackA16, kSampleRate, kChannels));
    assert(pulse::audio::AudioDecoder::decodeFile(trackB16BarsPath, trackB16, kSampleRate, kChannels));
    assert(pulse::audio::AudioDecoder::decodeFile(trackA6BarsPath, trackA6, kSampleRate, kChannels));

    // Test 1: Standard 8-Bar Transition Window Selection
    std::cout << "Test 1: Standard 8-bar phrase transition planning..." << std::endl;
    auto plan1 = pulse::audio::TransitionPlanner::planTransition(
        trackA16,
        trackB16,
        1.0,
        1.0,
        8,    // 8-bar preferred
        0.50, // min confidence
        true  // phrase-aware
    );

    std::cout << "  Plan 1: Strategy=" << plan1.selectedWindow.alignmentStrategy
              << ", ExitSec=" << plan1.selectedWindow.sourceExitSec
              << ", EntrySec=" << plan1.selectedWindow.destEntrySec
              << ", DurationBars=" << plan1.selectedWindow.durationBars
              << ", DurationSec=" << plan1.selectedWindow.durationSeconds
              << ", PhraseScore=" << plan1.selectedWindow.phraseScore
              << ", IsAligned=" << (plan1.selectedWindow.isPhraseAligned ? "true" : "false") << std::endl;

    assert(plan1.selectedWindow.isPhraseAligned);
    assert(plan1.selectedWindow.alignmentStrategy == "phrase_aligned");
    assert(plan1.selectedWindow.durationBars == 8);
    assert(std::abs(plan1.selectedWindow.durationSeconds - 16.0) < 0.1);
    // Outro boundary selection: for 16 bars (32.0s), 8-bar exit should be at bar 8 (16.0s)
    assert(std::abs(plan1.selectedWindow.sourceExitSec - 16.0) < 0.2);
    assert(std::abs(plan1.selectedWindow.destEntrySec - 0.0) < 0.1);
    assert(plan1.selectedWindow.phraseScore >= 0.70);
    assert(!plan1.candidateWindows.empty());
    std::cout << "  Passed: Standard 8-bar transition window selected cleanly." << std::endl;

    // Test 2: Short Track A Stepdown Fallback (6 bars = 12.0s, preferred 8 bars = 16.0s)
    std::cout << "Test 2: Short Track A stepdown fallback to 4 bars..." << std::endl;
    auto plan2 = pulse::audio::TransitionPlanner::planTransition(
        trackA6,
        trackB16,
        1.0,
        1.0,
        8,    // Requested 8 bars (doesn't fit in 12.0s)
        0.50,
        true
    );

    std::cout << "  Plan 2: Strategy=" << plan2.selectedWindow.alignmentStrategy
              << ", DurationBars=" << plan2.selectedWindow.durationBars
              << ", DurationSec=" << plan2.selectedWindow.durationSeconds
              << ", FallbackReason=" << plan2.selectedWindow.fallbackReason << std::endl;

    assert(plan2.selectedWindow.isPhraseAligned);
    assert(plan2.selectedWindow.alignmentStrategy == "phrase_aligned");
    assert(plan2.selectedWindow.durationBars == 4); // Stepped down to 4 bars
    assert(std::abs(plan2.selectedWindow.durationSeconds - 8.0) < 0.1);
    assert(!plan2.selectedWindow.fallbackReason.empty());
    std::cout << "  Passed: Gracefully stepped down from 8 bars to 4 bars." << std::endl;

    // Test 3: Low Confidence Fallback
    std::cout << "Test 3: Low phrase confidence fallback to downbeat bar snap..." << std::endl;
    pulse::audio::DecodedAudio lowConfTrack = trackA16;
    lowConfTrack.tempoProfile.phraseProfile.phraseConfidence = 0.25f; // Below 0.50 threshold

    auto plan3 = pulse::audio::TransitionPlanner::planTransition(
        lowConfTrack,
        trackB16,
        1.0,
        1.0,
        8,
        0.50, // min threshold is 0.50
        true
    );

    std::cout << "  Plan 3: Strategy=" << plan3.selectedWindow.alignmentStrategy
              << ", IsAligned=" << (plan3.selectedWindow.isPhraseAligned ? "true" : "false")
              << ", FallbackReason=" << plan3.selectedWindow.fallbackReason << std::endl;

    assert(!plan3.selectedWindow.isPhraseAligned);
    assert(plan3.selectedWindow.alignmentStrategy == "bar_fallback");
    assert(!plan3.selectedWindow.fallbackReason.empty());
    std::cout << "  Passed: Low confidence correctly triggered bar_fallback." << std::endl;

    // Test 4: Tempo-Scaled Phrase Duration
    std::cout << "Test 4: Tempo-scaled phrase duration under time-stretching..." << std::endl;
    // When Track A is stretched to 128 BPM (ratio = 128.0 / 120.0 = 1.0667):
    // 8 bars at 128 BPM = 8 * 4 * (60.0 / 128.0) = 15.0 seconds
    double ratioA = 128.0 / 120.0;
    auto plan4 = pulse::audio::TransitionPlanner::planTransition(
        trackA16,
        trackB16,
        ratioA,
        1.0,
        8,
        0.50,
        true
    );

    std::cout << "  Plan 4: Target BPM = 128.0, DurationSec = " << plan4.selectedWindow.durationSeconds
              << " (Expected 15.0s)" << std::endl;
    assert(plan4.selectedWindow.durationBars == 8);
    assert(std::abs(plan4.selectedWindow.durationSeconds - 15.0) < 0.1);
    std::cout << "  Passed: Phrase duration accurately scaled with tempo ratio." << std::endl;

    // Cleanup
    std::filesystem::remove_all(tempDir);

    std::cout << "All TransitionPlanner unit tests PASSED!" << std::endl;
    return 0;
}
