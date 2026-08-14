use crate::models::EnergyTarget;

pub struct EnergyPlanner;

impl EnergyPlanner {
    pub fn new() -> Self {
        Self
    }

    /// Computes target energy trajectory across the set duration.
    pub fn compute_trajectory(&self, target: &EnergyTarget) -> Vec<f32> {
        let points = (target.duration_minutes * 2) as usize;
        vec![f32::from(target.target_energy); points]
    }
}

impl Default for EnergyPlanner {
    fn default() -> Self {
        Self::new()
    }
}
