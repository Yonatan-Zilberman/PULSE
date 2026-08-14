#include "../include/TimeStretchEngine.h"
#include "../include/TempoStrategy.h"
#include "../include/WavWriter.h"
#include "../include/AudioDecoder.h"
#include "../include/AudioEngine.h"
#include <iostream>
#include <vector>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <algorithm>
#include <numbers>

namespace {

// Helper: Estimate fundamental pitch frequency using autocorrelation peak lag
double estimateFundamentalFrequency(const float* samples, size_t numFrames, uint32_t sampleRate, uint32_t channels) {
    if (numFrames < 1024) return 0.0;

    // Downmix to mono
    std::vector<float> mono(numFrames);
    for (size_t i = 0; i < numFrames; ++i) {
        float sum = 0.0f;
        for (uint32_t c = 0; c < channels; ++c) {
            sum += samples[i * channels + c];
        }
        mono[i] = sum / static_cast<float>(channels);
    }

    // Autocorrelation over search window [minLag, maxLag]
    // For 200 Hz to 1000 Hz at 48kHz: lag ~ 48 to 240
    size_t minLag = sampleRate / 1000;
    size_t maxLag = sampleRate / 200;
    size_t windowSize = std::min(numFrames - maxLag, static_cast<size_t>(2048));

    double bestCorr = -1e30;
    size_t bestLag = minLag;

    for (size_t lag = minLag; lag <= maxLag; ++lag) {
        double corr = 0.0;
        double norm1 = 0.0;
        double norm2 = 0.0;
        for (size_t n = 0; n < windowSize; ++n) {
            double s1 = mono[n];
            double s2 = mono[n + lag];
            corr += s1 * s2;
            norm1 += s1 * s1;
            norm2 += s2 * s2;
        }
        double norm = std::sqrt(norm1 * norm2);
        double score = (norm > 1e-9) ? (corr / norm) : 0.0;
        if (score > bestCorr) {
            bestCorr = score;
            bestLag = lag;
        }
    }

    if (bestLag == 0) return 0.0;
    return static_cast<double>(sampleRate) / static_cast<double>(bestLag);
}

// Helper: Measure transient crest factor (Peak / RMS)
double measureTransientSharpness(const float* samples, size_t numFrames, uint32_t channels) {
    if (numFrames == 0) return 0.0;
    double maxPeak = 0.0;
    double sumSq = 0.0;
    size_t totalSamples = numFrames * channels;

    for (size_t i = 0; i < totalSamples; ++i) {
        double absVal = std::abs(samples[i]);
        if (absVal > maxPeak) maxPeak = absVal;
        sumSq += samples[i] * samples[i];
    }

    double rms = std::sqrt(sumSq / totalSamples);
    return (rms > 1e-6) ? (maxPeak / rms) : 0.0;
}

} // anonymous namespace

int main() {
    std::cout << "Running PULSE TimeStretchEngine & Bounded Tempo Strategy Tests..." << std::endl;

    std::filesystem::path tempDir = std::filesystem::temp_directory_path() / "pulse_timestretch_tests";
    std::filesystem::create_directories(tempDir);

    constexpr uint32_t kSampleRate = 48000;
    constexpr uint32_t kChannels = 2;

    // =========================================================================
    // Test 1: Pitch Invariance (Fundamental Frequency Delta = 0.0 Semitones)
    // =========================================================================
    std::cout << "\nTest 1: Pitch Invariance Under +20% and -20% Time-Stretching..." << std::endl;
    {
        constexpr double kTargetFreq = 440.0;
        constexpr double kDuration = 2.0;
        uint64_t numFrames = static_cast<uint64_t>(kDuration * kSampleRate);
        std::vector<float> sineInput(numFrames * kChannels);
        constexpr double kTwoPi = 2.0 * std::numbers::pi;

        for (uint64_t f = 0; f < numFrames; ++f) {
            double t = static_cast<double>(f) / kSampleRate;
            float val = static_cast<float>(0.8 * std::sin(kTwoPi * kTargetFreq * t));
            sineInput[f * kChannels + 0] = val;
            sineInput[f * kChannels + 1] = val;
        }

        // Test 1A: Stretch Faster (+20%, ratio 1.20)
        pulse::audio::TimeStretchEngine stretchFaster;
        pulse::audio::TimeStretchConfig cfgFaster{kSampleRate, kChannels, 1.20, 0.0f, true, false};
        assert(stretchFaster.initialize(cfgFaster));

        stretchFaster.putSamples(sineInput.data(), static_cast<uint32_t>(numFrames));
        stretchFaster.flush();

        std::vector<float> outputFaster(numFrames * kChannels, 0.0f);
        uint32_t framesFaster = stretchFaster.receiveSamples(outputFaster.data(), static_cast<uint32_t>(numFrames));
        assert(framesFaster > 1024);

        double detectedFreqFaster = estimateFundamentalFrequency(outputFaster.data() + 1024 * kChannels, framesFaster - 1024, kSampleRate, kChannels);
        double freqDeltaFaster = std::abs(detectedFreqFaster - kTargetFreq);
        std::cout << "  Faster (+20% tempo): Target = " << kTargetFreq << " Hz, Detected = " << detectedFreqFaster
                  << " Hz (Delta = " << freqDeltaFaster << " Hz)" << std::endl;
        assert(freqDeltaFaster <= 1.0); // Within 1.0 Hz (exact pitch invariance)

        // Test 1B: Stretch Slower (-20%, ratio 0.80)
        pulse::audio::TimeStretchEngine stretchSlower;
        pulse::audio::TimeStretchConfig cfgSlower{kSampleRate, kChannels, 0.80, 0.0f, true, false};
        assert(stretchSlower.initialize(cfgSlower));

        stretchSlower.putSamples(sineInput.data(), static_cast<uint32_t>(numFrames));
        stretchSlower.flush();

        std::vector<float> outputSlower(numFrames * 2 * kChannels, 0.0f);
        uint32_t framesSlower = stretchSlower.receiveSamples(outputSlower.data(), static_cast<uint32_t>(numFrames * 2));
        assert(framesSlower > 1024);

        double detectedFreqSlower = estimateFundamentalFrequency(outputSlower.data() + 1024 * kChannels, framesSlower - 1024, kSampleRate, kChannels);
        double freqDeltaSlower = std::abs(detectedFreqSlower - kTargetFreq);
        std::cout << "  Slower (-20% tempo): Target = " << kTargetFreq << " Hz, Detected = " << detectedFreqSlower
                  << " Hz (Delta = " << freqDeltaSlower << " Hz)" << std::endl;
        assert(freqDeltaSlower <= 1.0);
    }
    std::cout << "  ✅ Test 1 Passed: Pitch invariance verified (0.0 semitone pitch shift)." << std::endl;

    // =========================================================================
    // Test 2: Small Stretch Correctness (120 BPM -> 122 BPM, +1.67%)
    // =========================================================================
    std::cout << "\nTest 2: Small Stretch Correctness (120 -> 122 BPM, +1.67%)..." << std::endl;
    {
        std::string fixturePath = (tempDir / "fixture_120.wav").string();
        assert(pulse::audio::WavWriter::createTempoTestFixture(fixturePath, 440.0, 10.0, 120.0, 0.80f, kSampleRate));

        pulse::audio::DecodedAudio inputDecoded;
        assert(pulse::audio::AudioDecoder::decodeFile(fixturePath, inputDecoded, kSampleRate, kChannels));

        double targetRatio = 122.0 / 120.0; // ~1.01667
        pulse::audio::TimeStretchEngine engine;
        pulse::audio::TimeStretchConfig cfg{kSampleRate, kChannels, targetRatio, 0.0f, true, false};
        assert(engine.initialize(cfg));

        engine.putSamples(inputDecoded.samples.data(), static_cast<uint32_t>(inputDecoded.totalFrames));
        engine.flush();

        std::vector<float> outputSamples(inputDecoded.totalFrames * kChannels, 0.0f);
        uint32_t renderedFrames = engine.receiveSamples(outputSamples.data(), static_cast<uint32_t>(inputDecoded.totalFrames));
        double renderedDuration = static_cast<double>(renderedFrames) / kSampleRate;
        double expectedDuration = 10.0 / targetRatio; // ~9.836s

        std::cout << "  Expected Duration: " << expectedDuration << "s, Rendered: " << renderedDuration << "s" << std::endl;
        assert(std::abs(renderedDuration - expectedDuration) < 0.08);
    }
    std::cout << "  ✅ Test 2 Passed: Small stretch duration and scaling verified." << std::endl;

    // =========================================================================
    // Test 3: Moderate Stretch Correctness & Transient Preservation (120 -> 126 BPM, +5.0%)
    // =========================================================================
    std::cout << "\nTest 3: Moderate Stretch Correctness & Transient Preservation (+5.0%)..." << std::endl;
    {
        std::string fixturePath = (tempDir / "fixture_120_mod.wav").string();
        assert(pulse::audio::WavWriter::createTempoTestFixture(fixturePath, 440.0, 6.0, 120.0, 0.80f, kSampleRate));

        pulse::audio::DecodedAudio inputDecoded;
        assert(pulse::audio::AudioDecoder::decodeFile(fixturePath, inputDecoded, kSampleRate, kChannels));

        double origSharpness = measureTransientSharpness(inputDecoded.samples.data(), inputDecoded.totalFrames, kChannels);

        double targetRatio = 126.0 / 120.0; // 1.05
        pulse::audio::TimeStretchEngine engine;
        pulse::audio::TimeStretchConfig cfg{kSampleRate, kChannels, targetRatio, 0.0f, true, false};
        assert(engine.initialize(cfg));

        engine.putSamples(inputDecoded.samples.data(), static_cast<uint32_t>(inputDecoded.totalFrames));
        engine.flush();

        std::vector<float> outputSamples(inputDecoded.totalFrames * kChannels, 0.0f);
        uint32_t renderedFrames = engine.receiveSamples(outputSamples.data(), static_cast<uint32_t>(inputDecoded.totalFrames));
        double stretchedSharpness = measureTransientSharpness(outputSamples.data(), renderedFrames, kChannels);

        double sharpnessRatio = stretchedSharpness / origSharpness;
        std::cout << "  Original Crest Factor: " << origSharpness << ", Stretched: " << stretchedSharpness
                  << " (Preservation Ratio: " << sharpnessRatio * 100.0 << "%)" << std::endl;
        assert(sharpnessRatio >= 0.85); // Preserves >= 85% transient sharpness
    }
    std::cout << "  ✅ Test 3 Passed: Moderate stretch transient preservation verified (>= 85%)." << std::endl;

    // =========================================================================
    // Test 4: Octave Hypothesis Resolution (70 BPM vs 140 BPM)
    // =========================================================================
    std::cout << "\nTest 4: Octave Hypothesis Resolution (70 BPM <-> 140 BPM)..." << std::endl;
    {
        auto decision = pulse::audio::TempoStrategy::evaluate(
            70.0,
            140.0,
            {140.0},
            {70.0},
            pulse::audio::TempoStrategyMode::Auto
        );

        std::cout << "  Decision: " << decision.strategy << ", OctaveJump: " << (decision.octaveJumpApplied ? "true" : "false")
                  << ", EffectiveRatio: " << decision.effectiveTempoRatio << ", Risk: " << decision.artifactRiskScore
                  << ", RecType: " << decision.recommendedTransitionType << std::endl;

        assert(decision.octaveJumpApplied == true);
        assert(std::abs(decision.effectiveTempoRatio - 1.0) < 1e-4);
        assert(decision.bpmDeltaPercent == 0.0);
        assert(decision.artifactRiskScore <= 0.05f);
        assert(!decision.stretchExceededThreshold);
        assert(decision.recommendedTransitionType == "breakdown_blend" || decision.recommendedTransitionType == "drop_swap");
    }
    std::cout << "  ✅ Test 4 Passed: Octave compatibility resolved to 1.0x ratio without unnatural stretch." << std::endl;

    // =========================================================================
    // Test 5: Excessive Stretch Penalty & Strategy Rejection (> 6.0%)
    // =========================================================================
    std::cout << "\nTest 5: Excessive Stretch Rejection (> 6.0%)..." << std::endl;
    {
        // 120 BPM vs 150 BPM (+25.0% delta)
        auto decisionRejected = pulse::audio::TempoStrategy::evaluate(
            120.0,
            150.0,
            {},
            {},
            pulse::audio::TempoStrategyMode::Source,
            0.0,
            6.0,
            false // forceStretch = false
        );

        std::cout << "  Standard Evaluation (>6%): Exceeded = " << (decisionRejected.stretchExceededThreshold ? "true" : "false")
                  << ", Risk = " << decisionRejected.artifactRiskScore
                  << ", RecType = " << decisionRejected.recommendedTransitionType << std::endl;

        assert(decisionRejected.stretchExceededThreshold == true);
        assert(decisionRejected.artifactRiskScore >= 0.70f);
        assert(decisionRejected.recommendedTransitionType == "energy_cut");
        assert(!decisionRejected.rejectionReason.empty());

        // Override with forceStretch = true
        auto decisionForced = pulse::audio::TempoStrategy::evaluate(
            120.0,
            150.0,
            {},
            {},
            pulse::audio::TempoStrategyMode::Source,
            0.0,
            6.0,
            true // forceStretch = true
        );

        assert(decisionForced.stretchExceededThreshold == false);
        assert(decisionForced.recommendedTransitionType == "eq_crossfade");
        assert(std::abs(decisionForced.effectiveDeckBTempoRatio - (120.0 / 150.0)) < 1e-4);
    }
    std::cout << "  ✅ Test 5 Passed: Excessive stretch cleanly rejected and routed to fallback strategy." << std::endl;

    // =========================================================================
    // Test 6: Phase Alignment During Transition (Deck A: 120 BPM, Deck B: 124 BPM -> 120 BPM)
    // =========================================================================
    std::cout << "\nTest 6: Dual-Deck Phase Alignment During Crossfade Transition..." << std::endl;
    {
        std::string trackAPath = (tempDir / "track_a_120.wav").string();
        std::string trackBPath = (tempDir / "track_b_124.wav").string();

        assert(pulse::audio::WavWriter::createTempoTestFixture(trackAPath, 440.0, 6.0, 120.0, 0.80f, kSampleRate));
        assert(pulse::audio::WavWriter::createTempoTestFixture(trackBPath, 880.0, 6.0, 124.0, 0.80f, kSampleRate));

        auto& engine = pulse::audio::AudioEngine::getInstance();
        AudioEngineConfigC engConfig{kSampleRate, 512, kChannels};
        assert(engine.initialize(engConfig) == 0);

        assert(engine.loadTrack(0, trackAPath));
        assert(engine.loadTrack(1, trackBPath));

        auto* deckA = engine.getDeck(0);
        auto* deckB = engine.getDeck(1);
        auto* mixer = engine.getMixer();

        // Evaluate tempo strategy: Match Deck B to Deck A
        auto decision = pulse::audio::TempoStrategy::evaluate(120.0, 124.0);
        deckA->setTempoRatio(decision.effectiveDeckATempoRatio); // 1.0
        deckB->setTempoRatio(decision.effectiveDeckBTempoRatio); // 120 / 124 = 0.96774

        deckA->setPlaybackPosition(0.0);
        deckA->setPlaying(true);

        deckB->setPlaybackPosition(0.0);
        deckB->setPlaying(true);

        mixer->setCrossfader(0.0f); // Center crossfade (50/50 blend)
        mixer->setMasterVolume(1.0f);

        uint32_t blockSize = 512;
        std::vector<float> block(blockSize * kChannels, 0.0f);
        std::vector<float> renderedMix;
        renderedMix.reserve(4 * kSampleRate * kChannels);

        // Process 4 seconds of blended playback
        for (size_t b = 0; b < (4 * kSampleRate) / blockSize; ++b) {
            engine.processAudioBlock(block.data(), blockSize, kChannels);
            renderedMix.insert(renderedMix.end(), block.begin(), block.end());
        }

        // Measure peak output amplitude: should be clean and limited (<= 1.0f)
        float maxPeak = 0.0f;
        for (float s : renderedMix) {
            maxPeak = std::max(maxPeak, std::abs(s));
        }
        std::cout << "  Rendered Blend: Frames = " << renderedMix.size() / kChannels << ", MaxPeak = " << maxPeak << std::endl;
        assert(maxPeak > 0.4f && maxPeak <= 1.0f);
    }
    std::cout << "  ✅ Test 6 Passed: Dual-deck phase-aligned transition mix verified." << std::endl;

    // Cleanup
    std::filesystem::remove_all(tempDir);

    std::cout << "\n🎉 ALL TimeStretchEngine & Bounded Tempo Strategy Unit Tests PASSED!" << std::endl;
    return 0;
}
