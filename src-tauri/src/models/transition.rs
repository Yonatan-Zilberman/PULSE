use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum TransitionType {
    EqCrossfade,
    BassSwap,
    StemIsolation,
    DropSwap,
    EchoOut,
    BreakdownBlend,
    VocalHandoff,
    EnergyCut,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct CandidateScore {
    pub bpm_score: f32,
    pub harmonic_score: f32,
    pub phrase_score: f32,
    pub energy_score: f32,
    pub vocal_score: f32,
    pub bass_score: f32,
    pub structural_score: f32,
    pub overall_score: f32,
}

impl CandidateScore {
    pub fn is_valid(&self) -> bool {
        let in_range = |s: f32| (0.0..=1.0).contains(&s);
        in_range(self.bpm_score)
            && in_range(self.harmonic_score)
            && in_range(self.phrase_score)
            && in_range(self.energy_score)
            && in_range(self.vocal_score)
            && in_range(self.bass_score)
            && in_range(self.structural_score)
            && in_range(self.overall_score)
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct TransitionCandidate {
    pub source_track_id: String,
    pub destination_track_id: String,
    pub source_exit_seconds: f64,
    pub destination_entry_seconds: f64,
    pub duration_seconds: f64,
    pub transition_type: TransitionType,
    pub score: CandidateScore,
    pub artifact_risk: f32,
    pub confidence: f32,
    pub required_stems: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct EQAutomation {
    pub low_gain_curve: Vec<(f64, f32)>, // (time_offset, gain_db)
    pub mid_gain_curve: Vec<(f64, f32)>,
    pub high_gain_curve: Vec<(f64, f32)>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct FilterAutomation {
    pub high_pass_cutoff_hz: Vec<(f64, f32)>,
    pub low_pass_cutoff_hz: Vec<(f64, f32)>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct TransitionPlan {
    pub source_track_id: String,
    pub destination_track_id: String,
    pub start_timestamp_seconds: f64,
    pub duration_seconds: f64,
    pub transition_type: TransitionType,
    pub target_bpm: f64,
    pub source_eq: EQAutomation,
    pub destination_eq: EQAutomation,
    pub source_filter: FilterAutomation,
    pub destination_filter: FilterAutomation,
    pub crossfader_curve: Vec<(f64, f32)>, // (time_offset, crossfader_pos -1.0 to 1.0)
    pub confidence: f32,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_candidate_score_validation() {
        let valid_score = CandidateScore {
            bpm_score: 0.95,
            harmonic_score: 1.0,
            phrase_score: 0.9,
            energy_score: 0.85,
            vocal_score: 0.92,
            bass_score: 0.88,
            structural_score: 0.94,
            overall_score: 0.92,
        };
        assert!(valid_score.is_valid());

        let invalid_score = CandidateScore {
            bpm_score: 1.2,
            harmonic_score: 0.9,
            phrase_score: 0.9,
            energy_score: 0.85,
            vocal_score: 0.92,
            bass_score: 0.88,
            structural_score: 0.94,
            overall_score: 0.92,
        };
        assert!(!invalid_score.is_valid());
    }

    #[test]
    fn test_transition_plan_serialization() {
        let plan = TransitionPlan {
            source_track_id: "trk-1".to_string(),
            destination_track_id: "trk-2".to_string(),
            start_timestamp_seconds: 120.0,
            duration_seconds: 15.2,
            transition_type: TransitionType::BassSwap,
            target_bpm: 126.0,
            source_eq: EQAutomation {
                low_gain_curve: vec![(0.0, 0.0), (7.6, -24.0)],
                mid_gain_curve: vec![(0.0, 0.0), (15.2, -6.0)],
                high_gain_curve: vec![(0.0, 0.0), (15.2, -3.0)],
            },
            destination_eq: EQAutomation {
                low_gain_curve: vec![(0.0, -24.0), (7.6, 0.0)],
                mid_gain_curve: vec![(0.0, -6.0), (15.2, 0.0)],
                high_gain_curve: vec![(0.0, -3.0), (15.2, 0.0)],
            },
            source_filter: FilterAutomation {
                high_pass_cutoff_hz: vec![(0.0, 20.0), (15.2, 200.0)],
                low_pass_cutoff_hz: vec![(0.0, 20000.0)],
            },
            destination_filter: FilterAutomation {
                high_pass_cutoff_hz: vec![(0.0, 20.0)],
                low_pass_cutoff_hz: vec![(0.0, 20000.0)],
            },
            crossfader_curve: vec![(0.0, -1.0), (7.6, 0.0), (15.2, 1.0)],
            confidence: 0.96,
        };

        let json = serde_json::to_string(&plan).expect("Failed to serialize TransitionPlan");
        let deserialized: TransitionPlan =
            serde_json::from_str(&json).expect("Failed to deserialize TransitionPlan");

        assert_eq!(plan, deserialized);
        assert_eq!(deserialized.transition_type, TransitionType::BassSwap);
    }
}
