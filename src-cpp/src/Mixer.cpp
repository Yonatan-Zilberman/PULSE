#include "../include/Mixer.h"
#include <algorithm>
#include <cmath>

namespace pulse::audio {

Mixer::Mixer()
    : dcBlockers_(8) {}

void Mixer::init(uint32_t sampleRate) noexcept {
    if (sampleRate == 0) sampleRate = 48000;
    sampleRate_ = sampleRate;
    float sr = static_cast<float>(sampleRate);
    constexpr float kPi = 3.14159265358979323846f;
    dcR_ = std::clamp(1.0f - (2.0f * kPi * 15.0f / sr), 0.95f, 0.9999f);

    xfadeSmoother_.reset(crossfaderPosition_.load(std::memory_order_relaxed), sr, 0.025f);
    masterVolSmoother_.reset(masterVolume_.load(std::memory_order_relaxed), sr, 0.025f);
    isInitialized_ = true;
    reset();
}

void Mixer::reset() noexcept {
    for (auto& b : dcBlockers_) {
        b.reset();
    }
}

void Mixer::setCrossfader(float position) noexcept {
    float clamped = std::clamp(position, -1.0f, 1.0f);
    crossfaderPosition_.store(clamped, std::memory_order_relaxed);
    if (!isInitialized_) {
        xfadeSmoother_.setImmediate(clamped);
    }
}

float Mixer::getCrossfader() const noexcept {
    return crossfaderPosition_.load(std::memory_order_relaxed);
}

void Mixer::setMasterVolume(float vol) noexcept {
    float clamped = std::clamp(vol, 0.0f, 1.0f);
    masterVolume_.store(clamped, std::memory_order_relaxed);
    if (!isInitialized_) {
        masterVolSmoother_.setImmediate(clamped);
    }
}

float Mixer::getMasterVolume() const noexcept {
    return masterVolume_.load(std::memory_order_relaxed);
}

void Mixer::mix(const float* deckABuffer, const float* deckBBuffer, float* masterBuffer, uint32_t numSamples, uint32_t numChannels) noexcept {
    if (!masterBuffer || !deckABuffer || !deckBBuffer) return;

    constexpr float kPiOverFour = 0.7853981633974483f; // pi / 4

    if (!isInitialized_) {
        // Fast static evaluation path for uninitialized standalone unit tests
        float xfade = crossfaderPosition_.load(std::memory_order_relaxed);
        float theta = (xfade + 1.0f) * kPiOverFour;
        float gainA = std::cos(theta);
        float gainB = std::sin(theta);
        float masterGain = masterVolume_.load(std::memory_order_relaxed);

        uint32_t totalSamples = numSamples * numChannels;
        for (uint32_t i = 0; i < totalSamples; ++i) {
            float rawMix = (deckABuffer[i] * gainA + deckBBuffer[i] * gainB) * masterGain;
            masterBuffer[i] = softLimit(rawMix);
        }
        return;
    }

    xfadeSmoother_.setTarget(crossfaderPosition_.load(std::memory_order_relaxed));
    masterVolSmoother_.setTarget(masterVolume_.load(std::memory_order_relaxed));

    for (uint32_t s = 0; s < numSamples; ++s) {
        float xfade = xfadeSmoother_.next();
        float masterGain = masterVolSmoother_.next();

        float theta = (xfade + 1.0f) * kPiOverFour;
        float gainA = std::cos(theta);
        float gainB = std::sin(theta);

        for (uint32_t c = 0; c < numChannels; ++c) {
            uint32_t idx = s * numChannels + c;
            float rawMix = (deckABuffer[idx] * gainA + deckBBuffer[idx] * gainB) * masterGain;

            float dcFiltered = rawMix;
            if (dcBlockerEnabled_) {
                dcFiltered = dcBlockers_[c % dcBlockers_.size()].process(rawMix, dcR_);
            }

            masterBuffer[idx] = softLimit(dcFiltered);
        }
    }
}

} // namespace pulse::audio
