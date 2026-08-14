#include "../include/TransitionExecutor.h"
#include <algorithm>

namespace pulse::audio {

void PhraseCrossfadeStrategy::update(double normalizedProgress, const TransitionCommandC& command, Mixer& mixer) noexcept {
    float startX = (command.source_deck == 0) ? -1.0f : 1.0f;
    float endX = (command.destination_deck == 1) ? 1.0f : -1.0f;

    double p = std::clamp(normalizedProgress, 0.0, 1.0);
    float currentX = static_cast<float>(startX + (endX - startX) * p);
    mixer.setCrossfader(currentX);
}

TransitionExecutor::TransitionExecutor() = default;

void TransitionExecutor::startTransition(const TransitionCommandC& command) {
    activeCommand_ = command;
    progress_.store(0.0);
    phraseCrossfadeStrategy_.reset();
    isActive_.store(true);
}

void TransitionExecutor::updateAutomation(double elapsedTimeSeconds, Mixer& mixer) noexcept {
    if (!isActive_.load() || activeCommand_.duration_seconds <= 0.0) return;

    double p = std::clamp(elapsedTimeSeconds / activeCommand_.duration_seconds, 0.0, 1.0);
    progress_.store(p);

    phraseCrossfadeStrategy_.update(p, activeCommand_, mixer);

    if (p >= 1.0) {
        isActive_.store(false);
    }
}

bool TransitionExecutor::isTransitionActive() const noexcept {
    return isActive_.load();
}

} // namespace pulse::audio
