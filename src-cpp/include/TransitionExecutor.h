#pragma once

#include "AudioBridgeTypes.h"
#include <atomic>

namespace pulse::audio {

/**
 * @brief Deterministic Transition Automation Executor.
 */
class TransitionExecutor {
public:
    TransitionExecutor() = default;
    ~TransitionExecutor() = default;

    void startTransition(const TransitionCommandC& command);
    void updateAutomation(double elapsedTimeSeconds) noexcept;
    bool isTransitionActive() const noexcept;

private:
    std::atomic<bool> isActive_{false};
    TransitionCommandC activeCommand_{};
};

} // namespace pulse::audio
