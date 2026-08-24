use super::types::{AudioEngineConfigC, AudioEngineStatsC, DeckStateC, TransitionCommandC};
use std::os::raw::{c_char, c_double, c_float, c_int};

extern "C" {
    pub fn pulse_audio_init(config: AudioEngineConfigC) -> c_int;
    pub fn pulse_audio_start() -> c_int;
    pub fn pulse_audio_stop() -> c_int;
    pub fn pulse_audio_shutdown() -> c_int;
    pub fn pulse_audio_is_running() -> c_int;
    pub fn pulse_audio_is_initialized() -> c_int;
    pub fn pulse_audio_get_stats(out_stats: *mut AudioEngineStatsC) -> c_int;
    pub fn pulse_audio_load_track(deck_id: u8, file_path: *const c_char) -> c_int;
    pub fn pulse_audio_prepare_deck(
        deck_id: u8,
        file_path: *const c_char,
        cue_seconds: c_double,
        tempo_ratio: c_double,
        preserve_pitch: u8,
    ) -> c_int;
    pub fn pulse_audio_play(deck_id: u8) -> c_int;
    pub fn pulse_audio_pause(deck_id: u8) -> c_int;
    pub fn pulse_audio_stop_deck(deck_id: u8) -> c_int;
    pub fn pulse_audio_seek(deck_id: u8, position_seconds: c_double) -> c_int;
    pub fn pulse_audio_play_pause(deck_id: u8, play: u8) -> c_int;
    pub fn pulse_audio_set_volume(deck_id: u8, volume: c_float) -> c_int;
    pub fn pulse_audio_set_eq(deck_id: u8, low: c_float, mid: c_float, high: c_float) -> c_int;
    pub fn pulse_audio_set_filter(deck_id: u8, filter_val: c_float) -> c_int;
    pub fn pulse_audio_set_tempo_ratio(deck_id: u8, ratio: c_double) -> c_int;
    pub fn pulse_audio_set_pitch_preservation(deck_id: u8, enabled: u8) -> c_int;
    pub fn pulse_audio_set_stem_levels(
        deck_id: u8,
        vocal: c_float,
        drum: c_float,
        bass: c_float,
        other: c_float,
    ) -> c_int;
    pub fn pulse_audio_get_deck_state(deck_id: u8, out_state: *mut DeckStateC) -> c_int;
    pub fn pulse_audio_is_deck_playing(deck_id: u8) -> c_int;
    pub fn pulse_audio_get_deck_position(deck_id: u8) -> c_double;
    pub fn pulse_audio_get_deck_duration(deck_id: u8) -> c_double;
    pub fn pulse_audio_execute_transition(command: TransitionCommandC) -> c_int;
}
