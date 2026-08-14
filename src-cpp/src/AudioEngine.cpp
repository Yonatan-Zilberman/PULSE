#include "../include/AudioEngine.h"
#include <algorithm>

namespace pulse::audio {

AudioEngine& AudioEngine::getInstance() {
    static AudioEngine instance;
    return instance;
}

AudioEngine::AudioEngine()
    : deckA_(std::make_unique<DeckPlayer>(0)),
      deckB_(std::make_unique<DeckPlayer>(1)),
      mixer_(std::make_unique<Mixer>()),
      transitionExecutor_(std::make_unique<TransitionExecutor>()),
      deckABuffer_(4096 * 2, 0.0f),
      deckBBuffer_(4096 * 2, 0.0f) {}

int AudioEngine::initialize(const AudioEngineConfigC& config) {
    config_ = config;
    size_t requiredBufferSize = std::max(4096u, config.buffer_size) * std::max(2u, config.channel_count);
    deckABuffer_.assign(requiredBufferSize, 0.0f);
    deckBBuffer_.assign(requiredBufferSize, 0.0f);
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

void AudioEngine::processAudioBlock(float* outMasterBuffer, uint32_t numSamples, uint32_t numChannels) noexcept {
    if (!outMasterBuffer || numSamples == 0 || numChannels == 0) return;

    size_t samplesNeeded = numSamples * numChannels;
    if (deckABuffer_.size() < samplesNeeded || deckBBuffer_.size() < samplesNeeded) return;

    deckA_->processBlock(deckABuffer_.data(), numSamples, numChannels);
    deckB_->processBlock(deckBBuffer_.data(), numSamples, numChannels);

    mixer_->mix(deckABuffer_.data(), deckBBuffer_.data(), outMasterBuffer, numSamples, numChannels);
}

void TransitionExecutor::startTransition(const TransitionCommandC& command) {
    activeCommand_ = command;
    isActive_.store(true);
}

void TransitionExecutor::updateAutomation(double elapsedTimeSeconds, Mixer& mixer) noexcept {
    if (!isActive_.load() || activeCommand_.duration_seconds <= 0.0) return;

    double progress = std::clamp(elapsedTimeSeconds / activeCommand_.duration_seconds, 0.0, 1.0);

    float startX = (activeCommand_.source_deck == 0) ? -1.0f : 1.0f;
    float endX = (activeCommand_.destination_deck == 1) ? 1.0f : -1.0f;

    float currentX = static_cast<float>(startX + (endX - startX) * progress);
    mixer.setCrossfader(currentX);

    if (progress >= 1.0) {
        isActive_.store(false);
    }
}

bool TransitionExecutor::isTransitionActive() const noexcept {
    return isActive_.load();
}

} // namespace pulse::audio
