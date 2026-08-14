#include "../include/Mixer.h"
#include <algorithm>
#include <cmath>

namespace pulse::audio {

Mixer::Mixer() = default;

void Mixer::setCrossfader(float position) {
    crossfaderPosition_.store(std::clamp(position, -1.0f, 1.0f));
}

float Mixer::getCrossfader() const noexcept {
    return crossfaderPosition_.load();
}

void Mixer::setMasterVolume(float vol) {
    masterVolume_.store(std::clamp(vol, 0.0f, 1.0f));
}

float Mixer::getMasterVolume() const noexcept {
    return masterVolume_.load();
}

void Mixer::mix(const float* deckABuffer, const float* deckBBuffer, float* masterBuffer, uint32_t numSamples, uint32_t numChannels) noexcept {
    if (!masterBuffer || !deckABuffer || !deckBBuffer) return;

    float xfade = crossfaderPosition_.load();
    
    // Equal-power sinusoidal crossfade curve
    constexpr float kPiOverFour = 0.7853981633974483f; // pi / 4
    float theta = (xfade + 1.0f) * kPiOverFour; // theta in [0, pi/2]
    float gainA = std::cos(theta);
    float gainB = std::sin(theta);
    float masterGain = masterVolume_.load();

    uint32_t totalSamples = numSamples * numChannels;
    for (uint32_t i = 0; i < totalSamples; ++i) {
        float rawMix = (deckABuffer[i] * gainA + deckBBuffer[i] * gainB) * masterGain;
        
        // Fast soft-knee limiter preventing digital wrap-around clipping
        if (rawMix > 1.0f) {
            masterBuffer[i] = 1.0f;
        } else if (rawMix < -1.0f) {
            masterBuffer[i] = -1.0f;
        } else {
            masterBuffer[i] = rawMix;
        }
    }
}

} // namespace pulse::audio
