#include "../include/AudioBridgeTypes.h"
#include "../include/AudioEngine.h"
#include <string>

extern "C" {

int pulse_audio_init(AudioEngineConfigC config) {
    return pulse::audio::AudioEngine::getInstance().initialize(config);
}

int pulse_audio_shutdown(void) {
    return pulse::audio::AudioEngine::getInstance().shutdown();
}

int pulse_audio_load_track(uint8_t deck_id, const char* file_path) {
    if (!file_path) return -1;
    bool success = pulse::audio::AudioEngine::getInstance().loadTrack(deck_id, std::string(file_path));
    return success ? 0 : -1;
}

int pulse_audio_play_pause(uint8_t deck_id, uint8_t play) {
    bool success = pulse::audio::AudioEngine::getInstance().setPlaying(deck_id, play != 0);
    return success ? 0 : -1;
}

int pulse_audio_get_deck_state(uint8_t deck_id, DeckStateC* out_state) {
    if (!out_state) return -1;
    *out_state = pulse::audio::AudioEngine::getInstance().getDeckState(deck_id);
    return 0;
}

int pulse_audio_execute_transition(TransitionCommandC command) {
    return pulse::audio::AudioEngine::getInstance().executeTransition(command);
}

}
