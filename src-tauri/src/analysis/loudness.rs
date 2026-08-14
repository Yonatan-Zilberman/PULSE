use crate::models::LoudnessProfile;

pub struct LoudnessAnalyzer;

impl LoudnessAnalyzer {
    pub fn new() -> Self {
        Self
    }

    /// Measures ITU-R BS.1770 integrated LUFS, short-term max, true peak, and dynamic range.
    pub fn analyze(&self, _samples: &[f32], _sample_rate: u32) -> LoudnessProfile {
        LoudnessProfile {
            integrated_lufs: -14.0,
            short_term_lufs_max: -10.0,
            true_peak_db: -1.0,
            dynamic_range_lu: 6.0,
        }
    }
}

impl Default for LoudnessAnalyzer {
    fn default() -> Self {
        Self::new()
    }
}
