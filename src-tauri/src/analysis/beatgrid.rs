pub struct BeatgridDetector;

impl BeatgridDetector {
    pub fn new() -> Self {
        Self
    }

    /// Computes beat positions and downbeat alignment for deterministic phase alignment.
    pub fn align_grid(&self, bpm: f64, offset_seconds: f64, duration_seconds: f64) -> Vec<f64> {
        let beat_interval = 60.0 / bpm;
        let mut beats = Vec::new();
        let mut pos = offset_seconds;
        while pos < duration_seconds {
            beats.push(pos);
            pos += beat_interval;
        }
        beats
    }
}

impl Default for BeatgridDetector {
    fn default() -> Self {
        Self::new()
    }
}
