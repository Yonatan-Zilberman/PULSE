use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct TrackMetadata {
    pub title: String,
    pub artist: String,
    pub album: Option<String>,
    pub genre: Option<String>,
    pub duration_seconds: f64,
    pub artwork_uri: Option<String>,
    pub file_path: String,
    pub sample_rate: u32,
    pub channels: u16,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct TempoProfile {
    pub bpm: f64,
    pub bpm_confidence: f32,
    pub alternative_bpm_hypotheses: Vec<f64>,
    pub beat_positions: Vec<f64>,
    pub downbeat_positions: Vec<f64>,
    pub bar_positions: Vec<f64>,
    pub grid_offset_seconds: f64,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct KeyProfile {
    pub key: String,
    pub camelot: String,
    pub key_confidence: f32,
    pub chroma_profile: Vec<f32>,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum SegmentType {
    Intro,
    Verse,
    PreChorus,
    Chorus,
    Breakdown,
    Build,
    Drop,
    Instrumental,
    Bridge,
    Outro,
    Silence,
    HardEnding,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct StructureSegment {
    pub segment_type: SegmentType,
    pub start_seconds: f64,
    pub end_seconds: f64,
    pub confidence: f32,
    pub energy: f32,
    pub vocal_density: f32,
    pub instrumental_density: f32,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct PhraseBoundaries {
    pub boundaries_4bar: Vec<f64>,
    pub boundaries_8bar: Vec<f64>,
    pub boundaries_16bar: Vec<f64>,
    pub boundaries_32bar: Vec<f64>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct LoudnessProfile {
    pub integrated_lufs: f32,
    pub short_term_lufs_max: f32,
    pub true_peak_db: f32,
    pub dynamic_range_lu: f32,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct MixabilityProfile {
    pub intro_quality: f32,
    pub outro_quality: f32,
    pub phrase_stability: f32,
    pub vocal_isolation_feasibility: f32,
    pub beat_stability: f32,
    pub tempo_stability: f32,
    pub transition_option_count: u32,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum StemCacheStatus {
    NotCached,
    Queued,
    Processing,
    Cached,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct TrackProfile {
    pub id: String,
    pub metadata: TrackMetadata,
    pub tempo: TempoProfile,
    pub key: KeyProfile,
    pub structure: Vec<StructureSegment>,
    pub phrases: PhraseBoundaries,
    pub energy_curve: Vec<f32>,
    pub loudness: LoudnessProfile,
    pub mixability: MixabilityProfile,
    pub stem_cache_status: StemCacheStatus,
    pub overall_confidence: f32,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_track_profile_serialization_roundtrip() {
        let profile = TrackProfile {
            id: "trk-001".to_string(),
            metadata: TrackMetadata {
                title: "Resonance".to_string(),
                artist: "Pulse Lab".to_string(),
                album: Some("Zero Cloud".to_string()),
                genre: Some("Melodic Techno".to_string()),
                duration_seconds: 360.0,
                artwork_uri: None,
                file_path: "/music/resonance.flac".to_string(),
                sample_rate: 48000,
                channels: 2,
            },
            tempo: TempoProfile {
                bpm: 126.0,
                bpm_confidence: 0.98,
                alternative_bpm_hypotheses: vec![63.0, 252.0],
                beat_positions: vec![0.0, 0.476, 0.952],
                downbeat_positions: vec![0.0, 1.904],
                bar_positions: vec![0.0, 1.904],
                grid_offset_seconds: 0.02,
            },
            key: KeyProfile {
                key: "A Minor".to_string(),
                camelot: "8A".to_string(),
                key_confidence: 0.95,
                chroma_profile: vec![0.1; 12],
            },
            structure: vec![StructureSegment {
                segment_type: SegmentType::Intro,
                start_seconds: 0.0,
                end_seconds: 30.0,
                confidence: 0.92,
                energy: 0.4,
                vocal_density: 0.0,
                instrumental_density: 0.8,
            }],
            phrases: PhraseBoundaries {
                boundaries_4bar: vec![0.0, 7.619],
                boundaries_8bar: vec![0.0, 15.238],
                boundaries_16bar: vec![0.0, 30.476],
                boundaries_32bar: vec![0.0, 60.952],
            },
            energy_curve: vec![0.4, 0.5, 0.6, 0.8, 0.9, 0.7],
            loudness: LoudnessProfile {
                integrated_lufs: -14.2,
                short_term_lufs_max: -10.5,
                true_peak_db: -0.8,
                dynamic_range_lu: 6.2,
            },
            mixability: MixabilityProfile {
                intro_quality: 0.9,
                outro_quality: 0.85,
                phrase_stability: 0.95,
                vocal_isolation_feasibility: 0.9,
                beat_stability: 0.99,
                tempo_stability: 0.99,
                transition_option_count: 12,
            },
            stem_cache_status: StemCacheStatus::Cached,
            overall_confidence: 0.96,
        };

        let json = serde_json::to_string(&profile).expect("Failed to serialize TrackProfile");
        let deserialized: TrackProfile =
            serde_json::from_str(&json).expect("Failed to deserialize TrackProfile");

        assert_eq!(profile, deserialized);
        assert_eq!(deserialized.metadata.title, "Resonance");
        assert_eq!(deserialized.tempo.bpm, 126.0);
        assert_eq!(deserialized.key.camelot, "8A");
    }
}
