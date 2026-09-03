#include "../include/AudioEngine.h"
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
    teardownCoreAudioHardware();

    config_ = config;

    size_t requiredBufferSize = std::max(32768u, config.buffer_size * 4) * std::max(2u, config.channel_count);
    deckABuffer_.assign(requiredBufferSize, 0.0f);
    deckBBuffer_.assign(requiredBufferSize, 0.0f);
    masterInterleavedScratch_.assign(requiredBufferSize, 0.0f);

    if (deckA_) deckA_->initCrossoverFilters(config.sample_rate);
    if (deckB_) deckB_->initCrossoverFilters(config.sample_rate);

    totalFramesProcessed_.store(0, std::memory_order_relaxed);
    underrunCount_.store(0, std::memory_order_relaxed);
    cpuLoad_.store(0.0f, std::memory_order_relaxed);

    setupCoreAudioHardware();

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

    return 0;
}

int AudioEngine::shutdown() {
    if (isRunning_.load(std::memory_order_acquire)) {
        stop();
    }
    teardownCoreAudioHardware();
    isInitialized_.store(false, std::memory_order_release);
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

bool AudioEngine::loadTrack(uint8_t deckId, const std::string& filePath) {
    if (deckId == 0 && deckA_) {
        return deckA_->loadFile(filePath);
    } else if (deckId == 1 && deckB_) {
        return deckB_->loadFile(filePath);
    }
    return false;
}

bool AudioEngine::prepareDeck(uint8_t deckId, const std::string& filePath, double cueSeconds, double tempoRatio, bool preservePitch) {
    if (deckId == 0 && deckA_) {
        return deckA_->prepareTrack(filePath, cueSeconds, tempoRatio, preservePitch);
    } else if (deckId == 1 && deckB_) {
        return deckB_->prepareTrack(filePath, cueSeconds, tempoRatio, preservePitch);
    }
    return false;
}

bool AudioEngine::playDeck(uint8_t deckId) {
    if (deckId == 0 && deckA_) {
        return deckA_->play();
    } else if (deckId == 1 && deckB_) {
        return deckB_->play();
    }
    return false;
}

bool AudioEngine::pauseDeck(uint8_t deckId) {
    if (deckId == 0 && deckA_) {
        return deckA_->pause();
    } else if (deckId == 1 && deckB_) {
        return deckB_->pause();
    }
    return false;
}

bool AudioEngine::stopDeck(uint8_t deckId) {
    if (deckId == 0 && deckA_) {
        return deckA_->stop();
    } else if (deckId == 1 && deckB_) {
        return deckB_->stop();
    }
    return false;
}

bool AudioEngine::seekDeck(uint8_t deckId, double seconds) {
    if (deckId == 0 && deckA_) {
        deckA_->seek(seconds);
        return true;
    } else if (deckId == 1 && deckB_) {
        deckB_->seek(seconds);
        return true;
    }
    return false;
}

bool AudioEngine::setPlaying(uint8_t deckId, bool isPlaying) {
    if (deckId == 0 && deckA_) {
        return isPlaying ? deckA_->play() : deckA_->pause();
    } else if (deckId == 1 && deckB_) {
        return isPlaying ? deckB_->play() : deckB_->pause();
    }
    return false;
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
    if (transitionExecutor_) {
        transitionExecutor_->startTransition(command);
        return 0;
    }
    return -1;
}

void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* /*device*/) {
    if (deckA_) deckA_->resetEq();
    if (deckB_) deckB_->resetEq();
}

void AudioEngine::audioDeviceStopped() {
    if (deckA_) deckA_->resetEq();
    if (deckB_) deckB_->resetEq();
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

    // Process Deck A & B
    deckA_->processBlock(deckABuffer_.data(), samples, 2);
    deckB_->processBlock(deckBBuffer_.data(), samples, 2);

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

    deckA_->processBlock(deckABuffer_.data(), numSamples, numChannels);
    deckB_->processBlock(deckBBuffer_.data(), numSamples, numChannels);

    mixer_->mix(deckABuffer_.data(), deckBBuffer_.data(), outMasterBuffer, numSamples, numChannels);

    totalFramesProcessed_.fetch_add(numSamples, std::memory_order_relaxed);
}

} // namespace pulse::audio

