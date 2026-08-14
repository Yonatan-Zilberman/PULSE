#pragma once

#include "AudioBridgeTypes.h"
#include "Mixer.h"
#include <atomic>

namespace pulse::audio {

class DeckPlayer;

/**
 * @brief Deterministic Transition Automation Executor.
 */
class TransitionExecutor {
public:
    TransitionExecutor() = default;
    ~TransitionExecutor() = default;

    void startTransition(const TransitionCommandC& command);
    void updateAutomation(double elapsedTimeSeconds, Mixer& mixer) noexcept;
    bool isTransitionActive() const noexcept;
    const TransitionCommandC& getActiveCommand() const noexcept { return activeCommand_; }

private:
    std::atomic<bool> isActive_{false};
    TransitionCommandC activeCommand_{};
};

} // namespace pulse::audio
