pub struct BeatgridDetector;

impl BeatgridDetector {
    pub fn new() -> Self {
        Self
    }

    /// Computes beat positions for deterministic phase alignment.
    pub fn align_grid(&self, bpm: f64, offset_seconds: f64, duration_seconds: f64) -> Vec<f64> {
        let (beats, _, _) = self.align_grid_full(bpm, offset_seconds, duration_seconds, 0);
        beats
    }

    /// Computes beat positions, downbeat positions, and bar boundaries for deterministic musical phase alignment.
    pub fn align_grid_full(
        &self,
        bpm: f64,
        offset_seconds: f64,
        duration_seconds: f64,
        downbeat_offset_beats: usize,
    ) -> (Vec<f64>, Vec<f64>, Vec<f64>) {
        if bpm <= 0.0 || duration_seconds <= 0.0 {
            return (Vec::new(), Vec::new(), Vec::new());
        }

        let beat_interval = 60.0 / bpm;
        let mut beats = Vec::new();
        let mut downbeats = Vec::new();
        let mut bars = Vec::new();

        let mut pos = offset_seconds.max(0.0);
        let mut beat_idx = 0usize;

        while pos < duration_seconds {
            let rounded_pos = (pos * 1000.0).round() / 1000.0;
            beats.push(rounded_pos);
            if (beat_idx % 4) == (downbeat_offset_beats % 4) {
                downbeats.push(rounded_pos);
                bars.push(rounded_pos);
            }
            pos += beat_interval;
            beat_idx += 1;
        }

        (beats, downbeats, bars)
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
    fn test_beatgrid_full_alignment() {
        let detector = BeatgridDetector::new();
        let (beats, downbeats, bars) = detector.align_grid_full(120.0, 0.25, 3.0, 0);
        assert_eq!(beats.len(), 6);
        assert_eq!(beats[0], 0.25);
        assert_eq!(beats[1], 0.75);
        assert_eq!(downbeats.len(), 2);
        assert_eq!(downbeats[0], 0.25);
        assert_eq!(downbeats[1], 2.25);
        assert_eq!(bars, downbeats);
    }

    #[test]
    fn test_beatgrid_invalid_inputs() {
        let detector = BeatgridDetector::new();
        assert!(detector.align_grid(0.0, 0.0, 2.0).is_empty());
        assert!(detector.align_grid(120.0, 0.0, 0.0).is_empty());
        let (beats, downbeats, bars) = detector.align_grid_full(-10.0, 0.0, 2.0, 0);
        assert!(beats.is_empty() && downbeats.is_empty() && bars.is_empty());
    }
}
