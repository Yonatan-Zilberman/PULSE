pub struct BeatgridDetector;

impl BeatgridDetector {
    pub fn new() -> Self {
        Self
    }

    /// Computes beat positions and downbeat alignment for deterministic phase alignment.
    pub fn align_grid(&self, bpm: f64, offset_seconds: f64, duration_seconds: f64) -> Vec<f64> {
        if bpm <= 0.0 || duration_seconds <= 0.0 {
            return Vec::new();
        }

        let beat_interval = 60.0 / bpm;
        let mut beats = Vec::new();
        let mut pos = offset_seconds.max(0.0);
        while pos < duration_seconds {
            beats.push((pos * 1000.0).round() / 1000.0);
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_beatgrid_alignment() {
        let detector = BeatgridDetector::new();
        let beats = detector.align_grid(120.0, 0.0, 2.0);
        assert_eq!(beats.len(), 4);
        assert_eq!(beats[0], 0.0);
        assert_eq!(beats[1], 0.5);
        assert_eq!(beats[2], 1.0);
        assert_eq!(beats[3], 1.5);
    }

    #[test]
    fn test_beatgrid_invalid_inputs() {
        let detector = BeatgridDetector::new();
        assert!(detector.align_grid(0.0, 0.0, 2.0).is_empty());
        assert!(detector.align_grid(120.0, 0.0, 0.0).is_empty());
    }
}
