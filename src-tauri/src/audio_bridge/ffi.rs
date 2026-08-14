use super::types::{AudioEngineConfigC, DeckStateC, TransitionCommandC};
use std::os::raw::{c_char, c_int};

extern "C" {
    pub fn pulse_audio_init(config: AudioEngineConfigC) -> c_int;
    pub fn pulse_audio_shutdown() -> c_int;
    pub fn pulse_audio_load_track(deck_id: u8, file_path: *const c_char) -> c_int;
    pub fn pulse_audio_play_pause(deck_id: u8, play: u8) -> c_int;
    pub fn pulse_audio_get_deck_state(deck_id: u8, out_state: *mut DeckStateC) -> c_int;
    pub fn pulse_audio_execute_transition(command: TransitionCommandC) -> c_int;
}
