#pragma once

#include <cmath>
#include <algorithm>

namespace pulse::audio {

/**
 * @brief Zero-allocation, branch-free one-pole parameter smoother.
 *
 * Provides real-time safe per-sample exponential smoothing to eliminate
 * zipper noise and click artifacts during parameter automation.
 */
class ParameterSmoother {
public:
    ParameterSmoother() = default;

    /**
     * @brief Resets the filter state and recomputes smoothing coefficient.
     * @param initialValue Initial parameter value.
     * @param sampleRate Audio processing sample rate (e.g. 48000 Hz).
     * @param timeConstantSeconds 63.2% rise time (default 30ms for audio controls).
     */
    void reset(float initialValue, float sampleRate, float timeConstantSeconds = 0.030f) noexcept {
        current_ = initialValue;
        target_ = initialValue;
        if (sampleRate > 0.0f && timeConstantSeconds > 0.0f) {
            alpha_ = 1.0f - std::exp(-1.0f / (timeConstantSeconds * sampleRate));
        } else {
            alpha_ = 1.0f;
        }
    }

    /**
     * @brief Updates target value for smoothing.
     */
    inline void setTarget(float target) noexcept {
        target_ = target;
    }

    /**
     * @brief Immediately snaps current value to target without smoothing.
     */
    inline void setImmediate(float value) noexcept {
        current_ = value;
        target_ = value;
    }

    /**
     * @brief Returns current active smoothed value.
     */
    inline float getCurrent() const noexcept {
        return current_;
    }

    /**
     * @brief Returns target value.
     */
    inline float getTarget() const noexcept {
        return target_;
    }

    /**
     * @brief Advances smoother by one sample and returns updated smoothed value.
     */
    inline float next() noexcept {
        current_ += alpha_ * (target_ - current_);
        // Denormal prevention and snap-to-target threshold
        if (std::abs(current_ - target_) < 1e-6f) {
            current_ = target_;
        }
        return current_;
    }

    /**
     * @brief Returns true if smoother has not yet reached target.
     */
    inline bool isSmoothing() const noexcept {
        return current_ != target_;
    }

private:
    float current_{0.0f};
    float target_{0.0f};
    float alpha_{0.05f};
};

} // namespace pulse::audio
