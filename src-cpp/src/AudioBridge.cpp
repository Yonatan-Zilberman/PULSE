#include "../include/AudioBridgeTypes.h"
#include "../include/AudioEngine.h"
#include <string>

extern "C" {

int pulse_audio_init(AudioEngineConfigC config) {
    return pulse::audio::AudioEngine::getInstance().initialize(config);
}

int pulse_audio_start(void) {
    return pulse::audio::AudioEngine::getInstance().start();
}

int pulse_audio_stop(void) {
    return pulse::audio::AudioEngine::getInstance().stop();
}

int pulse_audio_shutdown(void) {
    return pulse::audio::AudioEngine::getInstance().shutdown();
}

int pulse_audio_is_running(void) {
    return pulse::audio::AudioEngine::getInstance().isRunning() ? 1 : 0;
}

int pulse_audio_is_initialized(void) {
    return pulse::audio::AudioEngine::getInstance().isInitialized() ? 1 : 0;
}

int pulse_audio_get_stats(AudioEngineStatsC* out_stats) {
    if (!out_stats) return -1;
    *out_stats = pulse::audio::AudioEngine::getInstance().getStats();
    return 0;
}

int pulse_audio_load_track(uint8_t deck_id, const char* file_path) {
    if (!file_path) return -1;
    bool success = pulse::audio::AudioEngine::getInstance().loadTrack(deck_id, std::string(file_path));
    return success ? 0 : -1;
}

int pulse_audio_prepare_deck(uint8_t deck_id, const char* file_path, double cue_seconds, double tempo_ratio, uint8_t preserve_pitch) {
    if (!file_path) return -1;
    bool success = pulse::audio::AudioEngine::getInstance().prepareDeck(
        deck_id,
        std::string(file_path),
        cue_seconds,
        tempo_ratio,
        preserve_pitch != 0
    );
    return success ? 0 : -1;
}

int pulse_audio_play(uint8_t deck_id) {
    bool success = pulse::audio::AudioEngine::getInstance().playDeck(deck_id);
    return success ? 0 : -1;
}

int pulse_audio_pause(uint8_t deck_id) {
    bool success = pulse::audio::AudioEngine::getInstance().pauseDeck(deck_id);
    return success ? 0 : -1;
}

int pulse_audio_stop_deck(uint8_t deck_id) {
    bool success = pulse::audio::AudioEngine::getInstance().stopDeck(deck_id);
    return success ? 0 : -1;
}

int pulse_audio_seek(uint8_t deck_id, double position_seconds) {
    bool success = pulse::audio::AudioEngine::getInstance().seekDeck(deck_id, position_seconds);
    return success ? 0 : -1;
}

int pulse_audio_play_pause(uint8_t deck_id, uint8_t play) {
    bool success = pulse::audio::AudioEngine::getInstance().setPlaying(deck_id, play != 0);
    return success ? 0 : -1;
}

int pulse_audio_set_volume(uint8_t deck_id, float volume) {
    bool success = pulse::audio::AudioEngine::getInstance().setDeckVolume(deck_id, volume);
    return success ? 0 : -1;
}

int pulse_audio_set_eq(uint8_t deck_id, float low, float mid, float high) {
    bool success = pulse::audio::AudioEngine::getInstance().setDeckEq(deck_id, low, mid, high);
    return success ? 0 : -1;
}

int pulse_audio_set_filter(uint8_t deck_id, float filter_val) {
    bool success = pulse::audio::AudioEngine::getInstance().setDeckFilter(deck_id, filter_val);
    return success ? 0 : -1;
}

int pulse_audio_set_tempo_ratio(uint8_t deck_id, double ratio) {
    bool success = pulse::audio::AudioEngine::getInstance().setDeckTempoRatio(deck_id, ratio);
    return success ? 0 : -1;
}

int pulse_audio_set_pitch_preservation(uint8_t deck_id, uint8_t enabled) {
    bool success = pulse::audio::AudioEngine::getInstance().setDeckPitchPreservation(deck_id, enabled != 0);
    return success ? 0 : -1;
}

int pulse_audio_set_stem_levels(uint8_t deck_id, float vocal, float drum, float bass, float other) {
    bool success = pulse::audio::AudioEngine::getInstance().setDeckStemLevels(deck_id, vocal, drum, bass, other);
    return success ? 0 : -1;
}

int pulse_audio_get_deck_state(uint8_t deck_id, DeckStateC* out_state) {
    if (!out_state) return -1;
    *out_state = pulse::audio::AudioEngine::getInstance().getDeckState(deck_id);
    return 0;
}

int pulse_audio_is_deck_playing(uint8_t deck_id) {
    return pulse::audio::AudioEngine::getInstance().isDeckPlaying(deck_id) ? 1 : 0;
}

double pulse_audio_get_deck_position(uint8_t deck_id) {
    return pulse::audio::AudioEngine::getInstance().getDeckPosition(deck_id);
}

double pulse_audio_get_deck_duration(uint8_t deck_id) {
    return pulse::audio::AudioEngine::getInstance().getDeckDuration(deck_id);
}

int pulse_audio_execute_transition(TransitionCommandC command) {
    return pulse::audio::AudioEngine::getInstance().executeTransition(command);
}

int pulse_audio_drain_events(AudioEventC* out, uint32_t max_events, uint32_t* out_dropped) {
    if (!out || max_events == 0) return 0;
    return static_cast<int>(pulse::audio::AudioEngine::getInstance().drainEvents(out, max_events, out_dropped));
}

}

