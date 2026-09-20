//! Application-layer transition planning *data shape* for the audio engine.
//!
//! This is the acceptance API between DJ Brain and the C++ real-time engine:
//! a fully precomputed, serializable plan that maps 1:1 onto the versioned
//! v2 `TransitionCommandC` C ABI. NO planning logic lives here — DJ Brain
//! decides the values; the engine executes them deterministically.
//!
//! `TransitionPlan::sanitize()` mirrors the C++ executor's sanitization
//! contract exactly (non-finite -> safe default, bounds clamp, monotonic
//! phase cascade, structural rejection) so that a plan rejected on the Rust
//! side is also rejected on the C++ side.

use super::types::TransitionCommandC;

use serde::{Deserialize, Serialize};

/// Classic EQ Blend — the Tech Design §9.1 reference transition type.
pub const TRANSITION_TYPE_CLASSIC_EQ_BLEND: u32 = 9;

/// Versioned full-plan transition command, ready for the C ABI.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct TransitionPlan {
    /// Track the transition exits from (deck 0).
    pub source_track_id: String,
    /// Track the transition lands on (deck 1).
    pub source_deck: u8,
    pub destination_track_id: String,
    pub destination_deck: u8,
    /// Master-timeline positions (informational; the engine clock is block-driven).
    pub start_position_seconds: f64,
    pub end_position_seconds: f64,
    pub duration_seconds: f64,
    /// Tempo strategy, precomputed.
    pub src_tempo_ratio: f32,
    pub dst_tempo_ratio: f32,
    pub dst_tempo_ramp_seconds: f32,
    /// Precomputed gain staging (attenuation-only, LUFS-matched).
    pub src_gain: f32,
    pub dst_gain: f32,
    /// 3-band EQ targets per deck: [low, mid, high], each in [-1, 1].
    pub src_eq: [f32; 3],
    pub dst_eq: [f32; 3],
    /// Bipolar DJ filter targets per deck, each in [-1, 1].
    pub src_filter: f32,
    pub dst_filter: f32,
    /// Vocal stem handoff targets per deck, each in [0, 1].
    pub src_vocal_stem: f32,
    pub dst_vocal_stem: f32,
    /// Crossfader automation.
    pub crossfader_start: f32,
    pub crossfader_end: f32,
    /// 0 = equal-power, 1 = linear, 2 = s-curve.
    pub crossfader_curve: u8,
    /// Phase boundaries of the reference model, each in [0, 1].
    pub phase_sync_end: f32,
    pub phase_eq_end: f32,
    pub phase_vocal_end: f32,
    /// Sequenced bass handoff.
    pub bass_swap_point: f32,
    pub bass_swap_window: f32,
    pub transition_type: u32,
    /// DJ Brain confidence in this plan, in [0, 1].
    pub confidence: f32,
}

impl Default for TransitionPlan {
    fn default() -> Self {
        Self {
            source_track_id: String::new(),
            source_deck: 0,
            destination_track_id: String::new(),
            destination_deck: 1,
            start_position_seconds: 0.0,
            end_position_seconds: 16.0,
            duration_seconds: 16.0,
            src_tempo_ratio: 1.0,
            dst_tempo_ratio: 1.0,
            dst_tempo_ramp_seconds: 0.0,
            src_gain: 1.0,
            dst_gain: 1.0,
            src_eq: [0.0; 3],
            dst_eq: [0.0; 3],
            src_filter: 0.0,
            dst_filter: 0.0,
            src_vocal_stem: 1.0,
            dst_vocal_stem: 1.0,
            crossfader_start: -1.0,
            crossfader_end: 1.0,
            crossfader_curve: 0,
            phase_sync_end: 0.5,
            phase_eq_end: 0.875,
            phase_vocal_end: 1.0,
            bass_swap_point: 0.5,
            bass_swap_window: 0.1,
            transition_type: 0,
            confidence: 1.0,
        }
    }
}

impl TransitionPlan {
    /// Replaces a non-finite f32 by a safe default, then clamps to [lo, hi].
    /// (Mirrors `detail::sanitizeClamp` in the C++ executor.)
    fn sanitize_f32(value: f32, lo: f64, hi: f64, default: f32) -> f32 {
        if value.is_finite() {
            value.clamp(lo as f32, hi as f32)
        } else {
            default
        }
    }

    /// Structural validity: deck ids in [0, 1] and source != destination.
    pub fn is_valid_structure(&self) -> bool {
        self.source_deck <= 1 && self.destination_deck <= 1 && self.source_deck != self.destination_deck
    }

    /// Sanitized copy: non-finite -> safe default, bounds clamp, monotonic phases.
    /// (Mirrors the C++ `detail::sanitizePlan` contract field for field.)
    #[must_use]
    pub fn sanitize(&self) -> TransitionPlan {
        let mut p = self.clone();

        p.duration_seconds = if p.duration_seconds.is_finite() {
            p.duration_seconds.clamp(0.1, 600.0)
        } else {
            16.0
        };
        p.src_tempo_ratio = Self::sanitize_f32(p.src_tempo_ratio, 0.5, 2.0, 1.0);
        p.dst_tempo_ratio = Self::sanitize_f32(p.dst_tempo_ratio, 0.5, 2.0, 1.0);
        p.dst_tempo_ramp_seconds = Self::sanitize_f32(p.dst_tempo_ramp_seconds, 0.0, 300.0, 0.0);
        p.src_gain = Self::sanitize_f32(p.src_gain, 0.0, 1.0, 1.0);
        p.dst_gain = Self::sanitize_f32(p.dst_gain, 0.0, 1.0, 1.0);

        for band in &mut p.src_eq {
            *band = Self::sanitize_f32(*band, -1.0, 1.0, 0.0);
        }
        for band in &mut p.dst_eq {
            *band = Self::sanitize_f32(*band, -1.0, 1.0, 0.0);
        }
        p.src_filter = Self::sanitize_f32(p.src_filter, -1.0, 1.0, 0.0);
        p.dst_filter = Self::sanitize_f32(p.dst_filter, -1.0, 1.0, 0.0);
        p.src_vocal_stem = Self::sanitize_f32(p.src_vocal_stem, 0.0, 1.0, 1.0);
        p.dst_vocal_stem = Self::sanitize_f32(p.dst_vocal_stem, 0.0, 1.0, 1.0);
        p.crossfader_start = Self::sanitize_f32(p.crossfader_start, -1.0, 1.0, -1.0);
        p.crossfader_end = Self::sanitize_f32(p.crossfader_end, -1.0, 1.0, 1.0);

        p.phase_sync_end = Self::sanitize_f32(p.phase_sync_end, 0.0, 1.0, 0.5);
        p.phase_eq_end = Self::sanitize_f32(p.phase_eq_end, 0.0, 1.0, 0.875);
        p.phase_vocal_end = Self::sanitize_f32(p.phase_vocal_end, 0.0, 1.0, 1.0);
        // Force monotonic phase boundaries: sync <= eq <= vocal.
        let p1 = f64::from(p.phase_sync_end);
        let p2 = p1.max(f64::from(p.phase_eq_end));
        let p3 = p2.max(f64::from(p.phase_vocal_end));
        p.phase_sync_end = p1 as f32;
        p.phase_eq_end = p2 as f32;
        p.phase_vocal_end = p3 as f32;

        p.bass_swap_point = Self::sanitize_f32(p.bass_swap_point, 0.1, 0.9, 0.5);
        p.bass_swap_window = Self::sanitize_f32(p.bass_swap_window, 0.02, 0.5, 0.1);
        p.confidence = Self::sanitize_f32(p.confidence, 0.0, 1.0, 1.0);

        p
    }

    /// Maps the (sanitized) plan onto the versioned v2 C ABI command.
    pub fn into_transition_command(&self) -> TransitionCommandC {
        let p = self.sanitize();
        TransitionCommandC {
            version: 1,
            source_deck: p.source_deck,
            destination_deck: p.destination_deck,
            crossfader_curve: p.crossfader_curve.clamp(0, 2),
            flags: 0,
            _pad0: 0.0,
            duration_seconds: p.duration_seconds,
            src_tempo_ratio: p.src_tempo_ratio,
            dst_tempo_ratio: p.dst_tempo_ratio,
            dst_tempo_ramp_seconds: p.dst_tempo_ramp_seconds,
            src_gain: p.src_gain,
            dst_gain: p.dst_gain,
            src_low_eq: p.src_eq[0],
            src_mid_eq: p.src_eq[1],
            src_high_eq: p.src_eq[2],
            dst_low_eq: p.dst_eq[0],
            dst_mid_eq: p.dst_eq[1],
            dst_high_eq: p.dst_eq[2],
            src_filter: p.src_filter,
            dst_filter: p.dst_filter,
            src_vocal_stem: p.src_vocal_stem,
            dst_vocal_stem: p.dst_vocal_stem,
            crossfader_start: p.crossfader_start,
            crossfader_end: p.crossfader_end,
            phase_sync_end: p.phase_sync_end,
            phase_eq_end: p.phase_eq_end,
            phase_vocal_end: p.phase_vocal_end,
            bass_swap_point: p.bass_swap_point,
            bass_swap_window: p.bass_swap_window,
            transition_type: p.transition_type,
            _pad1: 0,
        }
    }
}

#[cfg(test)]
#[allow(clippy::float_cmp)] // deterministic sanitize/clamp results compared exactly
mod tests {
    use super::*;

    fn nan_plan() -> TransitionPlan {
        #[allow(clippy::field_reassign_with_default)]
        {
        let mut p = TransitionPlan::default();
        p.duration_seconds = f64::NAN;
        p.src_tempo_ratio = f32::INFINITY;
        p.dst_tempo_ratio = -f32::INFINITY;
        p.dst_tempo_ramp_seconds = f32::NAN;
        p.src_gain = 2.0;
        p.dst_gain = -3.0;
        p.src_eq = [f32::NAN, 5.0, -5.0];
        p.dst_eq = [1.5, -1.5, f32::INFINITY];
        p.src_filter = f32::NAN;
        p.dst_filter = 4.0;
        p.src_vocal_stem = -1.0;
        p.dst_vocal_stem = 9.0;
        p.crossfader_start = 8.0;
        p.crossfader_end = -8.0;
        p.phase_sync_end = 0.9;
        p.phase_eq_end = 0.3;
        p.phase_vocal_end = 0.2;
        p.bass_swap_point = 1.5;
        p.bass_swap_window = 0.001;
        p.confidence = f32::NAN;
        p
        }
    }

    #[test]
    fn sanitize_replaces_non_finite_and_clamps() {
        let p = nan_plan().sanitize();
        assert_eq!(p.duration_seconds, 16.0);
        assert_eq!(p.src_tempo_ratio, 1.0); // +Inf -> safe default
        assert_eq!(p.dst_tempo_ratio, 1.0); // -Inf -> safe default
        assert_eq!(p.dst_tempo_ramp_seconds, 0.0);
        assert_eq!(p.src_gain, 1.0);
        assert_eq!(p.dst_gain, 0.0);
        assert_eq!(p.src_eq, [0.0, 1.0, -1.0]);
        assert_eq!(p.dst_eq, [1.0, -1.0, 0.0]); // +Inf -> default 0.0
        assert_eq!(p.src_filter, 0.0);
        assert_eq!(p.dst_filter, 1.0);
        assert_eq!(p.src_vocal_stem, 0.0);
        assert_eq!(p.dst_vocal_stem, 1.0);
        assert_eq!(p.crossfader_start, 1.0); // 8.0 clamps to upper bound
        assert_eq!(p.crossfader_end, -1.0); // -8.0 clamps to lower bound
        // Monotonic phase cascade: 0.9 / 0.9 / 0.9.
        assert_eq!(p.phase_sync_end, 0.9);
        assert_eq!(p.phase_eq_end, 0.9);
        assert_eq!(p.phase_vocal_end, 0.9);
        assert_eq!(p.bass_swap_point, 0.9);
        assert_eq!(p.bass_swap_window, 0.02);
        assert_eq!(p.confidence, 1.0);
    }

    #[test]
    #[allow(clippy::field_reassign_with_default)]
    fn structure_rejects_same_deck_and_invalid_ids() {
        let mut p = TransitionPlan::default();
        assert!(p.is_valid_structure());
        p.source_deck = 1;
        p.destination_deck = 1;
        assert!(!p.is_valid_structure());
        p.source_deck = 7;
        assert!(!p.is_valid_structure());
    }

    #[test]
    fn into_command_matches_c_v2_layout_and_defaults() {
        let p = nan_plan();
        let c = p.into_transition_command();
        assert_eq!(c.version, 1);
        assert_eq!(c.source_deck, 0);
        assert_eq!(c.destination_deck, 1);
        assert_eq!(c.duration_seconds, 16.0);
        assert_eq!(c.transition_type, 0);
        assert_eq!(c.bass_swap_point, 0.9);
        assert_eq!(c.phase_vocal_end, 0.9);
    }

    #[test]
    #[allow(clippy::field_reassign_with_default)]
    fn classic_eq_blend_plan_maps_cleanly() {
        let mut p = TransitionPlan::default();
        p.transition_type = TRANSITION_TYPE_CLASSIC_EQ_BLEND;
        p.duration_seconds = 8.0;
        p.dst_tempo_ratio = 0.94;
        p.dst_tempo_ramp_seconds = 3.0;
        p.dst_gain = 0.92;
        let c = p.into_transition_command();
        assert_eq!(c.transition_type, TRANSITION_TYPE_CLASSIC_EQ_BLEND);
        assert!((c.duration_seconds - 8.0).abs() < f64::EPSILON);
        assert_eq!(c.dst_tempo_ratio, 0.94);
        assert_eq!(c.dst_tempo_ramp_seconds, 3.0);
        assert_eq!(c.dst_gain, 0.92);
    }

    #[test]
    #[allow(clippy::field_reassign_with_default)]
    fn serde_roundtrip() {
        let mut p = TransitionPlan::default();
        p.source_track_id = "t1".to_string();
        p.destination_track_id = "t2".to_string();
        let json = serde_json::to_string(&p).expect("serialize");
        let back: TransitionPlan = serde_json::from_str(&json).expect("deserialize");
        assert_eq!(back.source_track_id, "t1");
        assert_eq!(back.destination_track_id, "t2");
        assert!((back.duration_seconds - 16.0).abs() < f64::EPSILON);
    }
}
