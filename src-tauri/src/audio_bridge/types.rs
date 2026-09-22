use serde::{Deserialize, Serialize};

#[repr(C)]
#[derive(Debug, Default, Clone, Copy, Serialize, Deserialize, PartialEq)]
pub struct AudioEngineConfigC {
    pub sample_rate: u32,
    pub buffer_size: u32,
    pub channel_count: u32,
}

#[repr(C)]
#[derive(Debug, Default, Clone, Copy, Serialize, Deserialize, PartialEq)]
pub struct DeckStateC {
    pub deck_id: u8, // 0 = Deck A, 1 = Deck B
    pub is_playing: u8,
    pub playback_state: u8, // 0 = Empty, 1 = Loading, 2 = Ready, 3 = Playing, 4 = Paused, 5 = Error
    pub preserve_pitch: u8,
    pub playback_position_seconds: f64,
    pub duration_seconds: f64,
    pub bpm: f64,
    pub tempo_ratio: f64,
    pub volume: f32,
    pub low_eq: f32,
    pub mid_eq: f32,
    pub high_eq: f32,
    pub filter: f32,
    pub vocal_stem_vol: f32,
    pub drum_stem_vol: f32,
    pub bass_stem_vol: f32,
    pub other_stem_vol: f32,
}

/// Versioned full-plan transition execution command.
///
/// Mirrors `TransitionCommandC` in `src-cpp/include/AudioBridgeTypes.h` (v2 layout) field
/// for field. 120 bytes, 8-byte aligned; offsets: `version`@0, `duration_seconds`@16,
/// `transition_type`@112. The C++ executor sanitizes every parameter (non-finite -> safe
/// default, then bounds clamp) before execution.
#[repr(C)]
#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq)]
#[allow(clippy::pub_underscore_fields)]
pub struct TransitionCommandC {
    pub version: u32,                // 1 = v2
    pub source_deck: u8,             // 0 | 1
    pub destination_deck: u8,        // 0 | 1
    pub crossfader_curve: u8,        // 0 = eq-power, 1 = linear, 2 = s-curve
    pub flags: u8,                   // bit0 = customCrossfader
    pub _pad0: f32,                  // 0.0f (alignment)
    pub duration_seconds: f64,       // [0.1, 600.0]
    pub src_tempo_ratio: f32,        // [0.5, 2.0]
    pub dst_tempo_ratio: f32,        // [0.5, 2.0]
    pub dst_tempo_ramp_seconds: f32, // [0.0, 300.0]
    pub src_gain: f32,               // [0.0, 1.0]
    pub dst_gain: f32,               // [0.0, 1.0]
    pub src_low_eq: f32,             // [-1.0, 1.0]
    pub src_mid_eq: f32,             // [-1.0, 1.0]
    pub src_high_eq: f32,            // [-1.0, 1.0]
    pub dst_low_eq: f32,             // [-1.0, 1.0]
    pub dst_mid_eq: f32,             // [-1.0, 1.0]
    pub dst_high_eq: f32,            // [-1.0, 1.0]
    pub src_filter: f32,             // [-1.0, 1.0]
    pub dst_filter: f32,             // [-1.0, 1.0]
    pub src_vocal_stem: f32,         // [0.0, 1.0]
    pub dst_vocal_stem: f32,         // [0.0, 1.0]
    pub crossfader_start: f32,       // [-1.0, 1.0]
    pub crossfader_end: f32,         // [-1.0, 1.0]
    pub phase_sync_end: f32,         // [0,1], default 0.500
    pub phase_eq_end: f32,           // [0,1], default 0.875
    pub phase_vocal_end: f32,        // [0,1], default 1.000
    pub bass_swap_point: f32,        // [0.1, 0.9], default 0.50
    pub bass_swap_window: f32,       // [0.02, 0.50], default 0.10
    pub transition_type: u32,        // TransitionStrategyType
    pub _pad1: u32,                  // 0
}

impl TransitionCommandC {
    /// Safe-default v2 command (matches the C++ executor's documented sanitization
    /// defaults). Deck ids stay 0/1; callers set them explicitly per plan.
    pub const fn v2_default() -> Self {
        Self {
            version: 1,
            source_deck: 0,
            destination_deck: 1,
            crossfader_curve: 0,
            flags: 0,
            _pad0: 0.0,
            duration_seconds: 16.0,
            src_tempo_ratio: 1.0,
            dst_tempo_ratio: 1.0,
            dst_tempo_ramp_seconds: 0.0,
            src_gain: 1.0,
            dst_gain: 1.0,
            src_low_eq: 0.0,
            src_mid_eq: 0.0,
            src_high_eq: 0.0,
            dst_low_eq: 0.0,
            dst_mid_eq: 0.0,
            dst_high_eq: 0.0,
            src_filter: 0.0,
            dst_filter: 0.0,
            src_vocal_stem: 1.0,
            dst_vocal_stem: 1.0,
            crossfader_start: -1.0,
            crossfader_end: 1.0,
            phase_sync_end: 0.5,
            phase_eq_end: 0.875,
            phase_vocal_end: 1.0,
            bass_swap_point: 0.5,
            bass_swap_window: 0.1,
            transition_type: 0,
            _pad1: 0,
        }
    }
}

#[repr(C)]
#[derive(Debug, Default, Clone, Copy, Serialize, Deserialize, PartialEq)]
pub struct AudioEngineStatsC {
    pub sample_rate: u32,
    pub buffer_size: u32,
    pub channel_count: u32,
    pub is_initialized: u8,
    pub is_running: u8,
    pub pad: [u8; 2],
    pub total_frames_processed: u64,
    pub underrun_count: u32,
    pub cpu_load: f32,
}

/// Engine -> application event record. Mirrors `AudioEventC` in
/// `src-cpp/include/AudioBridgeTypes.h`: 32 bytes, 8-byte aligned, append-only.
/// Offsets: `version`@0, `kind`@4, `deck_id`@8, `code`@16, `detail`@24.
#[repr(C)]
#[derive(Debug, Default, Clone, Copy)]
#[allow(clippy::pub_underscore_fields)]
pub struct AudioEventC {
    pub version: u32, // must be 1
    pub kind: u32,    // AudioEventKindC
    pub deck_id: u8,  // 0|1; 255 = engine/transition-wide
    pub _pad0: [u8; 7],
    pub code: i32,
    pub _pad1: u32,
    pub detail: f64,
}

/// Engine event kinds — mirrors the C `AudioEventKindC` enum.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum EventKind {
    EngineStarted,
    EngineStopped,
    EngineShutdown,
    Underrun,
    TrackLoaded,
    TrackLoadFailed,
    DeckStateChanged,
    TrackEnded,
    TransitionStarted,
    TransitionCompleted,
    TransitionRejected,
    /// Kind value emitted by a newer engine build than this binary knows.
    Unknown(u32),
}

impl EventKind {
    /// Maps a raw C kind value; unknown values are preserved losslessly.
    #[must_use]
    pub fn from_c(kind: u32) -> Self {
        match kind {
            1 => Self::EngineStarted,
            2 => Self::EngineStopped,
            3 => Self::EngineShutdown,
            4 => Self::Underrun,
            5 => Self::TrackLoaded,
            6 => Self::TrackLoadFailed,
            7 => Self::DeckStateChanged,
            8 => Self::TrackEnded,
            9 => Self::TransitionStarted,
            10 => Self::TransitionCompleted,
            11 => Self::TransitionRejected,
            other => Self::Unknown(other),
        }
    }
}

/// Typed engine event delivered to the UI on the `pulse://audio-event` Tauri event.
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
pub struct EngineEvent {
    pub kind: EventKind,
    pub deck: u8,
    pub code: i32,
    pub detail: f64,
}

impl From<AudioEventC> for EngineEvent {
    fn from(event: AudioEventC) -> Self {
        Self {
            kind: EventKind::from_c(event.kind),
            deck: event.deck_id,
            code: event.code,
            detail: event.detail,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::mem::size_of;

    #[test]
    fn test_c_struct_sizes() {
        assert_eq!(size_of::<AudioEngineConfigC>(), 12);
        // DeckStateC has f64 and multiple f32s; ensure predictable memory layout
        assert!(size_of::<DeckStateC>() >= 48);
        assert_eq!(size_of::<AudioEngineStatsC>(), 32);
    }

    #[test]
    fn test_audio_event_c_layout() {
        assert_eq!(size_of::<AudioEventC>(), 32);
        let zero = AudioEventC::default();
        let base = std::ptr::addr_of!(zero) as usize;
        // Offsets must match the C header static_asserts exactly.
        assert_eq!(std::ptr::addr_of!(zero.version) as usize - base, 0);
        assert_eq!(std::ptr::addr_of!(zero.kind) as usize - base, 4);
        assert_eq!(std::ptr::addr_of!(zero.deck_id) as usize - base, 8);
        assert_eq!(std::ptr::addr_of!(zero.code) as usize - base, 16);
        assert_eq!(std::ptr::addr_of!(zero.detail) as usize - base, 24);
    }

    #[test]
    fn test_engine_event_from_c_maps_every_kind() {
        for (c_kind, expected) in [
            (1u32, EventKind::EngineStarted),
            (2, EventKind::EngineStopped),
            (3, EventKind::EngineShutdown),
            (4, EventKind::Underrun),
            (5, EventKind::TrackLoaded),
            (6, EventKind::TrackLoadFailed),
            (7, EventKind::DeckStateChanged),
            (8, EventKind::TrackEnded),
            (9, EventKind::TransitionStarted),
            (10, EventKind::TransitionCompleted),
            (11, EventKind::TransitionRejected),
        ] {
            let c = AudioEventC {
                version: 1,
                kind: c_kind,
                deck_id: 1,
                code: 7,
                detail: 1.5,
                ..Default::default()
            };
            let event = EngineEvent::from(c);
            assert_eq!(event.kind, expected);
            assert_eq!(event.deck, 1);
            assert_eq!(event.code, 7);
            assert_eq!(event.detail, 1.5);
        }

        let unknown = EngineEvent::from(AudioEventC {
            kind: 42,
            ..Default::default()
        });
        assert_eq!(unknown.kind, EventKind::Unknown(42));
    }

    #[test]
    fn test_transition_command_v2_layout() {
        assert_eq!(size_of::<TransitionCommandC>(), 120);
        let zero = TransitionCommandC::v2_default();
        let base = std::ptr::addr_of!(zero) as usize;
        // Offsets must match the C header static_asserts exactly.
        assert_eq!(std::ptr::addr_of!(zero.version) as usize - base, 0);
        assert_eq!(
            std::ptr::addr_of!(zero.duration_seconds) as usize - base,
            16
        );
        assert_eq!(
            std::ptr::addr_of!(zero.transition_type) as usize - base,
            112
        );
    }

    #[test]
    #[allow(clippy::used_underscore_binding, clippy::float_cmp)] // deterministic v2 default values, exact bits
    fn test_transition_command_v2_defaults_match_c() {
        let c = TransitionCommandC::v2_default();
        assert_eq!(c.version, 1);
        assert_eq!(c.source_deck, 0);
        assert_eq!(c.destination_deck, 1);
        assert_eq!(c.crossfader_curve, 0);
        assert_eq!(c.flags, 0);
        assert_eq!(c._pad0, 0.0);
        assert_eq!(c.duration_seconds, 16.0);
        assert_eq!(c.src_tempo_ratio, 1.0);
        assert_eq!(c.dst_tempo_ratio, 1.0);
        assert_eq!(c.dst_tempo_ramp_seconds, 0.0);
        assert_eq!(c.src_gain, 1.0);
        assert_eq!(c.dst_gain, 1.0);
        assert_eq!((c.src_low_eq, c.src_mid_eq, c.src_high_eq), (0.0, 0.0, 0.0));
        assert_eq!((c.dst_low_eq, c.dst_mid_eq, c.dst_high_eq), (0.0, 0.0, 0.0));
        assert_eq!((c.src_filter, c.dst_filter), (0.0, 0.0));
        assert_eq!((c.src_vocal_stem, c.dst_vocal_stem), (1.0, 1.0));
        assert_eq!((c.crossfader_start, c.crossfader_end), (-1.0, 1.0));
        assert_eq!(
            (c.phase_sync_end, c.phase_eq_end, c.phase_vocal_end),
            (0.5, 0.875, 1.0)
        );
        assert_eq!((c.bass_swap_point, c.bass_swap_window), (0.5, 0.1));
        assert_eq!(c.transition_type, 0);
        assert_eq!(c._pad1, 0);
    }
}
