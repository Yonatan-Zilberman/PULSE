#include "../include/AudioEngine.h"
#include "../include/TempoStrategy.h"
#include <algorithm>
#include <cstring>
#include <cmath>

#if defined(__APPLE__)
#include <AudioUnit/AudioUnit.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#endif

namespace pulse::audio {

#if defined(__APPLE__)
static OSStatus coreAudioRenderCallback(
    void* inRefCon,
    AudioUnitRenderActionFlags* ioActionFlags,
    const AudioTimeStamp* inTimeStamp,
    UInt32 inBusNumber,
    UInt32 inNumberFrames,
    AudioBufferList* ioData) noexcept {
    (void)ioActionFlags;
    (void)inTimeStamp;
    (void)inBusNumber;

    if (!inRefCon || !ioData || ioData->mNumberBuffers == 0 || inNumberFrames == 0) {
        return noErr;
    }

    auto* engine = static_cast<AudioEngine*>(inRefCon);
    if (!engine->isRunning()) {
        for (UInt32 b = 0; b < ioData->mNumberBuffers; ++b) {
            std::memset(ioData->mBuffers[b].mData, 0, ioData->mBuffers[b].mDataByteSize);
        }
        return noErr;
    }

    if (ioData->mNumberBuffers >= 2) {
        float* channels[2] = {
            static_cast<float*>(ioData->mBuffers[0].mData),
            static_cast<float*>(ioData->mBuffers[1].mData)
        };
        juce::AudioIODeviceCallbackContext ctx{};
        engine->audioDeviceIOCallbackWithContext(nullptr, 0, channels, 2, static_cast<int>(inNumberFrames), ctx);

        for (UInt32 b = 2; b < ioData->mNumberBuffers; ++b) {
            std::memset(ioData->mBuffers[b].mData, 0, ioData->mBuffers[b].mDataByteSize);
        }
    } else {
        float* outInterleaved = static_cast<float*>(ioData->mBuffers[0].mData);
        UInt32 channels = ioData->mBuffers[0].mNumberChannels;
        if (channels == 0) channels = 2;
        engine->processAudioBlock(outInterleaved, inNumberFrames, channels);
    }

    return noErr;
}
#endif

AudioEngine& AudioEngine::getInstance() {
    static AudioEngine instance;
    return instance;
}

AudioEngine::AudioEngine()
    : deckA_(std::make_unique<DeckPlayer>(0)),
      deckB_(std::make_unique<DeckPlayer>(1)),
      mixer_(std::make_unique<Mixer>()),
      transitionExecutor_(std::make_unique<TransitionExecutor>()),
      deckABuffer_(32768, 0.0f),
      deckBBuffer_(32768, 0.0f),
      masterInterleavedScratch_(32768, 0.0f) {}

AudioEngine::~AudioEngine() {
    shutdown();
}

int AudioEngine::setupCoreAudioHardware() {
#if defined(__APPLE__)
    AudioComponentDescription desc{};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_DefaultOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    desc.componentFlags = 0;
    desc.componentFlagsMask = 0;

    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (!comp) {
        return -1;
    }

    AudioComponentInstance audioUnit = nullptr;
    OSStatus status = AudioComponentInstanceNew(comp, &audioUnit);
    if (status != noErr || !audioUnit) {
        return -1;
    }

    AudioStreamBasicDescription streamFormat{};
    streamFormat.mSampleRate = static_cast<Float64>(config_.sample_rate);
    streamFormat.mFormatID = kAudioFormatLinearPCM;
    streamFormat.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsNonInterleaved | kAudioFormatFlagsNativeEndian;
    streamFormat.mFramesPerPacket = 1;
    streamFormat.mChannelsPerFrame = config_.channel_count;
    streamFormat.mBitsPerChannel = 32;
    streamFormat.mBytesPerPacket = 4;
    streamFormat.mBytesPerFrame = 4;

    status = AudioUnitSetProperty(
        audioUnit,
        kAudioUnitProperty_StreamFormat,
        kAudioUnitScope_Input,
        0,
        &streamFormat,
        sizeof(streamFormat)
    );
    if (status != noErr) {
        streamFormat.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagsNativeEndian;
        streamFormat.mBytesPerPacket = 4 * config_.channel_count;
        streamFormat.mBytesPerFrame = 4 * config_.channel_count;
        status = AudioUnitSetProperty(
            audioUnit,
            kAudioUnitProperty_StreamFormat,
            kAudioUnitScope_Input,
            0,
            &streamFormat,
            sizeof(streamFormat)
        );
    }

    UInt32 bufferSize = config_.buffer_size;
    AudioUnitSetProperty(
        audioUnit,
        kAudioUnitProperty_MaximumFramesPerSlice,
        kAudioUnitScope_Global,
        0,
        &bufferSize,
        sizeof(bufferSize)
    );

    AURenderCallbackStruct renderCallback{};
    renderCallback.inputProc = coreAudioRenderCallback;
    renderCallback.inputProcRefCon = this;
    status = AudioUnitSetProperty(
        audioUnit,
        kAudioUnitProperty_SetRenderCallback,
        kAudioUnitScope_Input,
        0,
        &renderCallback,
        sizeof(renderCallback)
    );
    if (status != noErr) {
        AudioComponentInstanceDispose(audioUnit);
        return -1;
    }

    status = AudioUnitInitialize(audioUnit);
    if (status != noErr) {
        AudioComponentInstanceDispose(audioUnit);
        return -1;
    }

    audioUnit_ = audioUnit;
    hardwareDeviceActive_ = true;
    return 0;
#else
    return 0;
#endif
}

void AudioEngine::teardownCoreAudioHardware() noexcept {
#if defined(__APPLE__)
    if (audioUnit_) {
        auto unit = static_cast<AudioComponentInstance>(audioUnit_);
        AudioUnitUninitialize(unit);
        AudioComponentInstanceDispose(unit);
        audioUnit_ = nullptr;
    }
#endif
    hardwareDeviceActive_ = false;
}

int AudioEngine::initialize(const AudioEngineConfigC& config) {
    if (config.sample_rate < 22050 || config.sample_rate > 192000) {
        return -1;
    }
    if (config.buffer_size < 16 || config.buffer_size > 8192) {
        return -1;
    }
    if (config.channel_count == 0 || config.channel_count > 8) {
        return -1;
    }

    if (isRunning_.load(std::memory_order_acquire)) {
        stop();
    }

    // Keep an identical-config re-init cheap: reusing the live AudioUnit avoids
    // the CoreAudio create/dispose churn that can leave the render thread dead
    // within a process (observed with repeated init cycles in the app/tests).
    const bool sameConfig = (config_.sample_rate == config.sample_rate) &&
                            (config_.buffer_size == config.buffer_size) &&
                            (config_.channel_count == config.channel_count);
    const bool keepHardware = sameConfig && hardwareDeviceActive_ && (audioUnit_ != nullptr);
    if (!keepHardware) {
        teardownCoreAudioHardware();
    }

    config_ = config;

    size_t requiredBufferSize = std::max(32768u, config.buffer_size * 4) * std::max(2u, config.channel_count);
    deckABuffer_.assign(requiredBufferSize, 0.0f);
    deckBBuffer_.assign(requiredBufferSize, 0.0f);
    masterInterleavedScratch_.assign(requiredBufferSize, 0.0f);

    if (deckA_) deckA_->initCrossoverFilters(config.sample_rate);
    if (deckB_) deckB_->initCrossoverFilters(config.sample_rate);
    if (mixer_) {
        mixer_->init(config.sample_rate);
    }

    totalFramesProcessed_.store(0, std::memory_order_relaxed);
    underrunCount_.store(0, std::memory_order_relaxed);
    cpuLoad_.store(0.0f, std::memory_order_relaxed);

    if (!keepHardware) {
        setupCoreAudioHardware();
    }

    isInitialized_.store(true, std::memory_order_release);
    return 0;
}

int AudioEngine::start() {
    if (!isInitialized_.load(std::memory_order_acquire)) {
        return -1;
    }
    if (isRunning_.load(std::memory_order_acquire)) {
        return 0;
    }

    audioDeviceAboutToStart(nullptr);

#if defined(__APPLE__)
    if (hardwareDeviceActive_ && audioUnit_) {
        OSStatus status = AudioOutputUnitStart(static_cast<AudioComponentInstance>(audioUnit_));
        if (status != noErr) {
            return -1;
        }
    }
#endif

    isRunning_.store(true, std::memory_order_release);
    emitEvent(PULSE_EVT_ENGINE_STARTED, kEngineWideDeckId, 0, 0.0);
    return 0;
}

int AudioEngine::stop() {
    if (!isRunning_.load(std::memory_order_acquire)) {
        return 0;
    }

    isRunning_.store(false, std::memory_order_release);

#if defined(__APPLE__)
    if (hardwareDeviceActive_ && audioUnit_) {
        AudioOutputUnitStop(static_cast<AudioComponentInstance>(audioUnit_));
    }
#endif

    audioDeviceStopped();

    if (deckA_) deckA_->resetEq();
    if (deckB_) deckB_->resetEq();

    emitEvent(PULSE_EVT_ENGINE_STOPPED, kEngineWideDeckId, 0, 0.0);
    return 0;
}

int AudioEngine::shutdown() {
    if (isRunning_.load(std::memory_order_acquire)) {
        stop();  // emits PULSE_EVT_ENGINE_STOPPED
    }
    teardownCoreAudioHardware();
    isInitialized_.store(false, std::memory_order_release);
    emitEvent(PULSE_EVT_ENGINE_SHUTDOWN, kEngineWideDeckId, 0, 0.0);
    return 0;
}

AudioEngineStatsC AudioEngine::getStats() const noexcept {
    AudioEngineStatsC stats{};
    stats.sample_rate = config_.sample_rate;
    stats.buffer_size = config_.buffer_size;
    stats.channel_count = config_.channel_count;
    stats.is_initialized = isInitialized_.load(std::memory_order_relaxed) ? 1 : 0;
    stats.is_running = isRunning_.load(std::memory_order_relaxed) ? 1 : 0;
    stats.pad[0] = 0;
    stats.pad[1] = 0;
    stats.total_frames_processed = totalFramesProcessed_.load(std::memory_order_relaxed);
    stats.underrun_count = underrunCount_.load(std::memory_order_relaxed);
    stats.cpu_load = cpuLoad_.load(std::memory_order_relaxed);
    return stats;
}

void AudioEngine::emitEvent(uint32_t kind, uint8_t deckId, int32_t code, double detail) noexcept {
    AudioEventC event{};
    event.version = 1;
    event.kind = kind;
    event.deck_id = deckId;
    event.code = code;
    event.detail = detail;
    events_.push(event);
}

uint32_t AudioEngine::drainEvents(AudioEventC* out, uint32_t max, uint32_t* outDropped) noexcept {
    if (out == nullptr || max == 0) {
        return 0;
    }
    const uint32_t drained = events_.drain(out, max);
    if (outDropped != nullptr) {
        *outDropped = events_.dropped();
    }
    return drained;
}

void AudioEngine::recordUnderrun() noexcept {
    underrunCount_.fetch_add(1, std::memory_order_relaxed);
    emitEvent(PULSE_EVT_UNDERRUN, kEngineWideDeckId, 0, 0.0);
}

void AudioEngine::takeRtSample(RtEventSample& sample) const noexcept {
    sample.transitionActive = transitionExecutor_ ? transitionExecutor_->isTransitionActive() : false;
    for (uint32_t i = 0; i < 2; ++i) {
        const DeckPlayer* deck = (i == 0) ? deckA_.get() : deckB_.get();
        if (deck) {
            sample.deckState[i] = static_cast<uint8_t>(deck->getPlaybackState());
            sample.deckPosition[i] = deck->getPlaybackPosition();
            sample.deckDuration[i] = deck->getDuration();
        } else {
            sample.deckState[i] = static_cast<uint8_t>(DeckPlaybackState::Empty);
            sample.deckPosition[i] = 0.0;
            sample.deckDuration[i] = 0.0;
        }
    }
}

void AudioEngine::emitRtEventEdges(const RtEventSample& before, const RtEventSample& after) noexcept {
    if (!before.transitionActive && after.transitionActive) {
        emitEvent(PULSE_EVT_TRANSITION_STARTED, kEngineWideDeckId, 0, 0.0);
    } else if (before.transitionActive && !after.transitionActive) {
        emitEvent(PULSE_EVT_TRANSITION_COMPLETED, kEngineWideDeckId, 0, 0.0);
    }

    for (uint32_t i = 0; i < 2; ++i) {
        const bool wasPlaying = (before.deckState[i] == static_cast<uint8_t>(DeckPlaybackState::Playing));
        const bool nowStopped = (after.deckState[i] == static_cast<uint8_t>(DeckPlaybackState::Ready) ||
                                 after.deckState[i] == static_cast<uint8_t>(DeckPlaybackState::Paused));
        const bool reachedEnd = (after.deckDuration[i] > 0.0) &&
                                (after.deckPosition[i] >= after.deckDuration[i] - 1e-3);
        if (wasPlaying && nowStopped && reachedEnd) {
            emitEvent(PULSE_EVT_TRACK_ENDED, static_cast<uint8_t>(i), 0, after.deckPosition[i]);
        }
    }
}

bool AudioEngine::loadTrack(uint8_t deckId, const std::string& filePath) {
    if (deckId > 1) {
        return false;  // Invalid deck id: no event.
    }
    DeckPlayer* deck = (deckId == 0) ? deckA_.get() : deckB_.get();
    if (!deck) {
        return false;
    }
    const bool ok = deck->loadFile(filePath);
    if (ok) {
        emitEvent(PULSE_EVT_TRACK_LOADED, deckId, 0, getDeckState(deckId).duration_seconds);
    } else {
        emitEvent(PULSE_EVT_TRACK_LOAD_FAILED, deckId, -1, 0.0);
    }
    return ok;
}

bool AudioEngine::prepareDeck(uint8_t deckId, const std::string& filePath, double cueSeconds, double tempoRatio, bool preservePitch) {
    if (deckId > 1) {
        return false;  // Invalid deck id: no event.
    }
    DeckPlayer* deck = (deckId == 0) ? deckA_.get() : deckB_.get();
    if (!deck) {
        return false;
    }
    const bool ok = deck->prepareTrack(filePath, cueSeconds, tempoRatio, preservePitch);
    if (ok) {
        emitEvent(PULSE_EVT_TRACK_LOADED, deckId, 0, getDeckState(deckId).duration_seconds);
    } else {
        emitEvent(PULSE_EVT_TRACK_LOAD_FAILED, deckId, -1, 0.0);
    }
    return ok;
}

bool AudioEngine::playDeck(uint8_t deckId) {
    DeckPlayer* deck = getDeck(deckId);
    if (!deck) {
        return false;
    }
    const uint8_t before = static_cast<uint8_t>(deck->getPlaybackState());
    const bool ok = deck->play();
    const uint8_t after = static_cast<uint8_t>(deck->getPlaybackState());
    if (before != after) {
        emitEvent(PULSE_EVT_DECK_STATE_CHANGED, deckId, static_cast<int32_t>(after), deck->getPlaybackPosition());
    }
    return ok;
}

bool AudioEngine::pauseDeck(uint8_t deckId) {
    DeckPlayer* deck = getDeck(deckId);
    if (!deck) {
        return false;
    }
    const uint8_t before = static_cast<uint8_t>(deck->getPlaybackState());
    const bool ok = deck->pause();
    const uint8_t after = static_cast<uint8_t>(deck->getPlaybackState());
    if (before != after) {
        emitEvent(PULSE_EVT_DECK_STATE_CHANGED, deckId, static_cast<int32_t>(after), deck->getPlaybackPosition());
    }
    return ok;
}

bool AudioEngine::stopDeck(uint8_t deckId) {
    DeckPlayer* deck = getDeck(deckId);
    if (!deck) {
        return false;
    }
    const uint8_t before = static_cast<uint8_t>(deck->getPlaybackState());
    const bool ok = deck->stop();
    const uint8_t after = static_cast<uint8_t>(deck->getPlaybackState());
    if (before != after) {
        emitEvent(PULSE_EVT_DECK_STATE_CHANGED, deckId, static_cast<int32_t>(after), deck->getPlaybackPosition());
    }
    return ok;
}

bool AudioEngine::seekDeck(uint8_t deckId, double seconds) {
    DeckPlayer* deck = getDeck(deckId);
    if (!deck) {
        return false;
    }
    // Seek does not change the state enum: no DeckStateChanged event.
    deck->seek(seconds);
    return true;
}

bool AudioEngine::setPlaying(uint8_t deckId, bool isPlaying) {
    DeckPlayer* deck = getDeck(deckId);
    if (!deck) {
        return false;
    }
    const uint8_t before = static_cast<uint8_t>(deck->getPlaybackState());
    const bool ok = (isPlaying ? deck->play() : deck->pause());
    const uint8_t after = static_cast<uint8_t>(deck->getPlaybackState());
    if (before != after) {
        emitEvent(PULSE_EVT_DECK_STATE_CHANGED, deckId, static_cast<int32_t>(after), deck->getPlaybackPosition());
    }
    return ok;
}

bool AudioEngine::setDeckVolume(uint8_t deckId, float volume) {
    if (deckId == 0 && deckA_) {
        deckA_->setVolume(volume);
        return true;
    } else if (deckId == 1 && deckB_) {
        deckB_->setVolume(volume);
        return true;
    }
    return false;
}

bool AudioEngine::setDeckEq(uint8_t deckId, float low, float mid, float high) {
    if (deckId == 0 && deckA_) {
        deckA_->setEq(low, mid, high);
        return true;
    } else if (deckId == 1 && deckB_) {
        deckB_->setEq(low, mid, high);
        return true;
    }
    return false;
}

bool AudioEngine::setDeckFilter(uint8_t deckId, float filterVal) {
    if (deckId == 0 && deckA_) {
        deckA_->setFilter(filterVal);
        return true;
    } else if (deckId == 1 && deckB_) {
        deckB_->setFilter(filterVal);
        return true;
    }
    return false;
}

bool AudioEngine::setDeckTempoRatio(uint8_t deckId, double ratio) {
    if (deckId == 0 && deckA_) {
        deckA_->setTempoRatio(ratio);
        return true;
    } else if (deckId == 1 && deckB_) {
        deckB_->setTempoRatio(ratio);
        return true;
    }
    return false;
}

TempoStrategyDecision AudioEngine::matchTempo(uint8_t sourceDeckId, uint8_t destDeckId,
                                              TempoStrategyMode mode, double masterTargetBpm,
                                              double maxStretchPct, bool forceStretch) {
    DeckPlayer* src = getDeck(sourceDeckId);
    DeckPlayer* dst = getDeck(destDeckId);
    if (!src || !dst) return TempoStrategyDecision{};

    const DecodedAudio& a = src->getDecodedAudio();
    const DecodedAudio& b = dst->getDecodedAudio();
    double bpmA = (a.detectedBpm > 0.0) ? a.detectedBpm : 120.0;
    double bpmB = (b.detectedBpm > 0.0) ? b.detectedBpm : 120.0;

    TempoStrategyDecision decision = TempoStrategy::evaluate(
        bpmA, bpmB,
        a.tempoProfile.alternativeHypotheses,
        b.tempoProfile.alternativeHypotheses,
        mode, masterTargetBpm, maxStretchPct, forceStretch);

    // Apply matched ratios through the existing per-deck setters (RT-safe control plane).
    // When the bounded strategy rejects direct stretching (> threshold, not forceStretched), keep
    // native 1.0x playback so the flagged degradation is actually avoided; the caller then acts on
    // decision.rejectionReason / recommendedTransitionType to choose an alternative transition.
    if (decision.stretchExceededThreshold && !forceStretch) {
        src->setTempoRatio(1.0);
        dst->setTempoRatio(1.0);
    } else {
        src->setTempoRatio(decision.effectiveDeckATempoRatio);
        dst->setTempoRatio(decision.effectiveDeckBTempoRatio);
    }
    return decision;
}

bool AudioEngine::setDeckPitchPreservation(uint8_t deckId, bool enabled) {
    if (deckId == 0 && deckA_) {
        deckA_->setPitchPreservation(enabled);
        return true;
    } else if (deckId == 1 && deckB_) {
        deckB_->setPitchPreservation(enabled);
        return true;
    }
    return false;
}

bool AudioEngine::setDeckStemLevels(uint8_t deckId, float vocal, float drum, float bass, float other) {
    if (deckId == 0 && deckA_) {
        deckA_->setStemLevels(vocal, drum, bass, other);
        return true;
    } else if (deckId == 1 && deckB_) {
        deckB_->setStemLevels(vocal, drum, bass, other);
        return true;
    }
    return false;
}

DeckStateC AudioEngine::getDeckState(uint8_t deckId) const noexcept {
    if (deckId == 0 && deckA_) {
        return deckA_->getState();
    } else if (deckId == 1 && deckB_) {
        return deckB_->getState();
    }
    DeckStateC emptyState{};
    emptyState.deck_id = deckId;
    return emptyState;
}

bool AudioEngine::isDeckPlaying(uint8_t deckId) const noexcept {
    if (deckId == 0 && deckA_) {
        return deckA_->isPlaying();
    } else if (deckId == 1 && deckB_) {
        return deckB_->isPlaying();
    }
    return false;
}

double AudioEngine::getDeckPosition(uint8_t deckId) const noexcept {
    if (deckId == 0 && deckA_) {
        return deckA_->getPlaybackPosition();
    } else if (deckId == 1 && deckB_) {
        return deckB_->getPlaybackPosition();
    }
    return 0.0;
}

double AudioEngine::getDeckDuration(uint8_t deckId) const noexcept {
    if (deckId == 0 && deckA_) {
        return deckA_->getDuration();
    } else if (deckId == 1 && deckB_) {
        return deckB_->getDuration();
    }
    return 0.0;
}

int AudioEngine::executeTransition(const TransitionCommandC& command) {
    if (!transitionExecutor_) {
        return -1;
    }
    const int result = transitionExecutor_->startTransition(command);
    if (result != 0) {
        emitEvent(PULSE_EVT_TRANSITION_REJECTED, kEngineWideDeckId, -1, 0.0);
    }
    return result;
}

void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* /*device*/) {
    if (deckA_) deckA_->resetEq();
    if (deckB_) deckB_->resetEq();
    if (mixer_) mixer_->reset();
}

void AudioEngine::audioDeviceStopped() {
    if (deckA_) deckA_->resetEq();
    if (deckB_) deckB_->resetEq();
    if (mixer_) mixer_->reset();
}

void AudioEngine::audioDeviceIOCallbackWithContext(
    const float* const* inputChannelData,
    int numInputChannels,
    float* const* outputChannelData,
    int numOutputChannels,
    int numSamples,
    const juce::AudioIODeviceCallbackContext& context) noexcept {
    (void)inputChannelData;
    (void)numInputChannels;
    (void)context;

    if (!outputChannelData || numSamples <= 0 || numOutputChannels <= 0) {
        return;
    }

    uint32_t samples = static_cast<uint32_t>(numSamples);
    size_t samplesNeeded = static_cast<size_t>(samples) * 2;

    if (deckABuffer_.size() < samplesNeeded || deckBBuffer_.size() < samplesNeeded || masterInterleavedScratch_.size() < samplesNeeded) {
        for (int c = 0; c < numOutputChannels; ++c) {
            if (outputChannelData[c]) {
                std::memset(outputChannelData[c], 0, numSamples * sizeof(float));
            }
        }
        recordUnderrun();
        return;
    }

    // Event edge-detection sample (before): no DSP effect.
    RtEventSample sampleBefore;
    takeRtSample(sampleBefore);

    // Transition automation (real-time): consumes pending full-plan handoff, advances the
    // transition clock for the block about to be rendered, applies the active strategy.
    if (transitionExecutor_ && mixer_) {
        transitionExecutor_->processBlock(samples, config_.sample_rate, *mixer_, deckA_.get(), deckB_.get());
    }

    // Process Deck A & B
    deckA_->processBlock(deckABuffer_.data(), samples, 2);
    deckB_->processBlock(deckBBuffer_.data(), samples, 2);

    // Event edge-detection sample (after): emits TransitionStarted/Completed, TrackEnded.
    RtEventSample sampleAfter;
    takeRtSample(sampleAfter);
    emitRtEventEdges(sampleBefore, sampleAfter);

    // Sum via mixer into master interleaved buffer
    mixer_->mix(deckABuffer_.data(), deckBBuffer_.data(), masterInterleavedScratch_.data(), samples, 2);

    // Route to output channels
    if (numOutputChannels == 1) {
        float* out = outputChannelData[0];
        if (out) {
            for (uint32_t s = 0; s < samples; ++s) {
                out[s] = 0.5f * (masterInterleavedScratch_[s * 2] + masterInterleavedScratch_[s * 2 + 1]);
            }
        }
    } else if (numOutputChannels >= 2) {
        float* outL = outputChannelData[0];
        float* outR = outputChannelData[1];
        if (outL && outR) {
            for (uint32_t s = 0; s < samples; ++s) {
                outL[s] = masterInterleavedScratch_[s * 2];
                outR[s] = masterInterleavedScratch_[s * 2 + 1];
            }
        }
        for (int c = 2; c < numOutputChannels; ++c) {
            if (outputChannelData[c]) {
                std::memset(outputChannelData[c], 0, numSamples * sizeof(float));
            }
        }
    }

    totalFramesProcessed_.fetch_add(samples, std::memory_order_relaxed);
}

void AudioEngine::processAudioBlock(float* outMasterBuffer, uint32_t numSamples, uint32_t numChannels) noexcept {
    if (!outMasterBuffer || numSamples == 0 || numChannels == 0) return;

    size_t samplesNeeded = static_cast<size_t>(numSamples) * std::max(2u, numChannels);
    if (deckABuffer_.size() < samplesNeeded || deckBBuffer_.size() < samplesNeeded) {
        std::memset(outMasterBuffer, 0, numSamples * numChannels * sizeof(float));
        recordUnderrun();
        return;
    }

    // Event edge-detection sample (before): no DSP effect.
    RtEventSample sampleBefore;
    takeRtSample(sampleBefore);

    // Transition automation (real-time): runs before deck processing so the block being
    // rendered already sees the automated parameters (deterministic block-granular clock).
    if (transitionExecutor_ && mixer_) {
        transitionExecutor_->processBlock(numSamples, config_.sample_rate, *mixer_, deckA_.get(), deckB_.get());
    }

    deckA_->processBlock(deckABuffer_.data(), numSamples, numChannels);
    deckB_->processBlock(deckBBuffer_.data(), numSamples, numChannels);

    // Event edge-detection sample (after): emits TransitionStarted/Completed, TrackEnded.
    RtEventSample sampleAfter;
    takeRtSample(sampleAfter);
    emitRtEventEdges(sampleBefore, sampleAfter);

    mixer_->mix(deckABuffer_.data(), deckBBuffer_.data(), outMasterBuffer, numSamples, numChannels);

    totalFramesProcessed_.fetch_add(numSamples, std::memory_order_relaxed);
}

} // namespace pulse::audio

