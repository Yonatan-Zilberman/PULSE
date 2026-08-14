use crate::models::TempoProfile;

pub struct BpmDetector;

impl BpmDetector {
    pub fn new() -> Self {
        Self
    }

    /// Analyzes PCM audio buffer to estimate BPM and candidate tempo hypotheses.
    pub fn analyze(&self, samples: &[f32], sample_rate: u32) -> TempoProfile {
        if samples.is_empty() || sample_rate == 0 {
            return Self::default_profile();
        }

        let total_frames = samples.len();
        let hop_size = (sample_rate / 100).max(1) as usize; // ~10ms hop
        let num_hops = total_frames / hop_size;

        if num_hops < 50 {
            return Self::default_profile();
        }

        let envelope = Self::compute_rms_envelope(samples, hop_size, num_hops, total_frames);
        let onset = Self::compute_onset_curve(&envelope, num_hops);
        let (detected_bpm, confidence) =
            Self::autocorrelate_bpm(&onset, num_hops, sample_rate, hop_size);

        let (beat_positions, downbeat_positions, bar_positions) =
            Self::generate_grids(detected_bpm, total_frames, sample_rate);

        let alt_hypotheses = vec![
            ((detected_bpm / 2.0) * 10.0).round() / 10.0,
            ((detected_bpm * 2.0) * 10.0).round() / 10.0,
        ];

        TempoProfile {
            bpm: detected_bpm,
            bpm_confidence: confidence,
            alternative_bpm_hypotheses: alt_hypotheses,
            beat_positions,
            downbeat_positions,
            bar_positions,
            grid_offset_seconds: 0.0,
        }
    }

    fn default_profile() -> TempoProfile {
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

    fn compute_rms_envelope(
        samples: &[f32],
        hop_size: usize,
        num_hops: usize,
        total_frames: usize,
    ) -> Vec<f64> {
        let mut envelope = Vec::with_capacity(num_hops);
        for h in 0..num_hops {
            let start = h * hop_size;
            let end = (start + hop_size).min(total_frames);
            let mut sum_sq = 0.0f64;
            for &s in &samples[start..end] {
                sum_sq += f64::from(s) * f64::from(s);
            }
            let rms = (sum_sq / (end - start) as f64).sqrt();
            envelope.push(rms);
        }
        envelope
    }

    fn compute_onset_curve(envelope: &[f64], num_hops: usize) -> Vec<f64> {
        let mut onset = vec![0.0f64; num_hops];
        for h in 1..num_hops {
            let diff = envelope[h] - envelope[h - 1];
            if diff > 0.0 {
                onset[h] = diff;
            }
        }
        onset
    }

    fn autocorrelate_bpm(
        onset: &[f64],
        num_hops: usize,
        sample_rate: u32,
        hop_size: usize,
    ) -> (f64, f32) {
        let hops_per_sec = f64::from(sample_rate) / hop_size as f64;
        let min_lag = ((60.0 / 180.0) * hops_per_sec) as usize;
        let max_lag = hops_per_sec.min(num_hops as f64 - 1.0) as usize;

        let mut best_corr = 0.0f64;
        let mut best_lag = min_lag;
        let mut mean_corr = 0.0f64;
        let mut lag_count = 0usize;

        for lag in min_lag..=max_lag {
            let count = num_hops - lag;
            if count == 0 {
                continue;
            }
            let mut corr = 0.0f64;
            for i in 0..count {
                corr += onset[i] * onset[i + lag];
            }
            corr /= count as f64;
            mean_corr += corr;
            lag_count += 1;

            if corr > best_corr {
                best_corr = corr;
                best_lag = lag;
            }
        }

        if lag_count > 0 {
            mean_corr /= lag_count as f64;
        }

        let detected_bpm = if best_lag > 0 && best_corr > 1e-6 {
            let raw_bpm = (60.0 * hops_per_sec) / best_lag as f64;
            (raw_bpm * 10.0).round() / 10.0
        } else {
            120.0
        };

        let confidence = if mean_corr > 1e-6 && best_corr > mean_corr {
            ((best_corr - mean_corr) / best_corr).clamp(0.0, 1.0) as f32
        } else {
            0.5f32
        };

        (detected_bpm, confidence)
    }

    fn generate_grids(
        bpm: f64,
        total_frames: usize,
        sample_rate: u32,
    ) -> (Vec<f64>, Vec<f64>, Vec<f64>) {
        let beat_interval = 60.0 / bpm;
        let duration_seconds = total_frames as f64 / f64::from(sample_rate);
        let mut beats = Vec::new();
        let mut downbeats = Vec::new();
        let mut bars = Vec::new();

        let mut pos = 0.0;
        let mut beat_idx = 0usize;
        while pos < duration_seconds {
            let rounded_pos = (pos * 1000.0).round() / 1000.0;
            beats.push(rounded_pos);
            if beat_idx.is_multiple_of(4) {
                downbeats.push(rounded_pos);
                bars.push(rounded_pos);
            }
            pos += beat_interval;
            beat_idx += 1;
        }

        (beats, downbeats, bars)
    }
}

impl Default for BpmDetector {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_bpm_detector_empty_samples() {
        let detector = BpmDetector::new();
        let profile = detector.analyze(&[], 48000);
        assert_eq!(profile.bpm, 120.0);
        assert_eq!(profile.bpm_confidence, 0.0);
        assert!(profile.beat_positions.is_empty());
    }

    #[test]
    fn test_bpm_detector_synthetic_120bpm() {
        let detector = BpmDetector::new();
        let sample_rate = 48000u32;
        let duration_sec = 5.0;
        let total_samples = (duration_sec * f64::from(sample_rate)) as usize;
        let mut samples = vec![0.0f32; total_samples];

        // Inject 120 BPM clicks (every 0.5 sec)
        let beat_interval_samples = (sample_rate as f64 * 0.5) as usize;
        for i in (0..total_samples).step_by(beat_interval_samples) {
            for j in 0..200.min(total_samples - i) {
                samples[i + j] = 0.8;
            }
        }

        let profile = detector.analyze(&samples, sample_rate);
        assert!((profile.bpm - 120.0).abs() < 2.0);
        assert!(!profile.beat_positions.is_empty());
        assert!(!profile.downbeat_positions.is_empty());
    }
}
