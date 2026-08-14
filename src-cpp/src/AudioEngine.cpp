#include "../include/AudioEngine.h"

namespace pulse::audio {

AudioEngine& AudioEngine::getInstance() {
    static AudioEngine instance;
    return instance;
}

AudioEngine::AudioEngine()
    : deckA_(std::make_unique<DeckPlayer>(0)),
      deckB_(std::make_unique<DeckPlayer>(1)),
      mixer_(std::make_unique<Mixer>()),
      transitionExecutor_(std::make_unique<TransitionExecutor>()) {}

int AudioEngine::initialize(const AudioEngineConfigC& config) {
    config_ = config;
    isInitialized_.store(true);
    return 0;
}

int AudioEngine::shutdown() {
    isInitialized_.store(false);
    return 0;
}

bool AudioEngine::loadTrack(uint8_t deckId, const std::string& filePath) {
    if (deckId == 0 && deckA_) {
        return deckA_->loadFile(filePath);
    } else if (deckId == 1 && deckB_) {
        return deckB_->loadFile(filePath);
    }
    return false;
}

bool AudioEngine::setPlaying(uint8_t deckId, bool isPlaying) {
    if (deckId == 0 && deckA_) {
        deckA_->setPlaying(isPlaying);
        return true;
    } else if (deckId == 1 && deckB_) {
        deckB_->setPlaying(isPlaying);
        return true;
    }
    return false;
}

DeckStateC AudioEngine::getDeckState(uint8_t deckId) const {
    if (deckId == 0 && deckA_) {
        return deckA_->getState();
    } else if (deckId == 1 && deckB_) {
        return deckB_->getState();
    }
    DeckStateC emptyState{};
    emptyState.deck_id = deckId;
    return emptyState;
}

int AudioEngine::executeTransition(const TransitionCommandC& command) {
    if (transitionExecutor_) {
        transitionExecutor_->startTransition(command);
        return 0;
    }
    return -1;
}

void TransitionExecutor::startTransition(const TransitionCommandC& command) {
    activeCommand_ = command;
    isActive_.store(true);
}

void TransitionExecutor::updateAutomation(double /*elapsedTimeSeconds*/) noexcept {
    // Parameter interpolation hook
}

bool TransitionExecutor::isTransitionActive() const noexcept {
    return isActive_.load();
}

} // namespace pulse::audio
