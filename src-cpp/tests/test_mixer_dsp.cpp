#include "../include/Mixer.h"
#include <iostream>
#include <vector>
#include <cassert>
#include <cmath>

int main() {
    std::cout << "Running PULSE Mixer DSP Unit Tests..." << std::endl;

    pulse::audio::Mixer mixer;
    constexpr uint32_t numSamples = 512;
    constexpr uint32_t numChannels = 2;
    constexpr size_t totalSamples = numSamples * numChannels;

    std::vector<float> deckA(totalSamples, 0.8f);
    std::vector<float> deckB(totalSamples, -0.6f);
    std::vector<float> master(totalSamples, 0.0f);

    // Test 1: Crossfader at -1.0 (Full Deck A)
    std::cout << "Test 1: Crossfader at -1.0 (Deck A isolate)..." << std::endl;
    mixer.setCrossfader(-1.0f);
    mixer.setMasterVolume(1.0f);
    mixer.mix(deckA.data(), deckB.data(), master.data(), numSamples, numChannels);

    for (size_t i = 0; i < totalSamples; ++i) {
        assert(std::abs(master[i] - 0.8f) < 1e-4f);
    }
    std::cout << "  Deck A isolated verified." << std::endl;

    // Test 2: Crossfader at +1.0 (Full Deck B)
    std::cout << "Test 2: Crossfader at +1.0 (Deck B isolate)..." << std::endl;
    mixer.setCrossfader(1.0f);
    mixer.mix(deckA.data(), deckB.data(), master.data(), numSamples, numChannels);

    for (size_t i = 0; i < totalSamples; ++i) {
        assert(std::abs(master[i] - (-0.6f)) < 1e-4f);
    }
    std::cout << "  Deck B isolated verified." << std::endl;

    // Test 3: Center Crossfader (0.0) Equal Power Blend
    std::cout << "Test 3: Center Crossfader (0.0) Equal Power..." << std::endl;
    mixer.setCrossfader(0.0f);
    mixer.mix(deckA.data(), deckB.data(), master.data(), numSamples, numChannels);

    // theta = pi/4 => gainA = gainB = sqrt(2)/2 ~= 0.70710678
    float expected = static_cast<float>((0.8 * 0.70710678118) + (-0.6 * 0.70710678118));
    for (size_t i = 0; i < totalSamples; ++i) {
        assert(std::abs(master[i] - expected) < 1e-3f);
    }
    std::cout << "  Center crossfader equal-power blend verified." << std::endl;

    // Test 4: Master Peak Limiter & Zero Clipping Wrap-around
    std::cout << "Test 4: Master peak limiter under hot signals..." << std::endl;
    std::vector<float> hotA(totalSamples, 1.0f);
    std::vector<float> hotB(totalSamples, 1.0f);
    mixer.setCrossfader(0.0f);
    mixer.setMasterVolume(1.0f);
    mixer.mix(hotA.data(), hotB.data(), master.data(), numSamples, numChannels);

    for (size_t i = 0; i < totalSamples; ++i) {
        // Sum would be ~1.414, must be clamped to 1.0f without wrap-around
        assert(master[i] <= 1.0f);
        assert(master[i] >= -1.0f);
        assert(master[i] > 0.99f);
    }
    std::cout << "  Peak limiter successfully clamped hot mix to 1.0 without wrap-around." << std::endl;

    // Test 5: Master Volume Mute
    std::cout << "Test 5: Master volume mute (0.0)..." << std::endl;
    mixer.setMasterVolume(0.0f);
    mixer.mix(deckA.data(), deckB.data(), master.data(), numSamples, numChannels);
    for (size_t i = 0; i < totalSamples; ++i) {
        assert(master[i] == 0.0f);
    }
    std::cout << "  Master mute verified." << std::endl;

    std::cout << "All Mixer DSP tests PASSED!" << std::endl;
    return 0;
}
