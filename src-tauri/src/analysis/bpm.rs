use crate::models::TempoProfile;

pub struct BpmDetector;

impl BpmDetector {
    pub fn new() -> Self {
        Self
    }

    /// Analyzes PCM audio buffer to estimate BPM, confidence, candidate hypotheses, phase offset, and beat positions.
    pub fn analyze(&self, samples: &[f32], sample_rate: u32) -> TempoProfile {
        if samples.is_empty() || sample_rate == 0 {
            return Self::default_profile();
        }

        let total_frames = samples.len();
        let total_duration_sec = total_frames as f64 / f64::from(sample_rate);
        let hop_size = (sample_rate / 100).max(1) as usize; // ~10ms hop
        let num_hops = total_frames / hop_size;

        if num_hops < 50 {
            return Self::default_profile();
        }

        let hops_per_sec = f64::from(sample_rate) / hop_size as f64;
        let envelope = Self::compute_rms_envelope(samples, hop_size, num_hops, total_frames);
        let onset_norm = Self::compute_onset_curve(&envelope, num_hops, hops_per_sec);

        let max_novelty = onset_norm.iter().copied().fold(0.0f64, f64::max);
        if max_novelty <= 1e-6 {
            return Self::default_profile();
        }

        let min_lag = ((60.0 / 220.0) * hops_per_sec).max(2.0) as usize;
        let max_lag = ((60.0 / 50.0) * hops_per_sec).min(num_hops as f64 - 1.0) as usize;

        let (best_lag, best_corr, mean_corr, peaks) =
            Self::autocorrelate_slice(&onset_norm, min_lag, max_lag);

        if best_corr <= 1e-6 || best_lag == 0 {
            return Self::default_profile();
        }

        let primary_bpm = ((60.0 * hops_per_sec) / best_lag as f64 * 10.0).round() / 10.0;
        let confidence = if mean_corr > 1e-6 && best_corr > mean_corr {
            (((best_corr - mean_corr) / (best_corr + 1e-6)).clamp(0.0, 1.0)) as f32
        } else {
            0.5f32
        };

        let alt_hypotheses = Self::resolve_hypotheses(primary_bpm, &peaks, hops_per_sec, best_corr);

        let (is_variable_tempo, drift_min_bpm, drift_max_bpm, local_bpms) = Self::detect_drift(
            &onset_norm,
            hops_per_sec,
            num_hops,
            min_lag,
            max_lag,
            primary_bpm,
            total_duration_sec,
        );

        let beat_interval_sec = 60.0 / primary_bpm;
        let beat_interval_hops = beat_interval_sec * hops_per_sec;

        let (best_phase_hop, first_onset_sec) = Self::find_phase_and_onset(
            &onset_norm,
            num_hops,
            hops_per_sec,
            beat_interval_hops,
            max_novelty,
        );

        let best_downbeat_offset =
            Self::find_downbeat_offset(&onset_norm, best_phase_hop, num_hops, beat_interval_hops);

        let (beats, downbeats, bars) = Self::generate_beats(
            first_onset_sec,
            total_duration_sec,
            beat_interval_sec,
            best_downbeat_offset,
            is_variable_tempo,
            &local_bpms,
        );

        TempoProfile {
            bpm: primary_bpm,
            bpm_confidence: confidence,
            alternative_bpm_hypotheses: alt_hypotheses,
            beat_positions: beats,
            downbeat_positions: downbeats,
            bar_positions: bars,
            grid_offset_seconds: (first_onset_sec * 1000.0).round() / 1000.0,
            is_variable_tempo,
            tempo_drift_min_bpm: drift_min_bpm,
            tempo_drift_max_bpm: drift_max_bpm,
        }
    }

    fn resolve_hypotheses(
        primary_bpm: f64,
        peaks: &[(usize, f64)],
        hops_per_sec: f64,
        best_corr: f64,
    ) -> Vec<f64> {
        let mut alt_hypotheses = vec![
            ((primary_bpm / 2.0) * 10.0).round() / 10.0,
            ((primary_bpm * 2.0) * 10.0).round() / 10.0,
        ];
        for (lag, corr) in peaks {
            let pk_bpm = ((60.0 * hops_per_sec) / *lag as f64 * 10.0).round() / 10.0;
            if (pk_bpm - primary_bpm).abs() > 2.0 && *corr > 0.5 * best_corr {
                alt_hypotheses.push(pk_bpm);
            }
        }
        alt_hypotheses.sort_by(|a, b| a.partial_cmp(b).unwrap_or(std::cmp::Ordering::Equal));
        alt_hypotheses.dedup();
        alt_hypotheses
    }

    fn detect_drift(
        onset_norm: &[f64],
        hops_per_sec: f64,
        num_hops: usize,
        min_lag: usize,
        max_lag: usize,
        primary_bpm: f64,
        total_duration_sec: f64,
    ) -> (bool, f64, f64, Vec<f64>) {
        let mut is_variable_tempo = false;
        let mut drift_min_bpm = primary_bpm;
        let mut drift_max_bpm = primary_bpm;
        let mut local_bpms = Vec::new();

        if total_duration_sec >= 6.0 {
            let window_hops = (4.0 * hops_per_sec) as usize;
            let step_hops = (1.0 * hops_per_sec) as usize;

            let mut start_h = 0;
            while start_h + window_hops <= num_hops {
                let slice = &onset_norm[start_h..start_h + window_hops];
                let (loc_lag, loc_corr, _, _) = Self::autocorrelate_slice(slice, min_lag, max_lag);
                if loc_corr > 1e-6 && loc_lag > 0 {
                    let mut loc_bpm = (60.0 * hops_per_sec) / loc_lag as f64;
                    if (loc_bpm * 2.0 - primary_bpm).abs() < (loc_bpm - primary_bpm).abs() {
                        loc_bpm *= 2.0;
                    } else if (loc_bpm * 0.5 - primary_bpm).abs() < (loc_bpm - primary_bpm).abs() {
                        loc_bpm *= 0.5;
                    }
                    local_bpms.push(loc_bpm);
                }
                start_h += step_hops;
            }

            if local_bpms.len() >= 3 {
                let min_loc = local_bpms.iter().copied().fold(f64::INFINITY, f64::min);
                let max_loc = local_bpms.iter().copied().fold(f64::NEG_INFINITY, f64::max);
                if (max_loc - min_loc) > 2.0 {
                    is_variable_tempo = true;
                    drift_min_bpm = (min_loc * 10.0).round() / 10.0;
                    drift_max_bpm = (max_loc * 10.0).round() / 10.0;
                }
            }
        }

        (is_variable_tempo, drift_min_bpm, drift_max_bpm, local_bpms)
    }

    fn find_phase_and_onset(
        onset_norm: &[f64],
        num_hops: usize,
        hops_per_sec: f64,
        beat_interval_hops: f64,
        max_novelty: f64,
    ) -> (usize, f64) {
        let search_phase_max = (beat_interval_hops.ceil() as usize).min(num_hops - 1);
        let mut best_phase_hop = 0usize;
        let mut best_phase_score = -1.0f64;

        for phase in 0..search_phase_max {
            let mut score = 0.0f64;
            let mut count = 0usize;
            let mut h = phase as f64;
            while h < num_hops as f64 {
                let hop_idx = h.round() as usize;
                if hop_idx < num_hops {
                    score += onset_norm[hop_idx];
                    count += 1;
                }
                h += beat_interval_hops;
            }
            if count > 0 {
                score /= count as f64;
                if score > best_phase_score {
                    best_phase_score = score;
                    best_phase_hop = phase;
                }
            }
        }

        let mut first_onset_sec = best_phase_hop as f64 / hops_per_sec;
        let threshold = max_novelty * 0.15;
        let mut h = best_phase_hop as f64;
        while h < num_hops as f64 {
            let hop_idx = h.round() as usize;
            if hop_idx < num_hops && onset_norm[hop_idx] >= threshold {
                first_onset_sec = hop_idx as f64 / hops_per_sec;
                break;
            }
            h += beat_interval_hops;
        }

        (best_phase_hop, first_onset_sec)
    }

    fn find_downbeat_offset(
        onset_norm: &[f64],
        best_phase_hop: usize,
        num_hops: usize,
        beat_interval_hops: f64,
    ) -> usize {
        let mut best_downbeat_offset = 0usize;
        let mut best_downbeat_score = -1.0f64;

        for bar_beat in 0..4 {
            let mut score = 0.0f64;
            let mut count = 0usize;
            let mut cur_h = best_phase_hop as f64 + bar_beat as f64 * beat_interval_hops;
            while cur_h < num_hops as f64 {
                let hop_idx = cur_h.round() as usize;
                if hop_idx < num_hops {
                    score += onset_norm[hop_idx];
                    count += 1;
                }
                cur_h += 4.0 * beat_interval_hops;
            }
            if count > 0 {
                score /= count as f64;
                if score > best_downbeat_score {
                    best_downbeat_score = score;
                    best_downbeat_offset = bar_beat;
                }
            }
        }

        best_downbeat_offset
    }

    fn generate_beats(
        first_onset_sec: f64,
        total_duration_sec: f64,
        beat_interval_sec: f64,
        best_downbeat_offset: usize,
        is_variable_tempo: bool,
        local_bpms: &[f64],
    ) -> (Vec<f64>, Vec<f64>, Vec<f64>) {
        let mut beats = Vec::new();
        let mut downbeats = Vec::new();
        let mut bars = Vec::new();

        let mut pos = first_onset_sec;
        let mut beat_count = 0usize;

        while pos < total_duration_sec {
            let rounded_pos = (pos * 1000.0).round() / 1000.0;
            beats.push(rounded_pos);

            if (beat_count % 4) == best_downbeat_offset {
                downbeats.push(rounded_pos);
                bars.push(rounded_pos);
            }

            let step = if is_variable_tempo && !local_bpms.is_empty() {
                let progress = pos / total_duration_sec;
                let idx = ((progress * (local_bpms.len() - 1) as f64).round() as usize)
                    .min(local_bpms.len() - 1);
                60.0 / local_bpms[idx]
            } else {
                beat_interval_sec
            };

            pos += step;
            beat_count += 1;
        }

        (beats, downbeats, bars)
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
            is_variable_tempo: false,
            tempo_drift_min_bpm: 120.0,
            tempo_drift_max_bpm: 120.0,
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

    fn compute_onset_curve(envelope: &[f64], num_hops: usize, hops_per_sec: f64) -> Vec<f64> {
        let mut raw_onset = vec![0.0f64; num_hops];
        for h in 1..num_hops {
            let diff = envelope[h] - envelope[h - 1];
            if diff > 0.0 {
                raw_onset[h] = diff;
            }
        }

        let local_window = ((hops_per_sec * 0.2) as usize).max(3);
        let mut onset_norm = vec![0.0f64; num_hops];

        for h in 0..num_hops {
            let w_start = h.saturating_sub(local_window);
            let w_end = (h + local_window).min(num_hops);
            let local_sum: f64 = raw_onset[w_start..w_end].iter().sum();
            let local_mean = local_sum / (w_end - w_start) as f64;
            let val = raw_onset[h] - local_mean * 0.5;
            onset_norm[h] = val.max(0.0);
        }

        onset_norm
    }

    fn autocorrelate_slice(
        data: &[f64],
        min_lag: usize,
        max_lag: usize,
    ) -> (usize, f64, f64, Vec<(usize, f64)>) {
        let len = data.len();
        let max_l = max_lag.min(len.saturating_sub(1));
        if min_lag >= max_l {
            return (min_lag, 0.0, 0.0, Vec::new());
        }

        let mut best_corr = -1.0f64;
        let mut best_lag = min_lag;
        let mut mean_corr = 0.0f64;
        let mut lag_count = 0usize;
        let mut corr_array = vec![0.0f64; max_l + 2];

        for lag in min_lag..=max_l {
            let count = len - lag;
            let mut corr = 0.0f64;
            for i in 0..count {
                corr += data[i] * data[i + lag];
            }
            corr /= count as f64;
            corr_array[lag] = corr;
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

        let mut peaks = Vec::new();
        for lag in (min_lag + 1)..max_l {
            if corr_array[lag] > corr_array[lag - 1]
                && corr_array[lag] > corr_array[lag + 1]
                && corr_array[lag] > mean_corr
            {
                peaks.push((lag, corr_array[lag]));
            }
        }
        peaks.sort_by(|a, b| b.1.partial_cmp(&a.1).unwrap_or(std::cmp::Ordering::Equal));

        (best_lag, best_corr, mean_corr, peaks)
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
        let duration_sec = 6.0;
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
        assert!(profile.bpm_confidence > 0.6);
        assert!(!profile.beat_positions.is_empty());
        assert!(!profile.downbeat_positions.is_empty());
        assert!(!profile.is_variable_tempo);
    }

    #[test]
    fn test_bpm_detector_ambiguous_70_140bpm() {
        let detector = BpmDetector::new();
        let sample_rate = 48000u32;
        let duration_sec = 6.0;
        let total_samples = (duration_sec * f64::from(sample_rate)) as usize;
        let mut samples = vec![0.0f32; total_samples];

        // Strong click every ~0.857s (70 BPM) and light click every ~0.428s (140 BPM)
        let interval_70 = (sample_rate as f64 * (60.0 / 70.0)) as usize;
        let interval_140 = (sample_rate as f64 * (60.0 / 140.0)) as usize;

        for i in (0..total_samples).step_by(interval_140) {
            let amp = if i % interval_70 == 0 { 0.8 } else { 0.4 };
            for j in 0..200.min(total_samples - i) {
                samples[i + j] = amp;
            }
        }

        let profile = detector.analyze(&samples, sample_rate);
        let is_70 = (profile.bpm - 70.0).abs() < 3.0;
        let is_140 = (profile.bpm - 140.0).abs() < 4.0;
        assert!(is_70 || is_140);
        assert!(!profile.alternative_bpm_hypotheses.is_empty());
    }

    #[test]
    fn test_bpm_detector_silence_and_dc() {
        let detector = BpmDetector::new();
        let silent = vec![0.0f32; 48000 * 3];
        let dc = vec![0.5f32; 48000 * 3];

        let prof_silent = detector.analyze(&silent, 48000);
        assert_eq!(prof_silent.bpm, 120.0);
        assert_eq!(prof_silent.bpm_confidence, 0.0);

        let prof_dc = detector.analyze(&dc, 48000);
        assert_eq!(prof_dc.bpm, 120.0);
    }
}
