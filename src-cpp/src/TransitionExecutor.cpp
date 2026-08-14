#include "../include/TransitionExecutor.h"
#include "../include/DeckPlayer.h"
#include <algorithm>

namespace pulse::audio {

void PhraseCrossfadeStrategy::update(double normalizedProgress, const TransitionCommandC& command, Mixer& mixer, DeckPlayer* deckA, DeckPlayer* deckB) noexcept {
    float startX = (command.source_deck == 0) ? -1.0f : 1.0f;
    float endX = (command.destination_deck == 1) ? 1.0f : -1.0f;

    double p = std::clamp(normalizedProgress, 0.0, 1.0);
    float currentX = static_cast<float>(startX + (endX - startX) * p);
    mixer.setCrossfader(currentX);

    if (deckA) deckA->setEq(0.0f, 0.0f, 0.0f);
    if (deckB) deckB->setEq(0.0f, 0.0f, 0.0f);
}

void EqCrossfadeStrategy::update(double normalizedProgress, const TransitionCommandC& command, Mixer& mixer, DeckPlayer* deckA, DeckPlayer* deckB) noexcept {
    float startX = (command.source_deck == 0) ? -1.0f : 1.0f;
    float endX = (command.destination_deck == 1) ? 1.0f : -1.0f;

    double p = std::clamp(normalizedProgress, 0.0, 1.0);
    float currentX = static_cast<float>(startX + (endX - startX) * p);
    mixer.setCrossfader(currentX);

    // Monotonic low-frequency crossover ramp
    // Source deck low: 0.0 (0 dB) -> -1.0 (kill)
    // Dest deck low: -1.0 (kill) -> 0.0 (0 dB)
    float lowSrc = -static_cast<float>(p);
    float lowDst = static_cast<float>(p - 1.0);

    DeckPlayer* src = (command.source_deck == 0) ? deckA : deckB;
    DeckPlayer* dst = (command.destination_deck == 1) ? deckB : deckA;

    if (src) src->setEq(lowSrc, 0.0f, 0.0f);
    if (dst) dst->setEq(lowDst, 0.0f, 0.0f);
}

void BassSwapStrategy::update(double normalizedProgress, const TransitionCommandC& command, Mixer& mixer, DeckPlayer* deckA, DeckPlayer* deckB) noexcept {
    float startX = (command.source_deck == 0) ? -1.0f : 1.0f;
    float endX = (command.destination_deck == 1) ? 1.0f : -1.0f;

    double p = std::clamp(normalizedProgress, 0.0, 1.0);
    float currentX = static_cast<float>(startX + (endX - startX) * p);
    mixer.setCrossfader(currentX);

    double halfWindow = swapWindow_ * 0.5;
    double swapStart = swapPoint_ - halfWindow;
    double swapEnd = swapPoint_ + halfWindow;

    float gainSrcLow = 1.0f;
    float gainDstLow = 0.0f;

    if (p <= swapStart) {
        gainSrcLow = 1.0f;
        gainDstLow = 0.0f;
    } else if (p >= swapEnd) {
        gainSrcLow = 0.0f;
        gainDstLow = 1.0f;
    } else {
        // Smooth Hermite smoothstep swap curve
        double u = (p - swapStart) / swapWindow_;
        float s = static_cast<float>(u * u * (3.0 - 2.0 * u));
        gainSrcLow = 1.0f - s;
        gainDstLow = s;
    }

    // Map linear gains [0.0, 1.0] to EQ parameters [-1.0, 0.0]
    // where gain = 1.0 + eqVal  =>  eqVal = gain - 1.0
    float lowEqSrc = std::clamp(gainSrcLow - 1.0f, -1.0f, 0.0f);
    float lowEqDst = std::clamp(gainDstLow - 1.0f, -1.0f, 0.0f);

    DeckPlayer* src = (command.source_deck == 0) ? deckA : deckB;
    DeckPlayer* dst = (command.destination_deck == 1) ? deckB : deckA;

    if (src) src->setEq(lowEqSrc, 0.0f, 0.0f);
    if (dst) dst->setEq(lowEqDst, 0.0f, 0.0f);
}

TransitionExecutor::TransitionExecutor() = default;

void TransitionExecutor::startTransition(const TransitionCommandC& command) {
    activeCommand_ = command;
    progress_.store(0.0);

    switch (static_cast<TransitionStrategyType>(command.transition_type)) {
        case TransitionStrategyType::EqCrossfade:
            currentStrategy_ = &eqCrossfadeStrategy_;
            break;
        case TransitionStrategyType::BassSwap:
            currentStrategy_ = &bassSwapStrategy_;
            break;
        case TransitionStrategyType::PhraseCrossfade:
        default:
            currentStrategy_ = &phraseCrossfadeStrategy_;
            break;
    }

    if (currentStrategy_) {
        currentStrategy_->reset();
    }
    isActive_.store(true);
}

void TransitionExecutor::updateAutomation(double elapsedTimeSeconds, Mixer& mixer, DeckPlayer* deckA, DeckPlayer* deckB) noexcept {
    if (!isActive_.load() || activeCommand_.duration_seconds <= 0.0) return;

    double p = std::clamp(elapsedTimeSeconds / activeCommand_.duration_seconds, 0.0, 1.0);
    progress_.store(p);

    if (currentStrategy_) {
        currentStrategy_->update(p, activeCommand_, mixer, deckA, deckB);
    }

    if (p >= 1.0) {
        isActive_.store(false);
    }
}

bool TransitionExecutor::isTransitionActive() const noexcept {
    return isActive_.load();
}

} // namespace pulse::audio
