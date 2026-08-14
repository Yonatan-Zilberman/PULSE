#pragma once

#include "AudioBridgeTypes.h"
#include "Mixer.h"
#include <atomic>
#include <cstdint>

namespace pulse::audio {

class DeckPlayer;

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
    virtual void update(double normalizedProgress, const TransitionCommandC& command, Mixer& mixer, DeckPlayer* deckA, DeckPlayer* deckB) noexcept = 0;
};

/**
 * @brief Phrase-synchronized crossfade transition strategy.
 */
class PhraseCrossfadeStrategy final : public ITransitionStrategy {
public:
    PhraseCrossfadeStrategy() = default;
    ~PhraseCrossfadeStrategy() override = default;

    void reset() noexcept override {}
    void update(double normalizedProgress, const TransitionCommandC& command, Mixer& mixer, DeckPlayer* deckA, DeckPlayer* deckB) noexcept override;
};

/**
 * @brief Deterministic 3-band EQ crossfade transition strategy.
 */
class EqCrossfadeStrategy final : public ITransitionStrategy {
public:
    EqCrossfadeStrategy() = default;
    ~EqCrossfadeStrategy() override = default;

    void reset() noexcept override {}
    void update(double normalizedProgress, const TransitionCommandC& command, Mixer& mixer, DeckPlayer* deckA, DeckPlayer* deckB) noexcept override;
};

/**
 * @brief Sequenced Low-Frequency Bass Swap transition strategy.
 */
class BassSwapStrategy final : public ITransitionStrategy {
public:
    explicit BassSwapStrategy(double swapPoint = 0.50, double swapWindow = 0.10) noexcept
        : swapPoint_(swapPoint), swapWindow_(swapWindow) {}
    ~BassSwapStrategy() override = default;

    void reset() noexcept override {}
    void update(double normalizedProgress, const TransitionCommandC& command, Mixer& mixer, DeckPlayer* deckA, DeckPlayer* deckB) noexcept override;

    void setSwapPoint(double pt) noexcept { swapPoint_ = (pt < 0.1 ? 0.1 : (pt > 0.9 ? 0.9 : pt)); }
    double getSwapPoint() const noexcept { return swapPoint_; }

    void setSwapWindow(double w) noexcept { swapWindow_ = (w < 0.02 ? 0.02 : (w > 0.5 ? 0.5 : w)); }
    double getSwapWindow() const noexcept { return swapWindow_; }

private:
    double swapPoint_{0.50};
    double swapWindow_{0.10};
};

/**
 * @brief Deterministic Transition Automation Executor.
 */
class TransitionExecutor {
public:
    TransitionExecutor();
    ~TransitionExecutor() = default;

    void startTransition(const TransitionCommandC& command);
    void updateAutomation(double elapsedTimeSeconds, Mixer& mixer, DeckPlayer* deckA = nullptr, DeckPlayer* deckB = nullptr) noexcept;
    bool isTransitionActive() const noexcept;
    const TransitionCommandC& getActiveCommand() const noexcept { return activeCommand_; }
    double getNormalizedProgress() const noexcept { return progress_.load(); }

    void setCurveType(CrossfaderCurveType curve) noexcept { curveType_ = curve; }
    CrossfaderCurveType getCurveType() const noexcept { return curveType_; }

    BassSwapStrategy& getBassSwapStrategy() noexcept { return bassSwapStrategy_; }

private:
    std::atomic<bool> isActive_{false};
    std::atomic<double> progress_{0.0};
    TransitionCommandC activeCommand_{};
    CrossfaderCurveType curveType_{CrossfaderCurveType::EqualPower};

    // Pre-allocated strategy handlers (Zero RT heap allocation)
    PhraseCrossfadeStrategy phraseCrossfadeStrategy_;
    EqCrossfadeStrategy eqCrossfadeStrategy_;
    BassSwapStrategy bassSwapStrategy_;
    ITransitionStrategy* currentStrategy_{&phraseCrossfadeStrategy_};
};

} // namespace pulse::audio
