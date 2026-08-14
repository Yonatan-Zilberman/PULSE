use crate::models::{CandidateScore, TrackProfile, TransitionCandidate, TransitionType};

pub struct CandidateGenerator;

#[derive(Debug, Clone, PartialEq)]
pub struct TempoCompatibility {
    pub bpm_score: f32,
    pub artifact_risk: f32,
    pub is_octave_compatible: bool,
    pub effective_tempo_ratio: f64,
    pub bpm_diff_norm: f64,
    pub recommended_transition_type: TransitionType,
}

impl CandidateGenerator {
    pub fn new() -> Self {
        Self
    }

    /// Evaluates tempo difference, bounded stretch risk, octave hypotheses, and artifact penalty.
    pub fn evaluate_tempo(outgoing: &TrackProfile, incoming: &TrackProfile) -> TempoCompatibility {
        let bpm_a = if outgoing.tempo.bpm > 0.0 {
            outgoing.tempo.bpm
        } else {
            120.0
        };
        let bpm_b = if incoming.tempo.bpm > 0.0 {
            incoming.tempo.bpm
        } else {
            120.0
        };

        // 1. Octave Check (Half-time / Double-time e.g., 70 <-> 140 BPM)
        let double_b = bpm_b * 2.0;
        let half_b = bpm_b * 0.5;

        let mut is_octave =
            (double_b - bpm_a).abs() / bpm_a <= 0.03 || (half_b - bpm_a).abs() / bpm_a <= 0.03;

        if !is_octave {
            for &alt in &incoming.tempo.alternative_bpm_hypotheses {
                if (alt - bpm_a).abs() / bpm_a <= 0.03 {
                    is_octave = true;
                    break;
                }
            }
        }
        if !is_octave {
            for &alt in &outgoing.tempo.alternative_bpm_hypotheses {
                if (alt - bpm_b).abs() / bpm_b <= 0.03 {
                    is_octave = true;
                    break;
                }
            }
        }

        if is_octave {
            return TempoCompatibility {
                bpm_score: 0.85,
                artifact_risk: 0.05,
                is_octave_compatible: true,
                effective_tempo_ratio: 1.0,
                bpm_diff_norm: 0.0,
                recommended_transition_type: TransitionType::BreakdownBlend,
            };
        }

        // 2. Direct Tempo Difference
        let bpm_diff_norm = (bpm_a - bpm_b).abs() / bpm_a;
        let effective_tempo_ratio = bpm_a / bpm_b;

        let (bpm_score, artifact_risk, recommended_type) = if bpm_diff_norm <= 0.03 {
            // Small delta (<= 3%)
            let score = 1.0 - (bpm_diff_norm / 0.03) as f32 * 0.10;
            let risk = (bpm_diff_norm / 0.03) as f32 * 0.05;
            (
                score.clamp(0.90, 1.0),
                risk.clamp(0.0, 0.05),
                TransitionType::BassSwap,
            )
        } else if bpm_diff_norm <= 0.06 {
            // Moderate delta (3% to 6%)
            let excess = (bpm_diff_norm - 0.03) / 0.03;
            let score = 0.90 - excess as f32 * 0.40;
            let risk = 0.15 + (excess * excess) as f32 * 0.25;
            (
                score.clamp(0.50, 0.90),
                risk.clamp(0.15, 0.40),
                TransitionType::BassSwap,
            )
        } else {
            // Excessive delta (> 6%)
            let excess = (bpm_diff_norm - 0.06) as f32;
            let score = (0.50 - excess * 5.0).max(0.0);
            let risk = (0.70 + excess * 2.0).min(1.0);
            (
                score.clamp(0.0, 0.50),
                risk.clamp(0.70, 1.0),
                TransitionType::EnergyCut,
            )
        };

        TempoCompatibility {
            bpm_score,
            artifact_risk,
            is_octave_compatible: false,
            effective_tempo_ratio,
            bpm_diff_norm,
            recommended_transition_type: recommended_type,
        }
    }

    /// Evaluates transition compatibility between incoming and outgoing tracks.
    pub fn generate_candidates(
        &self,
        outgoing: &TrackProfile,
        incoming: &TrackProfile,
    ) -> Vec<TransitionCandidate> {
        let tempo_comp = Self::evaluate_tempo(outgoing, incoming);

        // 1. Dynamic Phrase Compatibility Evaluation
        let base_phrase_score: f32 = if !outgoing.phrases.boundaries_8bar.is_empty()
            && !incoming.phrases.boundaries_8bar.is_empty()
        {
            0.95
        } else if !outgoing.phrases.boundaries_4bar.is_empty()
            && !incoming.phrases.boundaries_4bar.is_empty()
        {
            0.90
        } else if !outgoing.phrases.boundaries_4bar.is_empty()
            || !incoming.phrases.boundaries_4bar.is_empty()
        {
            0.75
        } else {
            0.50
        };

        let stability_factor = outgoing
            .mixability
            .phrase_stability
            .min(incoming.mixability.phrase_stability)
            .clamp(0.5, 1.0);
        let phrase_score = (base_phrase_score * stability_factor).clamp(0.0, 1.0);

        let harmonic_score = 0.90;
        let energy_score = 0.88;
        let vocal_score = 0.90;
        let bass_score = 0.85;
        let structural_score = 0.92;

        let overall_score = (tempo_comp.bpm_score * 0.35
            + harmonic_score * 0.25
            + phrase_score * 0.15
            + energy_score * 0.10
            + vocal_score * 0.05
            + bass_score * 0.05
            + structural_score * 0.05)
            .clamp(0.0, 1.0);

        let score = CandidateScore {
            bpm_score: tempo_comp.bpm_score,
            harmonic_score,
            phrase_score,
            energy_score,
            vocal_score,
            bass_score,
            structural_score,
            overall_score,
        };

        // 2. Phrase-Aligned Duration Calculation
        let target_bpm = if outgoing.tempo.bpm > 0.0 {
            outgoing.tempo.bpm
        } else {
            120.0
        };
        let sec_per_bar = 4.0 * (60.0 / target_bpm);

        let duration_seconds =
            if tempo_comp.recommended_transition_type == TransitionType::EnergyCut {
                2.0
            } else {
                let dur_8bar = 8.0 * sec_per_bar;
                if outgoing.metadata.duration_seconds >= dur_8bar {
                    (dur_8bar * 100.0).round() / 100.0
                } else {
                    let dur_4bar = 4.0 * sec_per_bar;
                    (dur_4bar.min(outgoing.metadata.duration_seconds * 0.5) * 100.0).round() / 100.0
                }
            };

        // 3. Phrase-Aligned Exit and Entry Alignment
        let mut exit_seconds = (outgoing.metadata.duration_seconds - duration_seconds).max(0.0);
        if tempo_comp.recommended_transition_type != TransitionType::EnergyCut
            && !outgoing.phrases.boundaries_8bar.is_empty()
        {
            let mut best_boundary = outgoing.phrases.boundaries_8bar[0];
            for &b in &outgoing.phrases.boundaries_8bar {
                if b + duration_seconds <= outgoing.metadata.duration_seconds + 0.05 {
                    best_boundary = b;
                }
            }
            exit_seconds = best_boundary;
        }

        let entry_seconds = incoming
            .phrases
            .boundaries_8bar
            .first()
            .copied()
            .unwrap_or(0.0);

        let candidate = TransitionCandidate {
            source_track_id: outgoing.id.clone(),
            destination_track_id: incoming.id.clone(),
            source_exit_seconds: exit_seconds,
            destination_entry_seconds: entry_seconds,
            duration_seconds,
            transition_type: tempo_comp.recommended_transition_type,
            score,
            artifact_risk: tempo_comp.artifact_risk,
            confidence: outgoing.overall_confidence.min(incoming.overall_confidence),
            required_stems: Vec::new(),
        };

        vec![candidate]
    }
}

impl Default for CandidateGenerator {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::models::{
        KeyProfile, LoudnessProfile, MixabilityProfile, PhraseBoundaries, StemCacheStatus,
        TrackMetadata,
    };

    fn make_test_profile(id: &str, bpm: f64, alt_hypotheses: Vec<f64>) -> TrackProfile {
        TrackProfile {
            id: id.to_string(),
            metadata: TrackMetadata {
                title: format!("Track {}", id),
                artist: "Test Artist".to_string(),
                album: None,
                genre: None,
                duration_seconds: 180.0,
                artwork_uri: None,
                file_path: format!("/music/{}.wav", id),
                sample_rate: 48000,
                channels: 2,
            },
            tempo: crate::models::TempoProfile {
                bpm,
                bpm_confidence: 0.95,
                alternative_bpm_hypotheses: alt_hypotheses,
                beat_positions: vec![0.0, 0.5, 1.0],
                downbeat_positions: vec![0.0, 2.0],
                bar_positions: vec![0.0, 2.0],
                grid_offset_seconds: 0.0,
                is_variable_tempo: false,
                tempo_drift_min_bpm: bpm,
                tempo_drift_max_bpm: bpm,
            },
            key: KeyProfile {
                key: "C Minor".to_string(),
                camelot: "5A".to_string(),
                key_confidence: 0.90,
                chroma_profile: vec![0.1; 12],
            },
            structure: Vec::new(),
            phrases: PhraseBoundaries {
                boundaries_4bar: vec![0.0, 8.0],
                boundaries_8bar: vec![0.0, 16.0],
                boundaries_16bar: vec![0.0, 32.0],
                boundaries_32bar: vec![0.0, 64.0],
            },
            energy_curve: vec![0.5; 10],
            loudness: LoudnessProfile {
                integrated_lufs: -14.0,
                short_term_lufs_max: -10.0,
                true_peak_db: -1.0,
                dynamic_range_lu: 6.0,
            },
            mixability: MixabilityProfile {
                intro_quality: 0.9,
                outro_quality: 0.9,
                phrase_stability: 0.9,
                vocal_isolation_feasibility: 0.9,
                beat_stability: 0.95,
                tempo_stability: 0.95,
                transition_option_count: 5,
            },
            stem_cache_status: StemCacheStatus::Cached,
            overall_confidence: 0.95,
        }
    }

    #[test]
    fn test_small_tempo_difference() {
        let track_a = make_test_profile("trk-a", 120.0, vec![60.0, 240.0]);
        let track_b = make_test_profile("trk-b", 122.0, vec![61.0, 244.0]);

        let generator = CandidateGenerator::new();
        let candidates = generator.generate_candidates(&track_a, &track_b);

        assert_eq!(candidates.len(), 1);
        let candidate = &candidates[0];

        assert!(candidate.score.bpm_score >= 0.90);
        assert!(candidate.artifact_risk <= 0.05);
        assert_eq!(candidate.transition_type, TransitionType::BassSwap);
        assert!(candidate.score.is_valid());
    }

    #[test]
    fn test_moderate_tempo_difference() {
        let track_a = make_test_profile("trk-a", 120.0, vec![60.0, 240.0]);
        let track_b = make_test_profile("trk-b", 126.0, vec![63.0, 252.0]);

        let generator = CandidateGenerator::new();
        let candidates = generator.generate_candidates(&track_a, &track_b);

        assert_eq!(candidates.len(), 1);
        let candidate = &candidates[0];

        // Delta = 5.0% -> Score in [0.50, 0.90], Risk in [0.15, 0.40]
        assert!(candidate.score.bpm_score >= 0.50 && candidate.score.bpm_score <= 0.90);
        assert!(candidate.artifact_risk >= 0.15 && candidate.artifact_risk <= 0.40);
        assert_eq!(candidate.transition_type, TransitionType::BassSwap);
        assert!(candidate.score.is_valid());
    }

    #[test]
    fn test_excessive_tempo_difference() {
        let track_a = make_test_profile("trk-a", 120.0, vec![]);
        let track_b = make_test_profile("trk-b", 150.0, vec![]);

        let generator = CandidateGenerator::new();
        let candidates = generator.generate_candidates(&track_a, &track_b);

        assert_eq!(candidates.len(), 1);
        let candidate = &candidates[0];

        // Delta = 25% -> Score <= 0.10, Risk >= 0.70, fallback to EnergyCut
        assert!(candidate.score.bpm_score <= 0.10);
        assert!(candidate.artifact_risk >= 0.70);
        assert_eq!(candidate.transition_type, TransitionType::EnergyCut);
        assert!(candidate.score.is_valid());
    }

    #[test]
    fn test_octave_compatibility() {
        let track_70 = make_test_profile("trk-70", 70.0, vec![140.0]);
        let track_140 = make_test_profile("trk-140", 140.0, vec![70.0]);

        let generator = CandidateGenerator::new();
        let candidates = generator.generate_candidates(&track_70, &track_140);

        assert_eq!(candidates.len(), 1);
        let candidate = &candidates[0];

        // Octave match gives high BPM score (0.85) and low artifact risk (0.05) with BreakdownBlend
        assert_eq!(candidate.score.bpm_score, 0.85);
        assert_eq!(candidate.artifact_risk, 0.05);
        assert_eq!(candidate.transition_type, TransitionType::BreakdownBlend);
        assert!(candidate.score.is_valid());
    }

    #[test]
    fn test_phrase_aware_candidate_generation() {
        let track_a = make_test_profile("trk-a", 120.0, vec![]);
        let track_b = make_test_profile("trk-b", 120.0, vec![]);

        let generator = CandidateGenerator::new();
        let candidates = generator.generate_candidates(&track_a, &track_b);

        assert_eq!(candidates.len(), 1);
        let candidate = &candidates[0];

        // 8 bars at 120 BPM = 16.0s
        assert_eq!(candidate.duration_seconds, 16.0);
        assert!(candidate.score.phrase_score >= 0.85);
        assert!(candidate.score.is_valid());
    }

    #[test]
    fn test_empty_phrase_fallback_candidate_generation() {
        let mut track_a = make_test_profile("trk-a", 120.0, vec![]);
        let mut track_b = make_test_profile("trk-b", 120.0, vec![]);
        track_a.phrases.boundaries_8bar.clear();
        track_a.phrases.boundaries_4bar.clear();
        track_b.phrases.boundaries_8bar.clear();
        track_b.phrases.boundaries_4bar.clear();

        let generator = CandidateGenerator::new();
        let candidates = generator.generate_candidates(&track_a, &track_b);

        assert_eq!(candidates.len(), 1);
        let candidate = &candidates[0];

        assert!(candidate.score.phrase_score <= 0.60);
        assert!(candidate.score.is_valid());
    }
}
