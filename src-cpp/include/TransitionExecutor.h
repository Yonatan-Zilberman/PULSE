#pragma once

#include "AudioBridgeTypes.h"
#include "Mixer.h"
#include <atomic>
#include <cstdint>

namespace pulse::audio {

class DeckPlayer;

/**
 * @brief Supported transition strategy types for real-time execution.
 *
 * ClassicEqBlend = 9 implements the Tech Design §9.1 reference model driven
 * entirely by the versioned (v2) TransitionCommandC full-plan parameters.
 * Unknown values map to PhraseCrossfade (legacy fallback).
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
    EnergyCut = 8,
    ClassicEqBlend = 9
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
 * `normalizedProgress` is elapsedSeconds/duration and MAY exceed 1.0 while a post-cut
 * tempo ramp (ClassicEqBlend phase 4) is still running; strategies must treat any
 * finite value deterministically (legacy strategies clamp to [0,1] internally).
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
 * @brief Classic EQ Blend — the Tech Design §9.1 reference transition.
 *
 * Stateless in `normalizedProgress` (every output value is a pure function of
 * the sanitized v2 command and `p`), so it is fully deterministic and trivially
 * real-time safe. Parameterless: ALL parameters come from the v2 command.
 *
 *   Phase 1 (p < phase_sync_end)      : silent destination sync + tempo match
 *   Phase 2 (p1 ≤ p < 1)              : fader move (plan curve), HPF'd bass
 *                                       handoff, mid/high + filter automation,
 *                                       precomputed gain staging
 *   Phase 3 (phase_eq_end ≤ p < 1)    : vocal stem handoff (overlaps phase 2 tail)
 *   Phase 4 (p ≥ 1, ramp > 0)         : source cut + destination tempo return ramp
 */
class ClassicEqBlendStrategy final : public ITransitionStrategy {
public:
    ClassicEqBlendStrategy() = default;
    ~ClassicEqBlendStrategy() override = default;

    void reset() noexcept override {}
    void update(double normalizedProgress, const TransitionCommandC& command, Mixer& mixer, DeckPlayer* deckA, DeckPlayer* deckB) noexcept override;
};

/**
 * @brief Deterministic Transition Automation Executor.
 *
 * Production contract:
 * - `startTransition` (control thread) validates + sanitizes the full v2 plan
 *   and stages it in a seqlock-protected pending slot. It returns 0 on
 *   acceptance and -1 for structurally malformed plans (stays inactive).
 * - `processBlock` (real-time thread) consumes the pending handoff with a
 *   bounded retry loop, advances a frame-exact clock, applies the active
 *   strategy, and performs a baseline restore on completion. Zero allocation,
 *   zero locks, zero I/O.
 * - `updateAutomation` (legacy offline driver) advances with an explicit time
 *   base and shares the same completion-restore semantics.
 * - A new request preempts a running transition (seqlock overwrite on the
 *   next real-time block); there is no separate cancel/stop API.
 */
class TransitionExecutor {
public:
    TransitionExecutor();
    ~TransitionExecutor() = default;

    /**
     * Control-plane entry point. Validates + sanitizes the full v2 plan and
     * stages it for lock-free real-time consumption.
     * @return 0 accepted; -1 structurally malformed (stays inactive, applies nothing).
     */
    int startTransition(const TransitionCommandC& command);

    /**
     * Real-time entry point (audio thread). Consumes any pending plan,
     * advances the transition clock by `numSamples` at `sampleRate`, applies
     * the active strategy, and restores captured deck baselines on completion.
     */
    void processBlock(uint32_t numSamples, uint32_t sampleRate, Mixer& mixer, DeckPlayer* deckA = nullptr, DeckPlayer* deckB = nullptr) noexcept;

    /** Legacy offline driver: advances with an explicit elapsed-time base. */
    void updateAutomation(double elapsedTimeSeconds, Mixer& mixer, DeckPlayer* deckA = nullptr, DeckPlayer* deckB = nullptr) noexcept;

    /**
     * Apply the completion restore (xfader → final endpoint; volume/EQ/filter/
     * stems → captured baselines; ramped tempo → 1.0) and deactivate.
     */
    void finishTransition(Mixer& mixer, DeckPlayer* deckA = nullptr, DeckPlayer* deckB = nullptr) noexcept;

    bool isTransitionActive() const noexcept;
    const TransitionCommandC& getActiveCommand() const noexcept { return activeCommand_; }
    double getNormalizedProgress() const noexcept { return progress_.load(); }

    void setCurveType(CrossfaderCurveType curve) noexcept { curveType_ = curve; }
    CrossfaderCurveType getCurveType() const noexcept { return curveType_; }

    BassSwapStrategy& getBassSwapStrategy() noexcept { return bassSwapStrategy_; }
    ClassicEqBlendStrategy& getClassicEqBlendStrategy() noexcept { return classicEqBlendStrategy_; }

    /** Fixed storage for per-deck parameter baselines captured at transition start. */
    struct DeckBaseline {
        float volume{1.0f};
        float low{0.0f};
        float mid{0.0f};
        float high{0.0f};
        float filter{0.0f};
        float vocal{1.0f};
        float drum{1.0f};
        float bass{1.0f};
        float other{1.0f};
    };

private:
    // Consumes the seqlock pending slot (idempotent when inactive); captures deck baselines.
    void consumePending(Mixer& mixer, DeckPlayer* deckA, DeckPlayer* deckB) noexcept;
    void captureBaselines(DeckPlayer* deckA, DeckPlayer* deckB) noexcept;
    void applyStrategy(double p, Mixer& mixer, DeckPlayer* deckA, DeckPlayer* deckB) noexcept;
    void deactivate() noexcept;

    std::atomic<bool> isActive_{false};
    std::atomic<double> progress_{0.0};
    TransitionCommandC activeCommand_{};
    CrossfaderCurveType curveType_{CrossfaderCurveType::EqualPower};

    // Lock-free pending handoff (seqlock: 0 = none, odd = writing, even > 0 = valid).
    std::atomic<uint32_t> pendingSeq_{0};
    TransitionCommandC pendingCmd_{};

    // Frame-exact real-time clock for the active transition.
    uint64_t elapsedFrames_{0};

    // Deck baselines for the completion restore (plain storage, no heap).
    DeckBaseline baselines_[2]{};

    // Pre-allocated strategy handlers (Zero RT heap allocation)
    PhraseCrossfadeStrategy phraseCrossfadeStrategy_;
    EqCrossfadeStrategy eqCrossfadeStrategy_;
    BassSwapStrategy bassSwapStrategy_;
    ClassicEqBlendStrategy classicEqBlendStrategy_;
    ITransitionStrategy* currentStrategy_{&phraseCrossfadeStrategy_};
};

} // namespace pulse::audio
