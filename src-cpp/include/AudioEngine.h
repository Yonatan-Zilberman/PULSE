#pragma once

#include "AudioBridgeTypes.h"
#include "DeckPlayer.h"
#include "Mixer.h"
#include "TransitionExecutor.h"
#include <memory>
#include <atomic>

namespace pulse::audio {

/**
 * @brief Master Audio Engine orchestrating CoreAudio I/O, Deck Players, and Mixer.
 */
class AudioEngine {
public:
    static AudioEngine& getInstance();

    int initialize(const AudioEngineConfigC& config);
    int shutdown();

    bool loadTrack(uint8_t deckId, const std::string& filePath);
    bool setPlaying(uint8_t deckId, bool isPlaying);
    DeckStateC getDeckState(uint8_t deckId) const;

    int executeTransition(const TransitionCommandC& command);

    bool isInitialized() const noexcept { return isInitialized_.load(); }

private:
    AudioEngine();
    ~AudioEngine() = default;

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    std::atomic<bool> isInitialized_{false};
    AudioEngineConfigC config_{48000, 512, 2};

    std::unique_ptr<DeckPlayer> deckA_;
    std::unique_ptr<DeckPlayer> deckB_;
    std::unique_ptr<Mixer> mixer_;
    std::unique_ptr<TransitionExecutor> transitionExecutor_;
};

} // namespace pulse::audio
