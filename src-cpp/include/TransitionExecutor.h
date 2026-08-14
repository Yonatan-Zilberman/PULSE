#pragma once

#include "AudioBridgeTypes.h"
#include "Mixer.h"
#include <atomic>
#include <cstdint>

namespace pulse::audio {

/**
 * @brief Supported transition strategy types for real-time execution.
 */
enum class TransitionStrategyType : uint32_t {
    PhraseCrossfade = 0,
    EqCrossfade = 1,
    BassSwap = 2,
    StemIsolation = 3,
    DropSwap = 4,
    EchoOut = 5,
    BreakdownBlend = 6,
    VocalHandoff = 7,
    EnergyCut = 8
};

/**
 * @brief Crossfader automation curve geometry.
 */
enum class CrossfaderCurveType : uint32_t {
    EqualPower = 0, // Constant power sinusoidal blend
    Linear = 1,     // Direct linear transition
    SCurve = 2      // Smooth step Hermite s-curve
};

/**
 * @brief Strategy interface for real-time transition automation.
 *
 * REAL-TIME SAFETY CONTRACT: All methods are strictly `noexcept` and cannot allocate memory.
 */
class ITransitionStrategy {
public:
    virtual ~ITransitionStrategy() = default;
    virtual void reset() noexcept = 0;
    virtual void update(double normalizedProgress, const TransitionCommandC& command, Mixer& mixer) noexcept = 0;
};

/**
 * @brief Phrase-synchronized crossfade transition strategy.
 */
class PhraseCrossfadeStrategy final : public ITransitionStrategy {
public:
    PhraseCrossfadeStrategy() = default;
    ~PhraseCrossfadeStrategy() override = default;

    void reset() noexcept override {}
    void update(double normalizedProgress, const TransitionCommandC& command, Mixer& mixer) noexcept override;
};

/**
 * @brief Deterministic Transition Automation Executor.
 */
class TransitionExecutor {
public:
    TransitionExecutor();
    ~TransitionExecutor() = default;

    void startTransition(const TransitionCommandC& command);
    void updateAutomation(double elapsedTimeSeconds, Mixer& mixer) noexcept;
    bool isTransitionActive() const noexcept;
    const TransitionCommandC& getActiveCommand() const noexcept { return activeCommand_; }
    double getNormalizedProgress() const noexcept { return progress_.load(); }

    void setCurveType(CrossfaderCurveType curve) noexcept { curveType_ = curve; }
    CrossfaderCurveType getCurveType() const noexcept { return curveType_; }

private:
    std::atomic<bool> isActive_{false};
    std::atomic<double> progress_{0.0};
    TransitionCommandC activeCommand_{};
    CrossfaderCurveType curveType_{CrossfaderCurveType::EqualPower};

    // Pre-allocated strategy handlers (Zero RT heap allocation)
    PhraseCrossfadeStrategy phraseCrossfadeStrategy_;
};

} // namespace pulse::audio
