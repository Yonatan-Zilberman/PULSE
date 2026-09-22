#pragma once

#include "AudioBridgeTypes.h"
#include "DeckPlayer.h"
#include "EventQueue.h"
#include "Mixer.h"
#include "TransitionExecutor.h"
#include "TempoStrategy.h"
#include <memory>
#include <atomic>
#include <vector>
#include <string>

namespace juce {

class AudioIODevice;

struct AudioIODeviceCallbackContext {
    const double* inputChannelLevels{nullptr};
    const double* outputChannelLevels{nullptr};
};

/**
 * @brief JUCE 8 compatible Audio I/O Device Callback interface.
 */
class AudioIODeviceCallback {
public:
    virtual ~AudioIODeviceCallback() = default;
    virtual void audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                                 int numInputChannels,
                                                 float* const* outputChannelData,
                                                 int numOutputChannels,
                                                 int numSamples,
                                                 const AudioIODeviceCallbackContext& context) noexcept = 0;
    virtual void audioDeviceAboutToStart(AudioIODevice* device) = 0;
    virtual void audioDeviceStopped() = 0;
};

} // namespace juce

namespace pulse::audio {

/**
 * @brief Master Audio Engine orchestrating CoreAudio hardware I/O, Deck Players, and Mixer.
 *
 * REAL-TIME SAFETY CONTRACT:
 * - audioDeviceIOCallbackWithContext() and processAudioBlock() execute on the OS real-time thread.
 * - Zero dynamic heap memory allocations (new/malloc).
 * - Zero blocking synchronization primitives (mutex/condition_variable).
 * - Zero system I/O (file, network, console logging).
 * - emitEvent() is real-time safe: it builds an event on the stack and pushes it into
 *   a fixed 512-slot lock-free ring (bounded CAS, drop-on-full). The stack is allowed
 *   on the real-time thread; the ring is preallocated at construction.
 */
class AudioEngine : public juce::AudioIODeviceCallback {
public:
    static AudioEngine& getInstance();

    // Production Lifecycle Management
    int initialize(const AudioEngineConfigC& config);
    int start();
    int stop();
    int shutdown();

    bool isRunning() const noexcept { return isRunning_.load(std::memory_order_relaxed); }
    bool isInitialized() const noexcept { return isInitialized_.load(std::memory_order_relaxed); }
    const AudioEngineConfigC& getConfig() const noexcept { return config_; }
    AudioEngineStatsC getStats() const noexcept;

    // Deck & Mixer Accessors
    DeckPlayer* getDeck(uint8_t deckId) noexcept {
        return (deckId == 0) ? deckA_.get() : ((deckId == 1) ? deckB_.get() : nullptr);
    }
    const DeckPlayer* getDeck(uint8_t deckId) const noexcept {
        return (deckId == 0) ? deckA_.get() : ((deckId == 1) ? deckB_.get() : nullptr);
    }
    Mixer* getMixer() noexcept { return mixer_.get(); }
    const Mixer* getMixer() const noexcept { return mixer_.get(); }
    TransitionExecutor* getTransitionExecutor() noexcept { return transitionExecutor_.get(); }
    const TransitionExecutor* getTransitionExecutor() const noexcept { return transitionExecutor_.get(); }

    // C ABI Support & Dual-Deck Playback Controls
    bool loadTrack(uint8_t deckId, const std::string& filePath);
    bool prepareDeck(uint8_t deckId, const std::string& filePath, double cueSeconds = 0.0, double tempoRatio = 1.0, bool preservePitch = true);
    bool playDeck(uint8_t deckId);
    bool pauseDeck(uint8_t deckId);
    bool stopDeck(uint8_t deckId);
    bool seekDeck(uint8_t deckId, double seconds);
    bool setPlaying(uint8_t deckId, bool isPlaying);
    bool setDeckVolume(uint8_t deckId, float volume);
    bool setDeckEq(uint8_t deckId, float low, float mid, float high);
    bool setDeckFilter(uint8_t deckId, float filterVal);
    bool setDeckTempoRatio(uint8_t deckId, double ratio);

    // Tempo Matching (Control Plane — NOT real-time audio thread)
    // Computes a bounded tempo-match decision from both decks' detected BPMs and applies the
    // matched ratios to each deck via setTempoRatio. Returns the decision for telemetry.
    // Safe to call from prepare/FFI/CLI control plane; never invoke from processAudioBlock.
    TempoStrategyDecision matchTempo(uint8_t sourceDeckId, uint8_t destDeckId,
                                     TempoStrategyMode mode = TempoStrategyMode::Source,
                                     double masterTargetBpm = 0.0,
                                     double maxStretchPct = 6.0,
                                     bool forceStretch = false);

    bool setDeckPitchPreservation(uint8_t deckId, bool enabled);
    bool setDeckStemLevels(uint8_t deckId, float vocal, float drum, float bass, float other);
    DeckStateC getDeckState(uint8_t deckId) const noexcept;
    bool isDeckPlaying(uint8_t deckId) const noexcept;
    double getDeckPosition(uint8_t deckId) const noexcept;
    double getDeckDuration(uint8_t deckId) const noexcept;
    int executeTransition(const TransitionCommandC& command);

    // Engine -> Application Event Delivery
    // Deck id 255 = engine/transition-wide (not attached to a single deck).
    static constexpr uint8_t kEngineWideDeckId = 255;

    /**
     * Emit one event into the internal lock-free queue (real-time safe; may drop
     * on overflow — see EventQueue). Safe from both the control plane and the
     * real-time thread.
     */
    void emitEvent(uint32_t kind, uint8_t deckId, int32_t code, double detail) noexcept;

    /**
     * Drain up to `max` events into a caller-allocated buffer (single consumer =
     * application layer). `outDropped` (nullable) receives the cumulative
     * dropped-event count.
     * @return Number of events copied (0..max).
     */
    uint32_t drainEvents(AudioEventC* out, uint32_t max, uint32_t* outDropped) noexcept;

    uint32_t droppedEvents() const noexcept { return events_.dropped(); }

    // AudioIODeviceCallback Implementation (Real-Time thread)
    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                         int numInputChannels,
                                         float* const* outputChannelData,
                                         int numOutputChannels,
                                         int numSamples,
                                         const juce::AudioIODeviceCallbackContext& context) noexcept override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

    // Offline / Testing Callback Path (Deterministic Real-Time DSP)
    void processAudioBlock(float* outMasterBuffer, uint32_t numSamples, uint32_t numChannels) noexcept;

    // Diagnostics / Underrun telemetry (emits PULSE_EVT_UNDERRUN)
    void recordUnderrun() noexcept;

private:
    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;
    AudioEngine(AudioEngine&&) = delete;
    AudioEngine& operator=(AudioEngine&&) = delete;

    int setupCoreAudioHardware();
    void teardownCoreAudioHardware() noexcept;

    // Real-time event edge-detection (sampled around existing DSP calls only —
    // no change to DSP ordering). No allocation: fixed-size value structs.
    struct RtEventSample {
        bool transitionActive{false};
        uint8_t deckState[2]{0, 0};
        double deckPosition[2]{0.0, 0.0};
        double deckDuration[2]{0.0, 0.0};
    };
    void takeRtSample(RtEventSample& sample) const noexcept;
    void emitRtEventEdges(const RtEventSample& before, const RtEventSample& after) noexcept;

    std::atomic<bool> isInitialized_{false};
    std::atomic<bool> isRunning_{false};
    AudioEngineConfigC config_{48000, 512, 2};

    std::unique_ptr<DeckPlayer> deckA_;
    std::unique_ptr<DeckPlayer> deckB_;
    std::unique_ptr<Mixer> mixer_;
    std::unique_ptr<TransitionExecutor> transitionExecutor_;

    // Fixed-capacity lock-free event queue (preallocated; drop-on-full).
    EventQueue events_;


    // Pre-allocated scratch buffers for real-time safe mixing
    std::vector<float> deckABuffer_;
    std::vector<float> deckBBuffer_;
    std::vector<float> masterInterleavedScratch_;

    // Real-Time Health & Operational Telemetry
    std::atomic<uint64_t> totalFramesProcessed_{0};
    std::atomic<uint32_t> underrunCount_{0};
    std::atomic<float> cpuLoad_{0.0f};

    // Hardware AudioUnit Handle (macOS CoreAudio)
    void* audioUnit_{nullptr};
    bool hardwareDeviceActive_{false};
};

} // namespace pulse::audio

