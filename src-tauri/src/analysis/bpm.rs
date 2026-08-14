use crate::models::TempoProfile;

pub struct BpmDetector;

impl BpmDetector {
    pub fn new() -> Self {
        Self
    }

    /// Analyzes PCM audio buffer to estimate BPM and candidate tempo hypotheses.
    pub fn analyze(&self, _samples: &[f32], _sample_rate: u32) -> TempoProfile {
        TempoProfile {
            bpm: 120.0,
            bpm_confidence: 0.0,
            alternative_bpm_hypotheses: vec![60.0, 240.0],
            beat_positions: Vec::new(),
            downbeat_positions: Vec::new(),
            bar_positions: Vec::new(),
            grid_offset_seconds: 0.0,
        }
    }
}

impl Default for BpmDetector {
    fn default() -> Self {
        Self::new()
    }
}
