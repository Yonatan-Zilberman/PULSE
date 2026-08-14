#include "../include/Mixer.h"
#include <algorithm>

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
    // Constant-power / linear crossfade baseline
    float gainA = (1.0f - xfade) * 0.5f;
    float gainB = (1.0f + xfade) * 0.5f;
    float masterGain = masterVolume_.load();

    uint32_t totalSamples = numSamples * numChannels;
    for (uint32_t i = 0; i < totalSamples; ++i) {
        masterBuffer[i] = (deckABuffer[i] * gainA + deckBBuffer[i] * gainB) * masterGain;
    }
}

} // namespace pulse::audio
