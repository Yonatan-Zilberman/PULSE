#include "../include/AudioEngine.h"
#include "../include/WavWriter.h"
#include <iostream>
#include <vector>
#include <cmath>
#include <cassert>
#include <filesystem>

namespace {

void createBassToneWav(const std::string& path, double freqHz, double durationSec, float amp = 0.85f, uint32_t sampleRate = 48000) {
    uint64_t totalFrames = static_cast<uint64_t>(durationSec * sampleRate);
    std::vector<float> samples(totalFrames * 2, 0.0f);
    constexpr double twoPi = 6.28318530717958647692;

    for (uint64_t f = 0; f < totalFrames; ++f) {
        double t = static_cast<double>(f) / sampleRate;
        float val = static_cast<float>(std::sin(twoPi * freqHz * t) * amp);
        samples[f * 2 + 0] = val;
        samples[f * 2 + 1] = val;
    }
    pulse::audio::WavWriter::writeWav16(path, samples.data(), totalFrames, sampleRate, 2);
}

} // anonymous namespace

int main() {
    std::cout << "=== Running Bass Swap Gain Safety & Headroom Tests ===" << std::endl;
    std::filesystem::create_directories("tests/audio/bass_safety_tmp");

    std::string bassWavA = "tests/audio/bass_safety_tmp/bass_80hz_a.wav";
    std::string bassWavB = "tests/audio/bass_safety_tmp/bass_80hz_b.wav";

    createBassToneWav(bassWavA, 80.0, 8.0, 0.85f);
    createBassToneWav(bassWavB, 80.0, 8.0, 0.85f);

    auto& engine = pulse::audio::AudioEngine::getInstance();
    AudioEngineConfigC config{48000, 512, 2};
    engine.initialize(config);

    // 1. Execute Bass Swap Strategy
    float bassSwapMaxPeak = 0.0f;
    {
        std::cout << "[Test 1] Simulating Bass Swap Transition (4.0s window)..." << std::endl;
        engine.loadTrack(0, bassWavA);
        engine.loadTrack(1, bassWavB);

        auto* deckA = engine.getDeck(0);
        auto* deckB = engine.getDeck(1);
        auto* mixer = engine.getMixer();
        auto* transExec = engine.getTransitionExecutor();

        deckA->setPlaybackPosition(0.0);
        deckA->setPlaying(true);

        deckB->setPlaybackPosition(0.0);
        deckB->setPlaying(true);

        mixer->setCrossfader(-1.0f);
        mixer->setMasterVolume(1.0f);

        TransitionCommandC cmd{0, 1, 4.0, 2}; // 2 = BassSwap
        transExec->startTransition(cmd);

        uint32_t blockSize = 512;
        uint32_t channels = 2;
        std::vector<float> block(blockSize * channels, 0.0f);
        double dt = static_cast<double>(blockSize) / 48000.0;
        double currentTime = 0.0;

        while (currentTime < 4.0) {
            transExec->updateAutomation(currentTime, *mixer, deckA, deckB);
            engine.processAudioBlock(block.data(), blockSize, channels);

            for (float s : block) {
                float a = std::abs(s);
                if (a > bassSwapMaxPeak) {
                    bassSwapMaxPeak = a;
                }
            }
            currentTime += dt;
        }

        std::cout << "  Bass Swap Max Peak: " << bassSwapMaxPeak
                  << " (" << (20.0f * std::log10(bassSwapMaxPeak)) << " dBFS)" << std::endl;

        // Baseline single-track peak is 0.85. Max peak must not exceed +0.5 dB over 0.85 (0.9004)
        float maxAllowedPeak = 0.85f * std::pow(10.0f, 0.5f / 20.0f); // ~0.9004
        std::cout << "  Max Allowed Peak (+0.5 dB over 0.85): " << maxAllowedPeak << std::endl;
        assert(bassSwapMaxPeak <= maxAllowedPeak);
        assert(bassSwapMaxPeak <= 1.0f);
        std::cout << "  ✓ Bass Swap gain safety bounded successfully!" << std::endl;
    }

    // 2. Contrast Against Naive Crossfade (Strategy 0 = PhraseCrossfade)
    float naiveMaxPeak = 0.0f;
    {
        std::cout << "[Test 2] Simulating Naive Full-Spectrum Crossfade..." << std::endl;
        engine.loadTrack(0, bassWavA);
        engine.loadTrack(1, bassWavB);

        auto* deckA = engine.getDeck(0);
        auto* deckB = engine.getDeck(1);
        auto* mixer = engine.getMixer();
        auto* transExec = engine.getTransitionExecutor();

        deckA->setPlaybackPosition(0.0);
        deckA->setPlaying(true);

        deckB->setPlaybackPosition(0.0);
        deckB->setPlaying(true);

        mixer->setCrossfader(-1.0f);
        mixer->setMasterVolume(1.0f);

        TransitionCommandC cmd{0, 1, 4.0, 0}; // 0 = PhraseCrossfade
        transExec->startTransition(cmd);

        uint32_t blockSize = 512;
        uint32_t channels = 2;
        std::vector<float> block(blockSize * channels, 0.0f);
        double dt = static_cast<double>(blockSize) / 48000.0;
        double currentTime = 0.0;

        while (currentTime < 4.0) {
            transExec->updateAutomation(currentTime, *mixer, deckA, deckB);
            engine.processAudioBlock(block.data(), blockSize, channels);

            for (float s : block) {
                float a = std::abs(s);
                if (a > naiveMaxPeak) {
                    naiveMaxPeak = a;
                }
            }
            currentTime += dt;
        }

        std::cout << "  Naive Crossfade Max Peak: " << naiveMaxPeak
                  << " (" << (20.0f * std::log10(naiveMaxPeak)) << " dBFS)" << std::endl;

        // Naive crossfade equal power sum at midpoint: 0.85 * (cos(pi/4) + sin(pi/4)) = 0.85 * 1.414 = 1.202
        // Due to peak limiter, naiveMaxPeak hits 1.0f (saturation / limiting)
        assert(naiveMaxPeak >= 0.999f);
        std::cout << "  ✓ Proved naive crossfade triggers limiter saturation (peak = 1.000)." << std::endl;
    }

    // 3. Confirm Bass Swap Preserves Headroom over Naive Crossfade
    assert(bassSwapMaxPeak < naiveMaxPeak);
    std::cout << "  ✓ Verified Bass Swap keeps signal headroom ("
              << (20.0f * std::log10(bassSwapMaxPeak)) << " dBFS vs "
              << (20.0f * std::log10(naiveMaxPeak)) << " dBFS)." << std::endl;

    std::filesystem::remove_all("tests/audio/bass_safety_tmp");

    std::cout << "🎉 ALL BASS SWAP GAIN SAFETY TESTS PASSED!" << std::endl;
    return 0;
}
