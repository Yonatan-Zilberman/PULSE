#include "../include/DeckPlayer.h"
#include <cstring>
#include <algorithm>

namespace pulse::audio {

DeckPlayer::DeckPlayer(uint8_t deckId) : deckId_(deckId) {}

bool DeckPlayer::loadFile(const std::string& /*filePath*/) {
    playbackPosition_.store(0.0);
    isPlaying_.store(false);
    return true;
}

void DeckPlayer::setPlaying(bool playing) {
    isPlaying_.store(playing);
}

bool DeckPlayer::isPlaying() const {
    return isPlaying_.load();
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

void DeckPlayer::processBlock(float* outputBuffer, uint32_t numSamples, uint32_t numChannels) noexcept {
    if (!outputBuffer) return;

    if (!isPlaying_.load()) {
        std::memset(outputBuffer, 0, numSamples * numChannels * sizeof(float));
        return;
    }

    // Baseline silence fill until DSP audio file decoding is integrated in Phase 1
    std::memset(outputBuffer, 0, numSamples * numChannels * sizeof(float));
    playbackPosition_.store(playbackPosition_.load() + (static_cast<double>(numSamples) / 48000.0));
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
