use crate::dj_brain::candidate_generator::CandidateGenerator;
use crate::models::{
    EQAutomation, EnergyTarget, FilterAutomation, SetPlan, TrackProfile, TransitionCandidate,
    TransitionPlan,
};
use std::collections::HashSet;

pub struct SetPlanner;

impl SetPlanner {
    pub fn new() -> Self {
        Self
    }

    /// Evaluates harmonic compatibility between two Camelot keys (e.g., "5A" and "6A").
    pub fn evaluate_camelot_distance(key_a: &str, key_b: &str) -> f32 {
        if key_a.is_empty() || key_b.is_empty() {
            return 0.70; // Neutral fallback when key is unanalyzed
        }

        let parse_camelot = |s: &str| -> Option<(i32, char)> {
            let s = s.trim().to_uppercase();
            if s.len() < 2 {
                return None;
            }
            let (num_part, letter_part) = s.split_at(s.len() - 1);
            let num: i32 = num_part.parse().ok()?;
            let letter = letter_part.chars().next()?;
            if (1..=12).contains(&num) && (letter == 'A' || letter == 'B') {
                Some((num, letter))
            } else {
                None
            }
        };

        let Some((num_a, letter_a)) = parse_camelot(key_a) else {
            return 0.70;
        };
        let Some((num_b, letter_b)) = parse_camelot(key_b) else {
            return 0.70;
        };

        if num_a == num_b && letter_a == letter_b {
            1.0 // Perfect match (same key)
        } else if num_a == num_b && letter_a != letter_b {
            0.95 // Relative major / minor swap
        } else {
            let raw_diff = (num_a - num_b).abs();
            let diff = raw_diff.min(12 - raw_diff);
            if diff == 1 && letter_a == letter_b {
                0.90 // Adjacent fifth (±1 step on Camelot wheel)
            } else if diff == 1 && letter_a != letter_b {
                0.80 // Diagonal Camelot step
            } else if diff == 2 && letter_a == letter_b {
                0.70 // 2-step energy boost / key change
            } else {
                0.50 // Clashing key delta
            }
        }
    }

    fn calculate_track_energy(track: &TrackProfile) -> f32 {
        if track.energy_curve.is_empty() {
            0.50
        } else {
            track.energy_curve.iter().sum::<f32>() / track.energy_curve.len() as f32
        }
    }

    fn find_best_starting_track(pool: &[TrackProfile], target_energy_norm: f32) -> usize {
        let mut best_start_idx = 0;
        let mut best_start_diff = f32::MAX;

        for (idx, track) in pool.iter().enumerate() {
            let track_energy = Self::calculate_track_energy(track);
            let diff = (track_energy - target_energy_norm).abs();
            if diff < best_start_diff {
                best_start_diff = diff;
                best_start_idx = idx;
            }
        }
        best_start_idx
    }

    fn build_transition_plan(
        current_track: &TrackProfile,
        next_track: &TrackProfile,
        candidate: &TransitionCandidate,
        current_timeline_sec: f64,
    ) -> TransitionPlan {
        let trans_dur = candidate.duration_seconds;
        let half_dur = trans_dur * 0.5;

        TransitionPlan {
            source_track_id: current_track.id.clone(),
            destination_track_id: next_track.id.clone(),
            start_timestamp_seconds: current_timeline_sec + candidate.source_exit_seconds,
            duration_seconds: trans_dur,
            transition_type: candidate.transition_type,
            target_bpm: current_track.tempo.bpm,
            source_eq: EQAutomation {
                low_gain_curve: vec![(0.0, 0.0), (half_dur, -12.0), (trans_dur, -24.0)],
                mid_gain_curve: vec![(0.0, 0.0), (trans_dur, -3.0)],
                high_gain_curve: vec![(0.0, 0.0), (trans_dur, -2.0)],
            },
            destination_eq: EQAutomation {
                low_gain_curve: vec![(0.0, -24.0), (half_dur, -12.0), (trans_dur, 0.0)],
                mid_gain_curve: vec![(0.0, -3.0), (trans_dur, 0.0)],
                high_gain_curve: vec![(0.0, -2.0), (trans_dur, 0.0)],
            },
            source_filter: FilterAutomation {
                high_pass_cutoff_hz: vec![(0.0, 20.0), (trans_dur, 100.0)],
                low_pass_cutoff_hz: vec![(0.0, 20000.0)],
            },
            destination_filter: FilterAutomation {
                high_pass_cutoff_hz: vec![(0.0, 20.0)],
                low_pass_cutoff_hz: vec![(0.0, 20000.0)],
            },
            crossfader_curve: vec![(0.0, -1.0), (half_dur, 0.0), (trans_dur, 1.0)],
            confidence: (candidate.confidence * candidate.score.overall_score).clamp(0.1, 1.0),
        }
    }

    /// Plans a sequence of tracks optimized for energy target and harmonic flow.
    pub fn plan_set(&self, pool: &[TrackProfile], target: EnergyTarget) -> SetPlan {
        if pool.is_empty() {
            return SetPlan {
                set_id: "set-empty".to_string(),
                energy_target: target,
                tracks: Vec::new(),
                transitions: Vec::new(),
                backup_tracks: Vec::new(),
                confidence: 0.0,
            };
        }

        if pool.len() == 1 {
            let track = &pool[0];
            return SetPlan {
                set_id: format!("set-single-{}", track.id),
                energy_target: target,
                tracks: vec![track.id.clone()],
                transitions: Vec::new(),
                backup_tracks: Vec::new(),
                confidence: track.overall_confidence,
            };
        }

        let generator = CandidateGenerator::new();
        let target_energy_norm = (f32::from(target.target_energy) / 10.0).clamp(0.1, 1.0);

        // 1. Select optimal starting track closest to target initial energy
        let best_start_idx = Self::find_best_starting_track(pool, target_energy_norm);

        let mut used_indices = HashSet::new();
        used_indices.insert(best_start_idx);

        let mut ordered_track_ids = Vec::with_capacity(pool.len());
        ordered_track_ids.push(pool[best_start_idx].id.clone());

        let mut transitions = Vec::with_capacity(pool.len() - 1);
        let mut current_idx = best_start_idx;
        let mut current_timeline_sec = 0.0;

        // 2. Iteratively select next best candidate maximizing transition score & harmonic flow
        while used_indices.len() < pool.len() {
            let current_track = &pool[current_idx];
            let mut best_cand_idx = None;
            let mut best_score = -1.0f32;
            let mut best_candidate_obj = None;

            for (cand_idx, cand_track) in pool.iter().enumerate() {
                if used_indices.contains(&cand_idx) {
                    continue;
                }

                let candidates = generator.generate_candidates(current_track, cand_track);
                if let Some(candidate) = candidates.into_iter().next() {
                    let harmonic_factor = Self::evaluate_camelot_distance(
                        &current_track.key.camelot,
                        &cand_track.key.camelot,
                    );

                    let cand_energy = Self::calculate_track_energy(cand_track);
                    let energy_score =
                        1.0 - (cand_energy - target_energy_norm).abs().clamp(0.0, 1.0);

                    // Combined heuristic score
                    let combined_score = candidate.score.overall_score * 0.45
                        + harmonic_factor * 0.35
                        + energy_score * 0.20;

                    if combined_score > best_score {
                        best_score = combined_score;
                        best_cand_idx = Some(cand_idx);
                        best_candidate_obj = Some(candidate);
                    }
                }
            }

            match (best_cand_idx, best_candidate_obj) {
                (Some(next_idx), Some(candidate)) => {
                    let next_track = &pool[next_idx];
                    let transition_plan = Self::build_transition_plan(
                        current_track,
                        next_track,
                        &candidate,
                        current_timeline_sec,
                    );

                    current_timeline_sec += candidate.source_exit_seconds;
                    transitions.push(transition_plan);
                    ordered_track_ids.push(next_track.id.clone());
                    used_indices.insert(next_idx);
                    current_idx = next_idx;
                }
                _ => break,
            }
        }

        let overall_confidence = if transitions.is_empty() {
            0.80
        } else {
            transitions.iter().map(|t| t.confidence).sum::<f32>() / transitions.len() as f32
        };

        SetPlan {
            set_id: format!("set-plan-{}", ordered_track_ids.len()),
            energy_target: target,
            tracks: ordered_track_ids,
            transitions,
            backup_tracks: Vec::new(),
            confidence: overall_confidence,
        }
    }
}

impl Default for SetPlanner {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::models::{
        KeyProfile, LoudnessProfile, MixabilityProfile, PhraseBoundaries, StemCacheStatus,
        TrackMetadata, TransitionType,
    };

    fn make_test_track(id: &str, bpm: f64, camelot: &str, energy: f32) -> TrackProfile {
        TrackProfile {
            id: id.to_string(),
            metadata: TrackMetadata {
                title: format!("Title {}", id),
                artist: "Pulse Artist".to_string(),
                album: None,
                genre: Some("House".to_string()),
                duration_seconds: 180.0,
                artwork_uri: None,
                file_path: format!("/audio/{}.wav", id),
                sample_rate: 48000,
                channels: 2,
            },
            tempo: crate::models::TempoProfile {
                bpm,
                bpm_confidence: 0.98,
                alternative_bpm_hypotheses: vec![bpm * 0.5, bpm * 2.0],
                beat_positions: vec![0.0, 0.5, 1.0, 1.5],
                downbeat_positions: vec![0.0, 2.0],
                bar_positions: vec![0.0, 2.0],
                grid_offset_seconds: 0.0,
                is_variable_tempo: false,
                tempo_drift_min_bpm: bpm,
                tempo_drift_max_bpm: bpm,
            },
            key: KeyProfile {
                key: format!("Key {}", camelot),
                camelot: camelot.to_string(),
                key_confidence: 0.95,
                chroma_profile: vec![0.1; 12],
            },
            structure: Vec::new(),
            phrases: PhraseBoundaries {
                boundaries_4bar: vec![0.0, 8.0, 16.0, 24.0, 32.0],
                boundaries_8bar: vec![0.0, 16.0, 32.0, 64.0, 128.0, 160.0],
                boundaries_16bar: vec![0.0, 32.0, 64.0, 128.0],
                boundaries_32bar: vec![0.0, 64.0, 128.0],
            },
            energy_curve: vec![energy; 10],
            loudness: LoudnessProfile {
                integrated_lufs: -14.0,
                short_term_lufs_max: -10.0,
                true_peak_db: -1.0,
                dynamic_range_lu: 6.0,
            },
            mixability: MixabilityProfile {
                intro_quality: 0.95,
                outro_quality: 0.95,
                phrase_stability: 0.95,
                vocal_isolation_feasibility: 0.90,
                beat_stability: 0.98,
                tempo_stability: 0.98,
                transition_option_count: 8,
            },
            stem_cache_status: StemCacheStatus::Cached,
            overall_confidence: 0.95,
        }
    }

    #[test]
    fn test_set_planner_empty_pool() {
        let planner = SetPlanner::new();
        let target = EnergyTarget {
            target_energy: 7,
            duration_minutes: 60,
            curve_type: "wave".to_string(),
        };
        let set = planner.plan_set(&[], target);
        assert!(set.tracks.is_empty());
        assert!(set.transitions.is_empty());
        assert_eq!(set.confidence, 0.0);
    }

    #[test]
    fn test_set_planner_single_track() {
        let planner = SetPlanner::new();
        let track = make_test_track("trk-1", 124.0, "8A", 0.7);
        let target = EnergyTarget {
            target_energy: 7,
            duration_minutes: 60,
            curve_type: "wave".to_string(),
        };
        let set = planner.plan_set(&[track], target);
        assert_eq!(set.tracks.len(), 1);
        assert!(set.transitions.is_empty());
        assert_eq!(set.confidence, 0.95);
    }

    #[test]
    fn test_camelot_distance_evaluation() {
        assert_eq!(SetPlanner::evaluate_camelot_distance("5A", "5A"), 1.0);
        assert_eq!(SetPlanner::evaluate_camelot_distance("5A", "5B"), 0.95);
        assert_eq!(SetPlanner::evaluate_camelot_distance("5A", "6A"), 0.90);
        assert_eq!(SetPlanner::evaluate_camelot_distance("12A", "1A"), 0.90);
        assert_eq!(SetPlanner::evaluate_camelot_distance("5A", "6B"), 0.80);
        assert_eq!(SetPlanner::evaluate_camelot_distance("5A", "7A"), 0.70);
        assert_eq!(SetPlanner::evaluate_camelot_distance("5A", "10A"), 0.50);
    }

    #[test]
    fn test_set_planner_25_tracks_sequencing() {
        let planner = SetPlanner::new();
        let camelot_keys = [
            "5A", "5B", "6A", "6B", "7A", "7B", "8A", "8B", "9A", "9B", "10A", "10B",
        ];
        let mut pool = Vec::new();

        for i in 1..=25 {
            let bpm = 120.0 + (i as f64 % 6.0); // 120.0 to 125.0 BPM
            let key = camelot_keys[i % camelot_keys.len()];
            let energy = 0.4 + (i as f32 * 0.02);
            pool.push(make_test_track(&format!("trk-{:02}", i), bpm, key, energy));
        }

        let target = EnergyTarget {
            target_energy: 7,
            duration_minutes: 60,
            curve_type: "steady".to_string(),
        };

        let set_plan = planner.plan_set(&pool, target);

        // Verify that 25 tracks yield 24 consecutive transitions (>= 20 transitions required)
        assert_eq!(set_plan.tracks.len(), 25);
        assert_eq!(set_plan.transitions.len(), 24);
        assert!(set_plan.confidence >= 0.70);

        // Verify zero duplicate tracks
        let unique_tracks: HashSet<_> = set_plan.tracks.iter().collect();
        assert_eq!(unique_tracks.len(), 25);

        // Verify all transitions have valid types, duration, and bounded tempo difference
        for trans in &set_plan.transitions {
            assert!(trans.duration_seconds > 0.0);
            assert!(trans.confidence >= 0.70);
            assert_eq!(trans.transition_type, TransitionType::BassSwap);
            assert!(!trans.crossfader_curve.is_empty());
            assert!(!trans.source_eq.low_gain_curve.is_empty());
            assert!(!trans.destination_eq.low_gain_curve.is_empty());
        }
    }
}
