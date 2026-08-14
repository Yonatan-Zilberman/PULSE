use crate::models::{TrackProfile, TransitionCandidate};

pub struct CandidateGenerator;

impl CandidateGenerator {
    pub fn new() -> Self {
        Self
    }

    /// Evaluates transition compatibility between incoming and outgoing tracks.
    pub fn generate_candidates(
        &self,
        _outgoing: &TrackProfile,
        _incoming: &TrackProfile,
    ) -> Vec<TransitionCandidate> {
        // Concrete candidate generation will be implemented in DJ Brain phase
        Vec::new()
    }
}

impl Default for CandidateGenerator {
    fn default() -> Self {
        Self::new()
    }
}
