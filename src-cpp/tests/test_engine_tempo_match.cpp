// Objective engine-level tempo-matching tests for the PULSE JUCE audio engine.
//
// Verifies that AudioEngine::matchTempo computes a bounded tempo-match decision from two
// loaded tracks' detected BPMs, applies the matched ratios to each deck through the existing
// per-deck setters, and that the tempo scaling actually flows through the real-time mixing
// path (processAudioBlock) without clipping. All rendering is offline/deterministic; no live
// hardware required.
//
// NOTE: AudioEngine is a process-wide singleton. Each case re-initializes and reloads tracks.

#include "../include/AudioEngine.h"
#include "../include/WavWriter.h"
#include "../include/TempoStrategy.h"
#include <iostream>
#include <fstream>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <vector>

int main() {
    std::cout << "Running PULSE Engine-Level Tempo-Matching Integration Tests..." << std::endl;

    constexpr uint32_t kSampleRate = 48000;
    constexpr uint32_t kChannels = 2;
    constexpr uint32_t kBlockSize = 512;

    std::filesystem::path tmpDir = std::filesystem::temp_directory_path() / "pulse_engine_tempo_e2e";
    std::filesystem::remove_all(tmpDir);
    std::filesystem::create_directories(tmpDir);

    const std::string pathA120 = (tmpDir / "deckA_120.wav").string();
    const std::string pathB125 = (tmpDir / "deckB_125.wav").string();
    const std::string pathB60  = (tmpDir / "deckB_60.wav").string();
    const std::string pathB150 = (tmpDir / "deckB_150.wav").string();

    // Fixtures carry detectable BPMs (percussive impulses at the target tempo).
    assert(pulse::audio::WavWriter::createTempoTestFixture(pathA120, 440.0, 8.0, 120.0, 0.80f, kSampleRate));
    assert(pulse::audio::WavWriter::createTempoTestFixture(pathB125, 880.0, 8.0, 125.0, 0.80f, kSampleRate));
    assert(pulse::audio::WavWriter::createTempoTestFixture(pathB60,  440.0, 8.0,  60.0, 0.80f, kSampleRate));
    assert(pulse::audio::WavWriter::createTempoTestFixture(pathB150, 440.0, 8.0, 150.0, 0.80f, kSampleRate));

    auto& engine = pulse::audio::AudioEngine::getInstance();
    AudioEngineConfigC config{kSampleRate, kBlockSize, kChannels};

    // ------------------------------------------------------------------
    // Case 1: Match-Source tempo matching (120 BPM -> 125 BPM)
    // ------------------------------------------------------------------
    assert(engine.initialize(config) == 0);
    assert(engine.loadTrack(0, pathA120));
    assert(engine.loadTrack(1, pathB125));

    auto sourceDecision = engine.matchTempo(0, 1, pulse::audio::TempoStrategyMode::Source);
    std::cout << "  [MatchSource] strategy=" << sourceDecision.strategy
              << " ratioA=" << sourceDecision.effectiveDeckATempoRatio
              << " ratioB=" << sourceDecision.effectiveDeckBTempoRatio << "\n";

    assert(sourceDecision.strategy == "match_source");
    assert(sourceDecision.effectiveDeckATempoRatio == 1.0);
    assert(std::abs(sourceDecision.effectiveDeckBTempoRatio - (120.0 / 125.0)) < 0.02);
    assert(sourceDecision.pitchSemitoneShift == 0.0f);          // pitch preserved
    assert(!sourceDecision.stretchExceededThreshold);

    // Ratios must be applied to the decks through the existing per-deck setters.
    assert(engine.getDeck(0)->getTempoRatio() == 1.0);
    assert(std::abs(engine.getDeck(1)->getTempoRatio() - (120.0 / 125.0)) < 0.02);

    // ------------------------------------------------------------------
    // Case 2: Octave match (120 BPM source vs 60 BPM dest, Auto) -> ratio 1.0/1.0
    // ------------------------------------------------------------------
    assert(engine.loadTrack(0, pathA120));
    assert(engine.loadTrack(1, pathB60));

    auto octaveDecision = engine.matchTempo(0, 1, pulse::audio::TempoStrategyMode::Auto);
    std::cout << "  [Octave]      strategy=" << octaveDecision.strategy
              << " octaveApplied=" << (int)octaveDecision.octaveJumpApplied
              << " ratioA=" << octaveDecision.effectiveDeckATempoRatio
              << " ratioB=" << octaveDecision.effectiveDeckBTempoRatio << "\n";

    assert(octaveDecision.strategy == "octave_match");
    assert(octaveDecision.octaveJumpApplied == true);
    assert(octaveDecision.effectiveDeckATempoRatio == 1.0);
    assert(octaveDecision.effectiveDeckBTempoRatio == 1.0);
    // Applied deck state must reflect the unity ratios.
    assert(engine.getDeck(0)->getTempoRatio() == 1.0);
    assert(engine.getDeck(1)->getTempoRatio() == 1.0);

    // ------------------------------------------------------------------
    // Case 3: Excessive-stretch rejection (120 BPM vs 150 BPM, > 6% delta)
    // ------------------------------------------------------------------
    assert(engine.loadTrack(0, pathA120));
    assert(engine.loadTrack(1, pathB150));

    auto stretchDecision = engine.matchTempo(0, 1, pulse::audio::TempoStrategyMode::Source);
    std::cout << "  [Stretch]     strategy=" << stretchDecision.strategy
              << " exceeded=" << (int)stretchDecision.stretchExceededThreshold
              << " ratioB=" << stretchDecision.effectiveDeckBTempoRatio << "\n";

    assert(stretchDecision.strategy == "match_source");
    assert(std::abs(stretchDecision.effectiveDeckBTempoRatio - (120.0 / 150.0)) < 0.02);
    assert(stretchDecision.stretchExceededThreshold == true);   // > 6% delta rejected
    assert(!stretchDecision.rejectionReason.empty());

    // Rejection must be honored: the engine keeps native 1.0x playback rather than applying the
    // flagged extreme ratio (decision.effectiveDeckBTempoRatio still reports the computed 0.8).
    assert(engine.getDeck(0)->getTempoRatio() == 1.0);
    assert(engine.getDeck(1)->getTempoRatio() == 1.0);

    // ------------------------------------------------------------------
    // Case 4: Tempo scaling flows through the real-time mixing path (no clipping)
    // ------------------------------------------------------------------
    // Re-load the matched pair and render offline; verify unity deck advances at wall-clock rate
    // while the stretched deck advances slower, proving tempo ratio drives playback speed in
    // processAudioBlock. Master output must never clip.
    assert(engine.loadTrack(0, pathA120));
    assert(engine.loadTrack(1, pathB125));
    auto renderDecision = engine.matchTempo(0, 1, pulse::audio::TempoStrategyMode::Source);

    // Enable pitch-preserved time-stretch so a non-1.0 ratio routes through the stretched path.
    assert(engine.setDeckPitchPreservation(0, true));
    assert(engine.setDeckPitchPreservation(1, true));
    engine.getDeck(0)->setPlaybackPosition(0.0);
    engine.getDeck(0)->setPlaying(true);
    engine.getDeck(1)->setPlaybackPosition(0.0);
    engine.getDeck(1)->setPlaying(true);

    const double renderSec = 4.0;
    const uint64_t totalBlocks = (uint64_t)(renderSec * kSampleRate / kBlockSize);
    std::vector<float> block(kBlockSize * kChannels, 0.0f);
    double masterPeak = 0.0;
    for (uint64_t b = 0; b < totalBlocks; ++b) {
        engine.processAudioBlock(block.data(), kBlockSize, kChannels);
        for (float s : block) {
            const float a = std::abs(s);
            if (a > masterPeak) masterPeak = a;
        }
    }

    const double posA = engine.getDeckPosition(0);   // unity ratio -> wall clock
    const double posB = engine.getDeckPosition(1);   // stretched ratio -> slower advance
    std::cout << "  [RT-Path]     posA(unity)=" << posA
              << " posB(ratio " << renderDecision.effectiveDeckBTempoRatio << ")=" << posB
              << " masterPeak=" << masterPeak << "\n";

    assert(std::abs(posA - renderSec) < 0.1);                 // unity deck tracks wall clock
    assert(posB < posA * 0.98);                               // stretched deck advances slower
    assert(masterPeak <= 0.999);                              // safe master output, no clipping

    engine.shutdown();
    std::filesystem::remove_all(tmpDir);

    std::cout << "\n==================================================" << std::endl;
    std::cout << "🎉 ALL ENGINE-LEVEL TEMPO-MATCHING TESTS PASSED!" << std::endl;
    std::cout << "==================================================" << std::endl;
    return 0;
}
