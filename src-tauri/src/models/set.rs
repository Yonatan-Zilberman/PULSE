use super::transition::TransitionPlan;
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct EnergyTarget {
    pub target_energy: u8, // 1 to 10
    pub duration_minutes: u32,
    pub curve_type: String, // e.g. "steady", "build_and_drop", "wave"
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct SetPlan {
    pub set_id: String,
    pub energy_target: EnergyTarget,
    pub tracks: Vec<String>, // ordered list of track IDs
    pub transitions: Vec<TransitionPlan>,
    pub backup_tracks: Vec<String>,
    pub confidence: f32,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_set_plan_roundtrip() {
        let set_plan = SetPlan {
            set_id: "set-001".to_string(),
            energy_target: EnergyTarget {
                target_energy: 8,
                duration_minutes: 60,
                curve_type: "wave".to_string(),
            },
            tracks: vec![
                "trk-1".to_string(),
                "trk-2".to_string(),
                "trk-3".to_string(),
            ],
            transitions: vec![],
            backup_tracks: vec!["trk-4".to_string()],
            confidence: 0.95,
        };

        let json = serde_json::to_string(&set_plan).expect("Failed to serialize SetPlan");
        let deserialized: SetPlan =
            serde_json::from_str(&json).expect("Failed to deserialize SetPlan");

        assert_eq!(set_plan, deserialized);
        assert_eq!(deserialized.tracks.len(), 3);
    }
}
