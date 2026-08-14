#pragma once

#include "AudioBridgeTypes.h"
#include "DeckPlayer.h"
#include "Mixer.h"
#include "TransitionExecutor.h"
#include <memory>
#include <atomic>
#include <vector>

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
    const AudioEngineConfigC& getConfig() const noexcept { return config_; }

    DeckPlayer* getDeck(uint8_t deckId) {
        return (deckId == 0) ? deckA_.get() : ((deckId == 1) ? deckB_.get() : nullptr);
    }
    Mixer* getMixer() { return mixer_.get(); }
    TransitionExecutor* getTransitionExecutor() { return transitionExecutor_.get(); }

    /**
     * @brief Process an audio block offline or in real-time callback.
     */
    void processAudioBlock(float* outMasterBuffer, uint32_t numSamples, uint32_t numChannels) noexcept;

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

    // Pre-allocated scratch buffers for real-time safe mixing
    std::vector<float> deckABuffer_;
    std::vector<float> deckBBuffer_;
};

} // namespace pulse::audio
