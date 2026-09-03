#include "../include/DeckPlayer.h"
#include "../include/Mixer.h"
#include "../include/AudioEngine.h"
#include "../include/WavWriter.h"
#include "../include/AudioDecoder.h"
#include <iostream>
#include <vector>
#include <cmath>
#include <cassert>
#include <filesystem>
#include <algorithm>
#include <numbers>

namespace {

void createMultiToneFixture(const std::string& path, double durationSec = 2.0, uint32_t sampleRate = 48000) {
    uint64_t totalFrames = static_cast<uint64_t>(durationSec * sampleRate);
    std::vector<float> samples(totalFrames * 2, 0.0f);
    constexpr double twoPi = 2.0 * std::numbers::pi;

    for (uint64_t f = 0; f < totalFrames; ++f) {
        double t = static_cast<double>(f) / sampleRate;
        // Equal sum of 60 Hz (bass), 1 kHz (mid), and 10 kHz (high)
        float s60 = static_cast<float>(std::sin(twoPi * 60.0 * t) * 0.25);
        float s1k = static_cast<float>(std::sin(twoPi * 1000.0 * t) * 0.25);
        float s10k = static_cast<float>(std::sin(twoPi * 10000.0 * t) * 0.25);
        float val = s60 + s1k + s10k; // Peak ~0.75

        samples[f * 2 + 0] = val;
        samples[f * 2 + 1] = val;
    }
    pulse::audio::WavWriter::writeWav16(path, samples.data(), totalFrames, sampleRate, 2);
}

void createSineFixture(const std::string& path, double freqHz, double durationSec = 2.0, float amp = 0.8f, uint32_t sampleRate = 48000) {
    uint64_t totalFrames = static_cast<uint64_t>(durationSec * sampleRate);
    std::vector<float> samples(totalFrames * 2, 0.0f);
    constexpr double twoPi = 2.0 * std::numbers::pi;

    for (uint64_t f = 0; f < totalFrames; ++f) {
        double t = static_cast<double>(f) / sampleRate;
        float val = static_cast<float>(std::sin(twoPi * freqHz * t) * amp);
        samples[f * 2 + 0] = val;
        samples[f * 2 + 1] = val;
    }
    pulse::audio::WavWriter::writeWav16(path, samples.data(), totalFrames, sampleRate, 2);
}

float measureSteadyStatePeak(pulse::audio::DeckPlayer& deck, uint32_t sampleRate = 48000) {
    constexpr uint32_t blockSize = 512;
    std::vector<float> block(blockSize * 2, 0.0f);
    float maxPeak = 0.0f;

    // Warm-up 0.2s to pass initial filter settling
    uint32_t warmupBlocks = (sampleRate / 5) / blockSize;
    for (uint32_t b = 0; b < warmupBlocks; ++b) {
        deck.processBlock(block.data(), blockSize, 2);
    }

    // Measure over next 0.5s
    uint32_t measureBlocks = (sampleRate / 2) / blockSize;
    for (uint32_t b = 0; b < measureBlocks; ++b) {
        deck.processBlock(block.data(), blockSize, 2);
        for (float s : block) {
            float a = std::abs(s);
            if (a > maxPeak) maxPeak = a;
        }
    }
    return maxPeak;
}

} // anonymous namespace

int main() {
    std::cout << "==================================================" << std::endl;
    std::cout << "Running Production Real-Time DSP Test Suite..." << std::endl;
    std::cout << "==================================================" << std::endl;

    std::filesystem::path tempDir = std::filesystem::temp_directory_path() / "pulse_production_dsp_tests";
    std::filesystem::create_directories(tempDir);

    std::string tone60Hz = (tempDir / "tone_60hz.wav").string();
    std::string tone1kHz = (tempDir / "tone_1khz.wav").string();
    std::string tone10kHz = (tempDir / "tone_10khz.wav").string();
    std::string multiTone = (tempDir / "multi_tone.wav").string();

    createSineFixture(tone60Hz, 60.0, 3.0, 0.80f);
    createSineFixture(tone1kHz, 1000.0, 3.0, 0.80f);
    createSineFixture(tone10kHz, 10000.0, 3.0, 0.80f);
    createMultiToneFixture(multiTone, 3.0);

    // =========================================================================
    // Test 1: Unity Bypass Preservation Across All Stages
    // =========================================================================
    std::cout << "\n[Test 1] Unity Bypass Verification (Gain=1, EQ=0, Filter=0, Stems=1)..." << std::endl;
    {
        pulse::audio::DeckPlayer deck(0);
        assert(deck.loadFile(tone1kHz));
        deck.setVolume(1.0f);
        deck.setEq(0.0f, 0.0f, 0.0f);
        deck.setFilter(0.0f);
        deck.setStemLevels(1.0f, 1.0f, 1.0f, 1.0f);
        deck.setTempoRatio(1.0);
        deck.setPlaying(true);

        float peak = measureSteadyStatePeak(deck, 48000);
        float delta = std::abs(peak - 0.80f);
        std::cout << "  Input Target: 0.8000 | Rendered Steady Peak: " << peak << " (Delta: " << delta << ")" << std::endl;
        assert(delta < 0.015f); // LR4 flat passband tolerance < 0.15 dB
        std::cout << "  ✓ Unity bypass verified with high precision." << std::endl;
    }

    // =========================================================================
    // Test 2: Bipolar DJ Filter Frequency Response (LPF & HPF)
    // =========================================================================
    std::cout << "\n[Test 2] Bipolar DJ Filter Response (LPF and HPF)..." << std::endl;
    {
        // 2A: Full Low-Pass Filter (-1.0) on 10 kHz Tone
        {
            pulse::audio::DeckPlayer deck(0);
            assert(deck.loadFile(tone10kHz));
            deck.setFilter(-1.0f); // Cutoff sweeps to 50 Hz
            deck.setVolume(1.0f);
            deck.setPlaying(true);

            float peak = measureSteadyStatePeak(deck, 48000);
            float attenuationDb = 20.0f * std::log10(peak / 0.80f);
            std::cout << "  LPF (-1.0) on 10 kHz: Peak = " << peak << " (Atten: " << attenuationDb << " dB)" << std::endl;
            assert(attenuationDb <= -30.0f); // >= 30 dB attenuation of highs
            assert(peak < 0.026f);
        }

        // 2B: Full Low-Pass Filter (-1.0) on 60 Hz Tone (Passband preservation)
        {
            pulse::audio::DeckPlayer deck(0);
            assert(deck.loadFile(tone60Hz));
            deck.setFilter(-1.0f);
            deck.setVolume(1.0f);
            deck.setPlaying(true);

            float peak = measureSteadyStatePeak(deck, 48000);
            float attenuationDb = 20.0f * std::log10(peak / 0.80f);
            std::cout << "  LPF (-1.0) on 60 Hz: Peak = " << peak << " (Atten: " << attenuationDb << " dB)" << std::endl;
            assert(attenuationDb >= -6.0f); // Bass retained in low-pass passband (cutoff is 50 Hz, -4.88 dB at 60 Hz)
        }

        // 2C: Full High-Pass Filter (+1.0) on 60 Hz Tone
        {
            pulse::audio::DeckPlayer deck(0);
            assert(deck.loadFile(tone60Hz));
            deck.setFilter(1.0f); // Cutoff sweeps to 15 kHz
            deck.setVolume(1.0f);
            deck.setPlaying(true);

            float peak = measureSteadyStatePeak(deck, 48000);
            float attenuationDb = 20.0f * std::log10(peak / 0.80f);
            std::cout << "  HPF (+1.0) on 60 Hz: Peak = " << peak << " (Atten: " << attenuationDb << " dB)" << std::endl;
            assert(attenuationDb <= -30.0f); // >= 30 dB attenuation of sub-bass
            assert(peak < 0.026f);
        }

        // 2D: Moderate High-Pass Filter (+0.5, fc ~550 Hz) on 10 kHz Tone (Passband preservation)
        {
            pulse::audio::DeckPlayer deck(0);
            assert(deck.loadFile(tone10kHz));
            deck.setFilter(0.5f);
            deck.setVolume(1.0f);
            deck.setPlaying(true);

            float peak = measureSteadyStatePeak(deck, 48000);
            float attenuationDb = 20.0f * std::log10(peak / 0.80f);
            std::cout << "  HPF (+0.5) on 10 kHz: Peak = " << peak << " (Atten: " << attenuationDb << " dB)" << std::endl;
            assert(attenuationDb >= -3.0f);
        }
        std::cout << "  ✓ Bipolar DJ filter attenuation and passbands verified." << std::endl;
    }

    // =========================================================================
    // Test 3: Parameter Smoothing (Anti-Zipper Noise Verification)
    // =========================================================================
    std::cout << "\n[Test 3] Parameter Smoothing (Zero Step-Discontinuities)..." << std::endl;
    {
        pulse::audio::DeckPlayer deck(0);
        assert(deck.loadFile(tone1kHz));
        deck.setVolume(0.0f);
        deck.setPlaying(true);

        constexpr uint32_t blockSize = 256;
        std::vector<float> block(blockSize * 2, 0.0f);

        // Process 2 blocks at 0 volume
        deck.processBlock(block.data(), blockSize, 2);
        deck.processBlock(block.data(), blockSize, 2);

        // Sudden step in Volume: 0.0 -> 1.0
        deck.setVolume(1.0f);

        float maxSampleDelta = 0.0f;
        float prevSample = 0.0f;

        // Pump 5 blocks during smoothing ramp
        for (int b = 0; b < 5; ++b) {
            deck.processBlock(block.data(), blockSize, 2);
            for (uint32_t i = 0; i < blockSize; ++i) {
                float sampleL = block[i * 2 + 0];
                float delta = std::abs(sampleL - prevSample);
                if (delta > maxSampleDelta) maxSampleDelta = delta;
                prevSample = sampleL;
            }
        }

        std::cout << "  Max Sample-to-Sample Delta under Volume Step (0->1): " << maxSampleDelta << std::endl;
        // Without smoothing, step causes a delta of ~0.80.
        // With smooth per-sample slew on 1 kHz carrier, max sample delta is strictly bounded
        assert(maxSampleDelta < 0.20f);
        std::cout << "  ✓ Parameter smoothing verified (no zipper noise or clicks)." << std::endl;
    }

    // =========================================================================
    // Test 4: Stem Mixer Fallback on Stereo Master Audio
    // =========================================================================
    std::cout << "\n[Test 4] Stem Mixer Fallback on Stereo Master Audio..." << std::endl;
    {
        // 4A: Mute Bass Stem on 60 Hz tone
        {
            pulse::audio::DeckPlayer deck(0);
            assert(deck.loadFile(tone60Hz));
            deck.setStemLevels(1.0f, 1.0f, 0.0f, 1.0f); // Bass stem muted (0.0)
            deck.setVolume(1.0f);
            deck.setPlaying(true);

            float peak = measureSteadyStatePeak(deck, 48000);
            float attenuationDb = 20.0f * std::log10(peak / 0.80f);
            std::cout << "  Bass Stem Mute on 60 Hz: Peak = " << peak << " (Atten: " << attenuationDb << " dB)" << std::endl;
            assert(attenuationDb <= -20.0f);
        }

        // 4B: Mute Vocal Stem on 1 kHz tone
        {
            pulse::audio::DeckPlayer deck(0);
            assert(deck.loadFile(tone1kHz));
            deck.setStemLevels(0.0f, 1.0f, 1.0f, 1.0f); // Vocal stem muted (0.0)
            deck.setVolume(1.0f);
            deck.setPlaying(true);

            float peak = measureSteadyStatePeak(deck, 48000);
            float attenuationDb = 20.0f * std::log10(peak / 0.80f);
            std::cout << "  Vocal Stem Mute on 1 kHz: Peak = " << peak << " (Atten: " << attenuationDb << " dB)" << std::endl;
            assert(attenuationDb <= -15.0f);
        }
        std::cout << "  ✓ Stem mixer 3-band fallback verified." << std::endl;
    }

    // =========================================================================
    // Test 5: Safe Master Limiter & Anti-Clipping Under Severe Overload (+12 dB)
    // =========================================================================
    std::cout << "\n[Test 5] Safe Master Output Soft Limiter Under Extreme Overload (+12 dB)..." << std::endl;
    {
        pulse::audio::Mixer mixer;
        mixer.init(48000);
        mixer.setCrossfader(0.0f); // 50/50 blend
        mixer.setMasterVolume(1.0f);

        constexpr uint32_t numSamples = 512;
        constexpr uint32_t numChannels = 2;
        constexpr size_t totalSamples = numSamples * numChannels;

        // Hot signals at 4.0x peak (+12 dBFS overload)
        std::vector<float> hotDeckA(totalSamples, 4.0f);
        std::vector<float> hotDeckB(totalSamples, 4.0f);
        std::vector<float> masterOut(totalSamples, 0.0f);

        // Mix 5 blocks through master soft limiter
        for (int b = 0; b < 5; ++b) {
            mixer.mix(hotDeckA.data(), hotDeckB.data(), masterOut.data(), numSamples, numChannels);
        }

        float maxOutputPeak = 0.0f;
        for (float s : masterOut) {
            maxOutputPeak = std::max(maxOutputPeak, std::abs(s));
        }

        std::cout << "  Hot Input: +12 dBFS (4.0) | Output Ceiling: " << maxOutputPeak
                  << " (" << 20.0f * std::log10(maxOutputPeak) << " dBFS)" << std::endl;

        // Limiter must guarantee absolute ceiling <= 0.999f (no DAC wrapping)
        assert(maxOutputPeak <= 0.999f);
        assert(maxOutputPeak > 0.95f);
        std::cout << "  ✓ Safe master output limiter strictly prevented digital wrap-around clipping." << std::endl;
    }

    // =========================================================================
    // Test 6: Dynamic Tempo Adjustment Correctness
    // =========================================================================
    std::cout << "\n[Test 6] Dynamic Tempo Adjustment Correctness & Scaling..." << std::endl;
    {
        pulse::audio::DeckPlayer deck(0);
        assert(deck.loadFile(tone1kHz));
        deck.setTempoRatio(1.20); // +20% tempo
        deck.setPitchPreservation(true);
        deck.setVolume(1.0f);
        deck.setPlaying(true);

        constexpr uint32_t blockSize = 512;
        std::vector<float> block(blockSize * 2, 0.0f);

        // Process 2 seconds of audio
        uint32_t totalBlocks = (48000 * 2) / blockSize;
        for (uint32_t b = 0; b < totalBlocks; ++b) {
            deck.processBlock(block.data(), blockSize, 2);
        }

        double pos = deck.getPlaybackPosition();
        // At 1.20 ratio, 2.0s of wall-clock playback processes ~2.4s of audio
        std::cout << "  2.0s wall-clock playback at 1.20x tempo -> Track Position = " << pos << "s" << std::endl;
        assert(pos > 2.2 && pos < 2.6);
        std::cout << "  ✓ Dynamic tempo adjustment duration scaling verified." << std::endl;
    }

    std::filesystem::remove_all(tempDir);
    std::cout << "\n==================================================" << std::endl;
    std::cout << "🎉 ALL PRODUCTION REAL-TIME DSP TESTS PASSED!" << std::endl;
    std::cout << "==================================================" << std::endl;
    return 0;
}
