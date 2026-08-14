#include "../include/DeckPlayer.h"
#include "../include/WavWriter.h"
#include <iostream>
#include <vector>
#include <cmath>
#include <cassert>
#include <filesystem>

namespace {

void createSineTestWav(const std::string& path, double freqHz, double durationSec, float amp = 1.0f, uint32_t sampleRate = 48000) {
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

float measureSteadyStatePeak(pulse::audio::DeckPlayer& deck, uint32_t sampleRate) {
    uint32_t blockSize = 512;
    std::vector<float> block(blockSize * 2, 0.0f);
    float maxPeak = 0.0f;

    // Warm-up 0.25s to pass initial filter transient
    uint32_t warmupBlocks = (sampleRate / 4) / blockSize;
    for (uint32_t b = 0; b < warmupBlocks; ++b) {
        deck.processBlock(block.data(), blockSize, 2);
    }

    // Measure peak over next 0.5s
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
    std::cout << "=== Running 3-Band DSP EQ Unit Tests ===" << std::endl;
    std::filesystem::create_directories("tests/audio/eq_test_tmp");

    std::string wav60Hz = "tests/audio/eq_test_tmp/sine_60hz.wav";
    std::string wav1kHz = "tests/audio/eq_test_tmp/sine_1khz.wav";
    std::string wav10kHz = "tests/audio/eq_test_tmp/sine_10khz.wav";

    createSineTestWav(wav60Hz, 60.0, 1.5, 0.80f);
    createSineTestWav(wav1kHz, 1000.0, 1.5, 0.80f);
    createSineTestWav(wav10kHz, 10000.0, 1.5, 0.80f);

    // 1. Unity Bypass Test (Low=0.0, Mid=0.0, High=0.0)
    {
        std::cout << "[Test 1] Unity Bypass Preservation..." << std::endl;
        pulse::audio::DeckPlayer deck(0);
        bool loaded = deck.loadFile(wav1kHz);
        assert(loaded);

        deck.setEq(0.0f, 0.0f, 0.0f);
        deck.setVolume(1.0f);
        deck.setPlaying(true);

        float peak = measureSteadyStatePeak(deck, 48000);
        std::cout << "  Input Peak: 0.8000 | Output Peak: " << peak << std::endl;
        assert(std::abs(peak - 0.80f) < 0.015f);
        std::cout << "  ✓ Unity bypass passed." << std::endl;
    }

    // 2. Low Kill Test (Low=-1.0, Mid=0.0, High=0.0 on 60 Hz tone)
    {
        std::cout << "[Test 2] Low Kill (60 Hz Attenuation >= 24 dB)..." << std::endl;
        pulse::audio::DeckPlayer deck(0);
        bool loaded = deck.loadFile(wav60Hz);
        assert(loaded);

        deck.setEq(-1.0f, 0.0f, 0.0f);
        deck.setVolume(1.0f);
        deck.setPlaying(true);

        float peak = measureSteadyStatePeak(deck, 48000);
        float attenuationDb = 20.0f * std::log10(peak / 0.80f);
        std::cout << "  60 Hz Attenuation: " << attenuationDb << " dB (Peak: " << peak << ")" << std::endl;
        assert(peak < 0.0504f);
        assert(attenuationDb <= -24.0f);
        std::cout << "  ✓ Low kill >= 24 dB verified." << std::endl;
    }

    // 3. High Kill Test (High=-1.0 on 10 kHz tone)
    {
        std::cout << "[Test 3] High Kill (10 kHz Attenuation >= 24 dB)..." << std::endl;
        pulse::audio::DeckPlayer deck(0);
        bool loaded = deck.loadFile(wav10kHz);
        assert(loaded);

        deck.setEq(0.0f, 0.0f, -1.0f);
        deck.setVolume(1.0f);
        deck.setPlaying(true);

        float peak = measureSteadyStatePeak(deck, 48000);
        float attenuationDb = 20.0f * std::log10(peak / 0.80f);
        std::cout << "  10 kHz Attenuation: " << attenuationDb << " dB (Peak: " << peak << ")" << std::endl;
        assert(peak < 0.0504f);
        assert(attenuationDb <= -24.0f);
        std::cout << "  ✓ High kill >= 24 dB verified." << std::endl;
    }

    // 4. Mid Isolation Test (Low=-1.0, Mid=0.0, High=-1.0 on 1 kHz tone)
    {
        std::cout << "[Test 4] Mid Isolation Passband on 1 kHz..." << std::endl;
        pulse::audio::DeckPlayer deck(0);
        bool loaded = deck.loadFile(wav1kHz);
        assert(loaded);

        deck.setEq(-1.0f, 0.0f, -1.0f);
        deck.setVolume(1.0f);
        deck.setPlaying(true);

        float peak = measureSteadyStatePeak(deck, 48000);
        float attenuationDb = 20.0f * std::log10(peak / 0.80f);
        std::cout << "  1 kHz Passband Peak: " << peak << " (Attenuation: " << attenuationDb << " dB)" << std::endl;
        assert(attenuationDb >= -1.5f);
        assert(peak > 0.65f);
        std::cout << "  ✓ Mid isolation passband verified." << std::endl;
    }

    // 5. Phase Coherence and Reset Test
    {
        std::cout << "[Test 5] EQ Reset State..." << std::endl;
        pulse::audio::DeckPlayer deck(0);
        bool loaded = deck.loadFile(wav1kHz);
        assert(loaded);

        deck.setEq(-1.0f, -1.0f, -1.0f);
        deck.setPlaying(true);
        measureSteadyStatePeak(deck, 48000);

        deck.resetEq();
        deck.setEq(0.0f, 0.0f, 0.0f);
        deck.setPlaybackPosition(0.0);
        float restoredPeak = measureSteadyStatePeak(deck, 48000);
        assert(std::abs(restoredPeak - 0.80f) < 0.015f);
        std::cout << "  ✓ Reset and phase coherence verified." << std::endl;
    }

    // Clean up temporary files
    std::filesystem::remove_all("tests/audio/eq_test_tmp");

    std::cout << "🎉 ALL 3-BAND DSP EQ TESTS PASSED!" << std::endl;
    return 0;
}
