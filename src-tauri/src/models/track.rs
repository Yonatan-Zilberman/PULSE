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
    #[serde(default)]
    pub is_variable_tempo: bool,
    #[serde(default)]
    pub tempo_drift_min_bpm: f64,
    #[serde(default)]
    pub tempo_drift_max_bpm: f64,
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

/// The five fixed analysis stages, in declaration order (the order the
/// cache read path returns stage entries in).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum AnalysisStage {
    Tempo,
    Key,
    Structure,
    Loudness,
    Mixability,
}

impl AnalysisStage {
    /// Wire/DB string form (doubles as the `{stage}_…` column-name prefix).
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Tempo => "tempo",
            Self::Key => "key",
            Self::Structure => "structure",
            Self::Loudness => "loudness",
            Self::Mixability => "mixability",
        }
    }

    /// Parse a wire string; `None` for unknown/garbage input (case-sensitive,
    /// no trimming — the DB CHECK constraint is the backstop).
    pub fn parse(s: &str) -> Option<Self> {
        match s {
            "tempo" => Some(Self::Tempo),
            "key" => Some(Self::Key),
            "structure" => Some(Self::Structure),
            "loudness" => Some(Self::Loudness),
            "mixability" => Some(Self::Mixability),
            _ => None,
        }
    }

    /// All stages in declaration order (5 entries).
    pub const fn all() -> [AnalysisStage; 5] {
        [
            Self::Tempo,
            Self::Key,
            Self::Structure,
            Self::Loudness,
            Self::Mixability,
        ]
    }

    /// The model version whose result this stage's cache entry was produced
    /// under. All stages launch at `"1.0.0"`; the first real ML-model rollout
    /// bumps exactly one of these constants, and only that stage's cache
    /// entries flip to `stale`.
    pub const fn model_version(self) -> &'static str {
        // All stages launch at the same version; when a real model rollout
        // bumps one stage, its arm diverges and this match becomes per-stage.
        match self {
            Self::Tempo | Self::Key | Self::Structure | Self::Loudness | Self::Mixability => {
                "1.0.0"
            }
        }
    }
}

/// The `structure` stage payload: `TrackProfile`'s three structural fields
/// (phrases and the energy curve derive from segmentation, so they share one
/// version). `TrackProfile` itself, `overall_confidence`, and
/// `stem_cache_status` are deliberately not persisted (see the stems
/// milestone and the future orchestrator).
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct StructureStageResult {
    pub segments: Vec<StructureSegment>,
    pub phrases: PhraseBoundaries,
    pub energy_curve: Vec<f32>,
}

/// A per-stage analysis result payload, internally tagged with `"stage"` so
/// the cache read path can detect a payload/column mismatch (a tag that does
/// not match the stage the column belongs to marks that stage `invalid`).
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(tag = "stage", rename_all = "snake_case")]
pub enum AnalysisStagePayload {
    Tempo(TempoProfile),
    Key(KeyProfile),
    Structure(StructureStageResult),
    Loudness(LoudnessProfile),
    Mixability(MixabilityProfile),
}

impl AnalysisStagePayload {
    /// The stage this payload was produced for (from its `"stage"` tag).
    pub fn stage(&self) -> AnalysisStage {
        match self {
            Self::Tempo(_) => AnalysisStage::Tempo,
            Self::Key(_) => AnalysisStage::Key,
            Self::Structure(_) => AnalysisStage::Structure,
            Self::Loudness(_) => AnalysisStage::Loudness,
            Self::Mixability(_) => AnalysisStage::Mixability,
        }
    }
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
                is_variable_tempo: false,
                tempo_drift_min_bpm: 126.0,
                tempo_drift_max_bpm: 126.0,
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

    /// Each stage variant round-trips through its `"stage"` JSON tag, the
    /// wire string round-trips through `as_str`/`parse`, and the model
    /// version constant is stable.
    #[test]
    fn test_analysis_stage_serialization_roundtrip() {
        let tempo = AnalysisStagePayload::Tempo(TempoProfile {
            bpm: 124.0,
            bpm_confidence: 0.9,
            alternative_bpm_hypotheses: vec![],
            beat_positions: vec![0.0],
            downbeat_positions: vec![0.0],
            bar_positions: vec![0.0],
            grid_offset_seconds: 0.0,
            is_variable_tempo: false,
            tempo_drift_min_bpm: 124.0,
            tempo_drift_max_bpm: 124.0,
        });
        let key = AnalysisStagePayload::Key(KeyProfile {
            key: "F# Minor".to_string(),
            camelot: "11A".to_string(),
            key_confidence: 0.88,
            chroma_profile: vec![0.2; 12],
        });
        let structure = AnalysisStagePayload::Structure(StructureStageResult {
            segments: vec![StructureSegment {
                segment_type: SegmentType::Drop,
                start_seconds: 30.0,
                end_seconds: 60.0,
                confidence: 0.91,
                energy: 0.9,
                vocal_density: 0.5,
                instrumental_density: 0.7,
            }],
            phrases: PhraseBoundaries {
                boundaries_4bar: vec![0.0, 8.0],
                boundaries_8bar: vec![0.0, 16.0],
                boundaries_16bar: vec![0.0, 32.0],
                boundaries_32bar: vec![0.0, 64.0],
            },
            energy_curve: vec![0.3, 0.95, 0.6],
        });
        let loudness = AnalysisStagePayload::Loudness(LoudnessProfile {
            integrated_lufs: -10.0,
            short_term_lufs_max: -7.5,
            true_peak_db: -1.2,
            dynamic_range_lu: 4.8,
        });
        let mixability = AnalysisStagePayload::Mixability(MixabilityProfile {
            intro_quality: 0.8,
            outro_quality: 0.82,
            phrase_stability: 0.9,
            vocal_isolation_feasibility: 0.88,
            beat_stability: 0.97,
            tempo_stability: 0.98,
            transition_option_count: 9,
        });

        for payload in [&tempo, &key, &structure, &loudness, &mixability] {
            let json = serde_json::to_string(payload).expect("serialize");
            let parsed: AnalysisStagePayload = serde_json::from_str(&json).expect("deserialize");
            assert_eq!(*payload, parsed, "payload must round-trip: {json}");
            assert_eq!(
                parsed.stage().as_str(),
                payload.stage().as_str(),
                "the stage tag must survive the round-trip: {json}"
            );
            // The tag is embedded as a plain JSON string field.
            assert!(
                json.contains(&format!(r#""stage":"{}""#, payload.stage().as_str())),
                "tag missing from: {json}"
            );
        }

        for stage in AnalysisStage::all() {
            assert_eq!(
                AnalysisStage::parse(stage.as_str()),
                Some(stage),
                "{} must parse back",
                stage.as_str()
            );
            assert_eq!(stage.model_version(), "1.0.0");
        }
        assert_eq!(AnalysisStage::all().len(), 5);
        assert_eq!(AnalysisStage::parse("Tempo"), None, "case-sensitive");
        assert_eq!(AnalysisStage::parse("tempo "), None, "no trimming");
        assert_eq!(AnalysisStage::parse("bpm"), None);
    }

    /// `parse` degrades to `None` on empty, NUL-laden, and overlong input
    /// without panicking, and `all()` is exactly the five distinct stages
    /// (no duplicates, no omissions).
    #[test]
    fn test_analysis_stage_parse_rejects_empty_null_and_all_unique() {
        let huge = "x".repeat(10_000);
        for garbage in ["", "\u{0}", "  ", "tempo\u{0}", "t\nempo", huge.as_str()] {
            assert_eq!(AnalysisStage::parse(garbage), None, "garbage {garbage:?}");
        }
        let all = AnalysisStage::all();
        let distinct = all
            .iter()
            .map(|s| s.as_str())
            .collect::<std::collections::HashSet<_>>();
        assert_eq!(distinct.len(), 5, "the five stages must be distinct");
    }
}
