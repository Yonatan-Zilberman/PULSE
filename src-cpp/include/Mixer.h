#pragma once

#include "ParameterSmoother.h"
#include <cstdint>
#include <atomic>
#include <vector>
#include <cmath>
#include <algorithm>

namespace pulse::audio {

/**
 * @brief Master Mixer, Crossfader Engine, and Safe Master Output DSP.
 *
 * REAL-TIME SAFETY CONTRACT:
 * - Deterministic equal-power crossfade curves.
 * - Smooth sample-by-sample parameter interpolation.
 * - High-pass DC blocking filter.
 * - Soft-knee transparent peak limiter preventing DAC wrap-around clipping.
 * - Strict zero memory allocation and zero blocking synchronization.
 */
class Mixer {
public:
    Mixer();
    ~Mixer() = default;

    void init(uint32_t sampleRate) noexcept;
    void reset() noexcept;

    void setCrossfader(float position) noexcept; // -1.0 (Deck A) to 1.0 (Deck B)
    float getCrossfader() const noexcept;

    void setMasterVolume(float vol) noexcept;
    float getMasterVolume() const noexcept;

    void enableDcBlocker(bool enable) noexcept { dcBlockerEnabled_ = enable; }
    bool isDcBlockerEnabled() const noexcept { return dcBlockerEnabled_; }

    // Mix stereo buffers from Deck A and Deck B into master output
    void mix(const float* deckABuffer, const float* deckBBuffer, float* masterBuffer, uint32_t numSamples, uint32_t numChannels) noexcept;

    struct DCBlocker {
        float x1{0.0f};
        float y1{0.0f};

        inline float process(float x, float r = 0.998f) noexcept {
            float y = x - x1 + r * y1;
            x1 = x;
            y1 = y;
            return y;
        }

        void reset() noexcept {
            x1 = 0.0f;
            y1 = 0.0f;
        }
    };

    static inline float softLimit(float x) noexcept {
        constexpr float kThreshold = 0.89125f; // -1.0 dBFS
        float absX = std::abs(x);
        if (absX <= kThreshold) {
            return x;
        }
        float sign = (x >= 0.0f) ? 1.0f : -1.0f;
        float excess = absX - kThreshold;
        constexpr float kHeadroom = 1.0f - kThreshold; // ~0.10875f
        float compressed = kThreshold + kHeadroom * std::tanh(excess / kHeadroom);
        return std::clamp(sign * compressed, -0.999f, 0.999f);
    }

private:
    std::atomic<float> crossfaderPosition_{-1.0f};
    std::atomic<float> masterVolume_{1.0f};

    uint32_t sampleRate_{48000};
    bool isInitialized_{false};
    bool dcBlockerEnabled_{false};
    float dcR_{0.998f};

    ParameterSmoother xfadeSmoother_;
    ParameterSmoother masterVolSmoother_;
    std::vector<DCBlocker> dcBlockers_{2};
};

} // namespace pulse::audio
