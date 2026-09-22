//! Private C FFI boundary — the only place declaring `extern "C"` engine symbols.
//!
//! Boundary rule: no production code outside [`super::bridge`] may call into
//! this module. Every command and future caller goes through
//! [`super::bridge::AudioBridge`]. (Integration tests may exercise the raw FFI
//! contract directly to assert C-side behavior such as `-1` rejections.)
//!
//! When `build.rs` could not build the C++ engine (`CMake` missing), the
//! `pulse_audio_engine_unavailable` cfg is set and stub functions returning
//! failure values are used instead, so the crate still compiles and every
//! command degrades to `EngineNotAvailable` at runtime.

use super::types::{
    AudioEngineConfigC, AudioEngineStatsC, AudioEventC, DeckStateC, TransitionCommandC,
};
use std::os::raw::{c_char, c_double, c_float, c_int};

#[cfg(not(pulse_audio_engine_unavailable))]
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
    pub fn pulse_audio_drain_events(
        out: *mut AudioEventC,
        max_events: u32,
        out_dropped: *mut u32,
    ) -> c_int;
}

#[cfg(pulse_audio_engine_unavailable)]
mod unavailable_stubs {
    use super::*;

    // Failure-valued stand-ins with the exact signatures of the real symbols:
    // every control call reports failure and every getter returns zero.
    pub fn pulse_audio_init(_config: AudioEngineConfigC) -> c_int {
        -1
    }
    pub fn pulse_audio_start() -> c_int {
        -1
    }
    pub fn pulse_audio_stop() -> c_int {
        -1
    }
    pub fn pulse_audio_shutdown() -> c_int {
        -1
    }
    pub fn pulse_audio_is_running() -> c_int {
        0
    }
    pub fn pulse_audio_is_initialized() -> c_int {
        0
    }
    pub fn pulse_audio_get_stats(_out_stats: *mut AudioEngineStatsC) -> c_int {
        -1
    }
    pub fn pulse_audio_load_track(_deck_id: u8, _file_path: *const c_char) -> c_int {
        -1
    }
    pub fn pulse_audio_prepare_deck(
        _deck_id: u8,
        _file_path: *const c_char,
        _cue_seconds: c_double,
        _tempo_ratio: c_double,
        _preserve_pitch: u8,
    ) -> c_int {
        -1
    }
    pub fn pulse_audio_play(_deck_id: u8) -> c_int {
        -1
    }
    pub fn pulse_audio_pause(_deck_id: u8) -> c_int {
        -1
    }
    pub fn pulse_audio_stop_deck(_deck_id: u8) -> c_int {
        -1
    }
    pub fn pulse_audio_seek(_deck_id: u8, _position_seconds: c_double) -> c_int {
        -1
    }
    pub fn pulse_audio_play_pause(_deck_id: u8, _play: u8) -> c_int {
        -1
    }
    pub fn pulse_audio_set_volume(_deck_id: u8, _volume: c_float) -> c_int {
        -1
    }
    pub fn pulse_audio_set_eq(_deck_id: u8, _low: c_float, _mid: c_float, _high: c_float) -> c_int {
        -1
    }
    pub fn pulse_audio_set_filter(_deck_id: u8, _filter_val: c_float) -> c_int {
        -1
    }
    pub fn pulse_audio_set_tempo_ratio(_deck_id: u8, _ratio: c_double) -> c_int {
        -1
    }
    pub fn pulse_audio_set_pitch_preservation(_deck_id: u8, _enabled: u8) -> c_int {
        -1
    }
    pub fn pulse_audio_set_stem_levels(
        _deck_id: u8,
        _vocal: c_float,
        _drum: c_float,
        _bass: c_float,
        _other: c_float,
    ) -> c_int {
        -1
    }
    pub fn pulse_audio_get_deck_state(_deck_id: u8, _out_state: *mut DeckStateC) -> c_int {
        -1
    }
    pub fn pulse_audio_is_deck_playing(_deck_id: u8) -> c_int {
        0
    }
    pub fn pulse_audio_get_deck_position(_deck_id: u8) -> c_double {
        0.0
    }
    pub fn pulse_audio_get_deck_duration(_deck_id: u8) -> c_double {
        0.0
    }
    pub fn pulse_audio_execute_transition(_command: TransitionCommandC) -> c_int {
        -1
    }
    pub fn pulse_audio_drain_events(
        _out: *mut AudioEventC,
        _max_events: u32,
        _out_dropped: *mut u32,
    ) -> c_int {
        0
    }
}

#[cfg(pulse_audio_engine_unavailable)]
pub use unavailable_stubs::*;
