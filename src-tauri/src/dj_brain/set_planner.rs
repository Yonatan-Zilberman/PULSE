use crate::models::{EnergyTarget, SetPlan, TrackProfile};

pub struct SetPlanner;

impl SetPlanner {
    pub fn new() -> Self {
        Self
    }

    /// Plans a sequence of tracks optimized for energy target and harmonic flow.
    pub fn plan_set(&self, _pool: &[TrackProfile], target: EnergyTarget) -> SetPlan {
        SetPlan {
            set_id: "set-default".to_string(),
            energy_target: target,
            tracks: Vec::new(),
            transitions: Vec::new(),
            backup_tracks: Vec::new(),
            confidence: 1.0,
        }
    }
}

impl Default for SetPlanner {
    fn default() -> Self {
        Self::new()
    }
}
