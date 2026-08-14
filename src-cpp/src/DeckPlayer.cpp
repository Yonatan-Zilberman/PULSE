#include "../include/DeckPlayer.h"
#include <cstring>
#include <algorithm>
#include <cmath>

namespace pulse::audio {

DeckPlayer::DeckPlayer(uint8_t deckId)
    : deckId_(deckId),
      stretchEngine_(std::make_unique<TimeStretchEngine>()),
      inputBlockScratch_(4096 * 2, 0.0f),
      outputBlockScratch_(4096 * 2, 0.0f) {}

DeckPlayer::~DeckPlayer() = default;

bool DeckPlayer::loadFile(const std::string& filePath) {
    DecodedAudio decoded;
    if (!AudioDecoder::decodeFile(filePath, decoded)) {
        return false;
    }

    loadedAudio_ = std::move(decoded);
    playbackPosition_.store(0.0);
    isPlaying_.store(false);

    // Initialize TimeStretchEngine
    TimeStretchConfig stretchCfg;
    stretchCfg.sampleRate = loadedAudio_.sampleRate;
    stretchCfg.channels = loadedAudio_.channels;
    stretchCfg.tempoRatio = tempoRatio_.load();
    stretchCfg.preservePitch = preservePitch_.load();
    stretchCfg.pitchSemiTones = 0.0f;
    stretchCfg.quickSeek = false;

    stretchEngine_->initialize(stretchCfg);
    stretchEngine_->clear();

    size_t scratchSize = std::max(4096u, loadedAudio_.sampleRate / 4) * std::max(2u, loadedAudio_.channels);
    inputBlockScratch_.assign(scratchSize, 0.0f);
    outputBlockScratch_.assign(scratchSize, 0.0f);

    return true;
}

void DeckPlayer::setPlaying(bool playing) {
    isPlaying_.store(playing);
}

bool DeckPlayer::isPlaying() const {
    return isPlaying_.load();
}

void DeckPlayer::setPlaybackPosition(double seconds) {
    playbackPosition_.store(std::max(0.0, seconds));
    if (stretchEngine_) {
        stretchEngine_->clear();
    }
}

double DeckPlayer::getPlaybackPosition() const {
    return playbackPosition_.load();
}

double DeckPlayer::getDuration() const {
    return loadedAudio_.durationSeconds;
}

void DeckPlayer::setVolume(float vol) {
    volume_.store(std::clamp(vol, 0.0f, 1.0f));
}

float DeckPlayer::getVolume() const {
    return volume_.load();
}

void DeckPlayer::setEq(float low, float mid, float high) {
    lowEq_.store(std::clamp(low, -1.0f, 1.0f));
    midEq_.store(std::clamp(mid, -1.0f, 1.0f));
    highEq_.store(std::clamp(high, -1.0f, 1.0f));
}

void DeckPlayer::setFilter(float filterVal) {
    filter_.store(std::clamp(filterVal, -1.0f, 1.0f));
}

void DeckPlayer::setStemLevels(float vocal, float drum, float bass, float other) {
    vocalStem_.store(std::clamp(vocal, 0.0f, 1.0f));
    drumStem_.store(std::clamp(drum, 0.0f, 1.0f));
    bassStem_.store(std::clamp(bass, 0.0f, 1.0f));
    otherStem_.store(std::clamp(other, 0.0f, 1.0f));
}

void DeckPlayer::setTempoRatio(double ratio) {
    double clamped = std::clamp(ratio, 0.25, 4.0);
    tempoRatio_.store(clamped);
    if (stretchEngine_) {
        stretchEngine_->setTempoRatio(clamped);
    }
}

double DeckPlayer::getTempoRatio() const {
    return tempoRatio_.load();
}

void DeckPlayer::setPitchPreservation(bool enabled) {
    preservePitch_.store(enabled);
}

bool DeckPlayer::isPitchPreserved() const {
    return preservePitch_.load();
}

void DeckPlayer::processBlock(float* outputBuffer, uint32_t numSamples, uint32_t numChannels) noexcept {
    if (!outputBuffer) return;

    if (!isPlaying_.load() || loadedAudio_.samples.empty() || loadedAudio_.sampleRate == 0) {
        std::memset(outputBuffer, 0, numSamples * numChannels * sizeof(float));
        return;
    }

    double currentPosSec = playbackPosition_.load();
    uint64_t currentFrame = static_cast<uint64_t>(currentPosSec * loadedAudio_.sampleRate);
    uint32_t srcChannels = loadedAudio_.channels;
    uint64_t totalFrames = loadedAudio_.totalFrames;
    float vol = volume_.load();
    double ratio = tempoRatio_.load();

    // 1. Fast Path: Unstretched 1.0x Playback
    if (std::abs(ratio - 1.0) < 1e-5 || !preservePitch_.load()) {
        for (uint32_t s = 0; s < numSamples; ++s) {
            if (currentFrame < totalFrames) {
                for (uint32_t c = 0; c < numChannels; ++c) {
                    uint32_t srcChan = (srcChannels == 1) ? 0 : (c % srcChannels);
                    float sample = loadedAudio_.samples[currentFrame * srcChannels + srcChan];
                    outputBuffer[s * numChannels + c] = sample * vol;
                }
                currentFrame++;
            } else {
                for (uint32_t c = 0; c < numChannels; ++c) {
                    outputBuffer[s * numChannels + c] = 0.0f;
                }
            }
        }

        double nextPosSec = static_cast<double>(currentFrame) / loadedAudio_.sampleRate;
        playbackPosition_.store(nextPosSec);

        if (currentFrame >= totalFrames) {
            isPlaying_.store(false);
        }
        return;
    }

    // 2. Pitch-Preserved Time-Stretched Playback
    // Ensure scratch buffers are large enough
    size_t requiredScratch = numSamples * std::max(srcChannels, numChannels);
    if (outputBlockScratch_.size() < requiredScratch) {
        std::memset(outputBuffer, 0, numSamples * numChannels * sizeof(float));
        return;
    }

    // Feed input frames into stretch engine until enough output frames are ready
    uint32_t chunkSize = 512;
    while (stretchEngine_->numAvailableSamples() < numSamples && currentFrame < totalFrames) {
        uint32_t framesToRead = static_cast<uint32_t>(std::min(static_cast<uint64_t>(chunkSize), totalFrames - currentFrame));
        if (framesToRead == 0) break;

        const float* srcPtr = &loadedAudio_.samples[currentFrame * srcChannels];
        stretchEngine_->putSamples(srcPtr, framesToRead);
        currentFrame += framesToRead;
    }

    if (currentFrame >= totalFrames && stretchEngine_->numAvailableSamples() < numSamples) {
        stretchEngine_->flush();
    }

    // Retrieve stretched samples
    uint32_t received = stretchEngine_->receiveSamples(outputBlockScratch_.data(), numSamples);

    for (uint32_t s = 0; s < numSamples; ++s) {
        if (s < received) {
            for (uint32_t c = 0; c < numChannels; ++c) {
                uint32_t srcChan = (srcChannels == 1) ? 0 : (c % srcChannels);
                float sample = outputBlockScratch_[s * srcChannels + srcChan];
                outputBuffer[s * numChannels + c] = sample * vol;
            }
        } else {
            for (uint32_t c = 0; c < numChannels; ++c) {
                outputBuffer[s * numChannels + c] = 0.0f;
            }
        }
    }

    double nextPosSec = static_cast<double>(currentFrame) / loadedAudio_.sampleRate;
    playbackPosition_.store(nextPosSec);

    if (currentFrame >= totalFrames && stretchEngine_->numAvailableSamples() == 0 && received < numSamples) {
        isPlaying_.store(false);
    }
}

DeckStateC DeckPlayer::getState() const noexcept {
    DeckStateC state;
    state.deck_id = deckId_;
    state.is_playing = isPlaying_.load() ? 1 : 0;
    state.playback_position_seconds = playbackPosition_.load();
    state.volume = volume_.load();
    state.low_eq = lowEq_.load();
    state.mid_eq = midEq_.load();
    state.high_eq = highEq_.load();
    state.filter = filter_.load();
    state.vocal_stem_vol = vocalStem_.load();
    state.drum_stem_vol = drumStem_.load();
    state.bass_stem_vol = bassStem_.load();
    state.other_stem_vol = otherStem_.load();
    return state;
}

} // namespace pulse::audio
