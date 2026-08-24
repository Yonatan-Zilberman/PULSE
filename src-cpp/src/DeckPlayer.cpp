#include "../include/DeckPlayer.h"
#include <cstring>
#include <algorithm>
#include <cmath>

namespace pulse::audio {

DeckPlayer::DeckPlayer(uint8_t deckId)
    : deckId_(deckId),
      stretchEngine_(std::make_unique<TimeStretchEngine>()),
      eqChannels_(8),
      inputBlockScratch_(32768, 0.0f),
      outputBlockScratch_(32768, 0.0f) {
    initCrossoverFilters(48000);
}

DeckPlayer::~DeckPlayer() {
    isPlaying_.store(false, std::memory_order_release);
    activeAudio_.store(nullptr, std::memory_order_release);
}

void DeckPlayer::initCrossoverFilters(uint32_t sampleRate) noexcept {
    if (sampleRate == 0) sampleRate = 48000;

    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kSqrt2 = 1.41421356237309504880f;

    // 1. Low Crossover at 250 Hz (Butterworth Q = 1/sqrt(2))
    float fLow = 250.0f;
    float wLow = 2.0f * kPi * fLow / static_cast<float>(sampleRate);
    float sinLow = std::sin(wLow);
    float cosLow = std::cos(wLow);
    float alphaLow = sinLow / (2.0f * (1.0f / kSqrt2)); // sinLow / sqrt(2)
    float a0Low = 1.0f + alphaLow;

    // 2nd-order Butterworth LP at 250 Hz
    eqCoeffs_.coeffLpLow.b0 = (1.0f - cosLow) / (2.0f * a0Low);
    eqCoeffs_.coeffLpLow.b1 = (1.0f - cosLow) / a0Low;
    eqCoeffs_.coeffLpLow.b2 = (1.0f - cosLow) / (2.0f * a0Low);
    eqCoeffs_.coeffLpLow.a1 = (-2.0f * cosLow) / a0Low;
    eqCoeffs_.coeffLpLow.a2 = (1.0f - alphaLow) / a0Low;

    // 2nd-order Butterworth HP at 250 Hz
    eqCoeffs_.coeffHpLow.b0 = (1.0f + cosLow) / (2.0f * a0Low);
    eqCoeffs_.coeffHpLow.b1 = -(1.0f + cosLow) / a0Low;
    eqCoeffs_.coeffHpLow.b2 = (1.0f + cosLow) / (2.0f * a0Low);
    eqCoeffs_.coeffHpLow.a1 = (-2.0f * cosLow) / a0Low;
    eqCoeffs_.coeffHpLow.a2 = (1.0f - alphaLow) / a0Low;

    // 2. High Crossover at 3500 Hz (Butterworth Q = 1/sqrt(2))
    float fHigh = 3500.0f;
    float wHigh = 2.0f * kPi * fHigh / static_cast<float>(sampleRate);
    float sinHigh = std::sin(wHigh);
    float cosHigh = std::cos(wHigh);
    float alphaHigh = sinHigh / (2.0f * (1.0f / kSqrt2)); // sinHigh / sqrt(2)
    float a0High = 1.0f + alphaHigh;

    // 2nd-order Butterworth LP at 3500 Hz (Mid band)
    eqCoeffs_.coeffLpHigh.b0 = (1.0f - cosHigh) / (2.0f * a0High);
    eqCoeffs_.coeffLpHigh.b1 = (1.0f - cosHigh) / a0High;
    eqCoeffs_.coeffLpHigh.b2 = (1.0f - cosHigh) / (2.0f * a0High);
    eqCoeffs_.coeffLpHigh.a1 = (-2.0f * cosHigh) / a0High;
    eqCoeffs_.coeffLpHigh.a2 = (1.0f - alphaHigh) / a0High;

    // 2nd-order Butterworth HP at 3500 Hz (High band)
    eqCoeffs_.coeffHpHigh.b0 = (1.0f + cosHigh) / (2.0f * a0High);
    eqCoeffs_.coeffHpHigh.b1 = -(1.0f + cosHigh) / a0High;
    eqCoeffs_.coeffHpHigh.b2 = (1.0f + cosHigh) / (2.0f * a0High);
    eqCoeffs_.coeffHpHigh.a1 = (-2.0f * cosHigh) / a0High;
    eqCoeffs_.coeffHpHigh.a2 = (1.0f - alphaHigh) / a0High;

    // 2nd-order All-Pass at 3500 Hz (aligns low band phase with mid/high)
    eqCoeffs_.coeffApHigh.b0 = (1.0f - alphaHigh) / a0High;
    eqCoeffs_.coeffApHigh.b1 = (-2.0f * cosHigh) / a0High;
    eqCoeffs_.coeffApHigh.b2 = 1.0f;
    eqCoeffs_.coeffApHigh.a1 = (-2.0f * cosHigh) / a0High;
    eqCoeffs_.coeffApHigh.a2 = (1.0f - alphaHigh) / a0High;

    resetEq();
}

void DeckPlayer::resetEq() noexcept {
    for (auto& eq : eqChannels_) {
        eq.reset();
    }
}

bool DeckPlayer::loadFile(const std::string& filePath) {
    return prepareTrack(filePath, 0.0, tempoRatio_.load(std::memory_order_relaxed), preservePitch_.load(std::memory_order_relaxed));
}

bool DeckPlayer::prepareTrack(const std::string& filePath, double cuePositionSec, double tempoRatio, bool preservePitch) {
    playbackState_.store(DeckPlaybackState::Loading, std::memory_order_release);

    DecodedAudio decoded;
    if (!AudioDecoder::decodeFile(filePath, decoded)) {
        playbackState_.store(DeckPlaybackState::Error, std::memory_order_release);
        return false;
    }

    auto newAudio = std::make_unique<DecodedAudio>(std::move(decoded));

    double clampedCue = std::clamp(cuePositionSec, 0.0, newAudio->durationSeconds);
    cuePosition_.store(clampedCue, std::memory_order_release);
    playbackPosition_.store(clampedCue, std::memory_order_release);
    isPlaying_.store(false, std::memory_order_release);

    double clampedRatio = std::clamp(tempoRatio, 0.25, 4.0);
    tempoRatio_.store(clampedRatio, std::memory_order_release);
    preservePitch_.store(preservePitch, std::memory_order_release);

    // Initialize TimeStretchEngine
    TimeStretchConfig stretchCfg;
    stretchCfg.sampleRate = newAudio->sampleRate;
    stretchCfg.channels = newAudio->channels;
    stretchCfg.tempoRatio = clampedRatio;
    stretchCfg.preservePitch = preservePitch;
    stretchCfg.pitchSemiTones = 0.0f;
    stretchCfg.quickSeek = false;

    stretchEngine_->initialize(stretchCfg);
    stretchEngine_->clear();

    initCrossoverFilters(newAudio->sampleRate);

    size_t scratchSize = std::max(32768u, static_cast<uint32_t>(newAudio->sampleRate / 4) * std::max(2u, newAudio->channels));
    if (inputBlockScratch_.size() < scratchSize) {
        inputBlockScratch_.assign(scratchSize, 0.0f);
    }
    if (outputBlockScratch_.size() < scratchSize) {
        outputBlockScratch_.assign(scratchSize, 0.0f);
    }

    // Atomically publish new audio buffer for real-time thread consumption
    activeAudio_.store(newAudio.get(), std::memory_order_release);
    playbackState_.store(DeckPlaybackState::Ready, std::memory_order_release);

    // Keep old audio alive until next load/dtor to prevent RT use-after-free
    previousAudio_ = std::move(currentAudio_);
    currentAudio_ = std::move(newAudio);

    return true;
}

void DeckPlayer::play() {
    const DecodedAudio* audio = activeAudio_.load(std::memory_order_acquire);
    if (!audio || audio->samples.empty()) {
        return;
    }

    if (playbackPosition_.load(std::memory_order_relaxed) >= audio->durationSeconds) {
        playbackPosition_.store(cuePosition_.load(std::memory_order_relaxed), std::memory_order_release);
    }

    isPlaying_.store(true, std::memory_order_release);
    playbackState_.store(DeckPlaybackState::Playing, std::memory_order_release);
}

void DeckPlayer::pause() {
    isPlaying_.store(false, std::memory_order_release);
    if (activeAudio_.load(std::memory_order_acquire)) {
        playbackState_.store(DeckPlaybackState::Paused, std::memory_order_release);
    }
}

void DeckPlayer::stop() {
    isPlaying_.store(false, std::memory_order_release);
    playbackPosition_.store(cuePosition_.load(std::memory_order_relaxed), std::memory_order_release);
    if (stretchEngine_) {
        stretchEngine_->clear();
    }
    resetEq();
    if (activeAudio_.load(std::memory_order_acquire)) {
        playbackState_.store(DeckPlaybackState::Ready, std::memory_order_release);
    }
}

void DeckPlayer::seek(double seconds) {
    const DecodedAudio* audio = activeAudio_.load(std::memory_order_acquire);
    double maxDuration = audio ? audio->durationSeconds : 0.0;
    double clamped = std::clamp(seconds, 0.0, maxDuration);

    playbackPosition_.store(clamped, std::memory_order_release);
    if (stretchEngine_) {
        stretchEngine_->clear();
    }
    resetEq();

    if (!isPlaying_.load(std::memory_order_relaxed) && audio) {
        cuePosition_.store(clamped, std::memory_order_release);
    }
}

void DeckPlayer::setPlaying(bool playing) {
    if (playing) {
        play();
    } else {
        pause();
    }
}

bool DeckPlayer::isPlaying() const noexcept {
    return isPlaying_.load(std::memory_order_relaxed);
}

void DeckPlayer::setPlaybackPosition(double seconds) {
    seek(seconds);
}

double DeckPlayer::getPlaybackPosition() const noexcept {
    return playbackPosition_.load(std::memory_order_relaxed);
}

double DeckPlayer::getDuration() const noexcept {
    const DecodedAudio* audio = activeAudio_.load(std::memory_order_relaxed);
    return audio ? audio->durationSeconds : 0.0;
}

double DeckPlayer::getBpm() const noexcept {
    const DecodedAudio* audio = activeAudio_.load(std::memory_order_relaxed);
    return audio ? audio->detectedBpm : 0.0;
}

void DeckPlayer::setVolume(float vol) {
    volume_.store(std::clamp(vol, 0.0f, 1.0f), std::memory_order_relaxed);
}

float DeckPlayer::getVolume() const noexcept {
    return volume_.load(std::memory_order_relaxed);
}

void DeckPlayer::setEq(float low, float mid, float high) {
    lowEq_.store(std::clamp(low, -1.0f, 1.0f), std::memory_order_relaxed);
    midEq_.store(std::clamp(mid, -1.0f, 1.0f), std::memory_order_relaxed);
    highEq_.store(std::clamp(high, -1.0f, 1.0f), std::memory_order_relaxed);
}

void DeckPlayer::setFilter(float filterVal) {
    filter_.store(std::clamp(filterVal, -1.0f, 1.0f), std::memory_order_relaxed);
}

void DeckPlayer::setStemLevels(float vocal, float drum, float bass, float other) {
    vocalStem_.store(std::clamp(vocal, 0.0f, 1.0f), std::memory_order_relaxed);
    drumStem_.store(std::clamp(drum, 0.0f, 1.0f), std::memory_order_relaxed);
    bassStem_.store(std::clamp(bass, 0.0f, 1.0f), std::memory_order_relaxed);
    otherStem_.store(std::clamp(other, 0.0f, 1.0f), std::memory_order_relaxed);
}

void DeckPlayer::setTempoRatio(double ratio) {
    double clamped = std::clamp(ratio, 0.25, 4.0);
    tempoRatio_.store(clamped, std::memory_order_relaxed);
    if (stretchEngine_) {
        stretchEngine_->setTempoRatio(clamped);
    }
}

double DeckPlayer::getTempoRatio() const noexcept {
    return tempoRatio_.load(std::memory_order_relaxed);
}

void DeckPlayer::setPitchPreservation(bool enabled) {
    preservePitch_.store(enabled, std::memory_order_relaxed);
}

bool DeckPlayer::isPitchPreserved() const noexcept {
    return preservePitch_.load(std::memory_order_relaxed);
}

void DeckPlayer::processBlock(float* outputBuffer, uint32_t numSamples, uint32_t numChannels) noexcept {
    if (!outputBuffer) return;

    const DecodedAudio* audio = activeAudio_.load(std::memory_order_acquire);

    if (!isPlaying_.load(std::memory_order_relaxed) || !audio || audio->samples.empty() || audio->sampleRate == 0) {
        std::memset(outputBuffer, 0, numSamples * numChannels * sizeof(float));
        return;
    }

    double currentPosSec = playbackPosition_.load(std::memory_order_relaxed);
    uint64_t currentFrame = static_cast<uint64_t>(std::round(currentPosSec * audio->sampleRate));
    uint32_t srcChannels = audio->channels;
    uint64_t totalFrames = audio->totalFrames;
    float vol = volume_.load(std::memory_order_relaxed);
    double ratio = tempoRatio_.load(std::memory_order_relaxed);

    float lowVal = lowEq_.load(std::memory_order_relaxed);
    float midVal = midEq_.load(std::memory_order_relaxed);
    float highVal = highEq_.load(std::memory_order_relaxed);

    // Map EQ parameters [-1.0, 1.0]: [-1, 0] -> [0, 1], [0, 1] -> [1, 2]
    float gainLow = std::clamp((lowVal < 0.0f) ? (1.0f + lowVal) : (1.0f + lowVal), 0.0f, 2.0f);
    float gainMid = std::clamp((midVal < 0.0f) ? (1.0f + midVal) : (1.0f + midVal), 0.0f, 2.0f);
    float gainHigh = std::clamp((highVal < 0.0f) ? (1.0f + highVal) : (1.0f + highVal), 0.0f, 2.0f);

    // 1. Fast Path: Unstretched 1.0x Playback
    if (std::abs(ratio - 1.0) < 1e-5 || !preservePitch_.load(std::memory_order_relaxed)) {
        for (uint32_t s = 0; s < numSamples; ++s) {
            if (currentFrame < totalFrames) {
                for (uint32_t c = 0; c < numChannels; ++c) {
                    uint32_t srcChan = (srcChannels == 1) ? 0 : (c % srcChannels);
                    float rawSample = audio->samples[currentFrame * srcChannels + srcChan];

                    // 3-Band LR4 Crossover Filtering
                    auto& eq = eqChannels_[c % eqChannels_.size()];
                    float lowRaw = eq.lpLow.process(rawSample, eqCoeffs_.coeffLpLow);
                    float midHigh = eq.hpLow.process(rawSample, eqCoeffs_.coeffHpLow);
                    float low = eq.apHigh.process(lowRaw, eqCoeffs_.coeffApHigh);
                    float mid = eq.lpHigh.process(midHigh, eqCoeffs_.coeffLpHigh);
                    float high = eq.hpHigh.process(midHigh, eqCoeffs_.coeffHpHigh);

                    float filtered = low * gainLow + mid * gainMid + high * gainHigh;
                    outputBuffer[s * numChannels + c] = filtered * vol;
                }
                currentFrame++;
            } else {
                for (uint32_t c = 0; c < numChannels; ++c) {
                    outputBuffer[s * numChannels + c] = 0.0f;
                }
            }
        }

        double nextPosSec = static_cast<double>(currentFrame) / audio->sampleRate;
        playbackPosition_.store(nextPosSec, std::memory_order_release);

        if (currentFrame >= totalFrames) {
            isPlaying_.store(false, std::memory_order_release);
            playbackState_.store(DeckPlaybackState::Ready, std::memory_order_release);
        }
        return;
    }

    // 2. Pitch-Preserved Time-Stretched Playback
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

        const float* srcPtr = &audio->samples[currentFrame * srcChannels];
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
                float rawSample = outputBlockScratch_[s * srcChannels + srcChan];

                // 3-Band LR4 Crossover Filtering
                auto& eq = eqChannels_[c % eqChannels_.size()];
                float lowRaw = eq.lpLow.process(rawSample, eqCoeffs_.coeffLpLow);
                float midHigh = eq.hpLow.process(rawSample, eqCoeffs_.coeffHpLow);
                float low = eq.apHigh.process(lowRaw, eqCoeffs_.coeffApHigh);
                float mid = eq.lpHigh.process(midHigh, eqCoeffs_.coeffLpHigh);
                float high = eq.hpHigh.process(midHigh, eqCoeffs_.coeffHpHigh);

                float filtered = low * gainLow + mid * gainMid + high * gainHigh;
                outputBuffer[s * numChannels + c] = filtered * vol;
            }
        } else {
            for (uint32_t c = 0; c < numChannels; ++c) {
                outputBuffer[s * numChannels + c] = 0.0f;
            }
        }
    }

    double nextPosSec = static_cast<double>(currentFrame) / audio->sampleRate;
    playbackPosition_.store(nextPosSec, std::memory_order_release);

    if (currentFrame >= totalFrames && stretchEngine_->numAvailableSamples() == 0 && received < numSamples) {
        isPlaying_.store(false, std::memory_order_release);
        playbackState_.store(DeckPlaybackState::Ready, std::memory_order_release);
    }
}

DeckStateC DeckPlayer::getState() const noexcept {
    DeckStateC state{};
    state.deck_id = deckId_;
    state.is_playing = isPlaying_.load(std::memory_order_relaxed) ? 1 : 0;
    state.playback_state = static_cast<uint8_t>(playbackState_.load(std::memory_order_relaxed));
    state.preserve_pitch = preservePitch_.load(std::memory_order_relaxed) ? 1 : 0;
    state.playback_position_seconds = playbackPosition_.load(std::memory_order_relaxed);

    const DecodedAudio* audio = activeAudio_.load(std::memory_order_relaxed);
    if (audio) {
        state.duration_seconds = audio->durationSeconds;
        state.bpm = audio->detectedBpm;
    } else {
        state.duration_seconds = 0.0;
        state.bpm = 0.0;
    }

    state.tempo_ratio = tempoRatio_.load(std::memory_order_relaxed);
    state.volume = volume_.load(std::memory_order_relaxed);
    state.low_eq = lowEq_.load(std::memory_order_relaxed);
    state.mid_eq = midEq_.load(std::memory_order_relaxed);
    state.high_eq = highEq_.load(std::memory_order_relaxed);
    state.filter = filter_.load(std::memory_order_relaxed);
    state.vocal_stem_vol = vocalStem_.load(std::memory_order_relaxed);
    state.drum_stem_vol = drumStem_.load(std::memory_order_relaxed);
    state.bass_stem_vol = bassStem_.load(std::memory_order_relaxed);
    state.other_stem_vol = otherStem_.load(std::memory_order_relaxed);
    return state;
}

} // namespace pulse::audio
