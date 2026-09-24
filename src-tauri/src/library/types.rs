//! Data types for the library subsystem: DB row shapes, IPC projections,
//! state enums, and scan reports.
//!
//! Enums serialize as lowercase `snake_case` strings over Tauri IPC.

use serde::{Deserialize, Serialize};

/// Filesystem presence of a track's path.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Availability {
    /// The file exists at `file_path`.
    Available,
    /// The file vanished from `file_path`.
    Missing,
}

impl Availability {
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Available => "available",
            Self::Missing => "missing",
        }
    }

    pub fn parse(s: &str) -> Option<Self> {
        match s {
            "available" => Some(Self::Available),
            "missing" => Some(Self::Missing),
            _ => None,
        }
    }
}

/// Outcome of reading a file's embedded tags during ingest.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum IngestStatus {
    /// Tags (or at least a fallback title) were read successfully.
    Ok,
    /// The file could not be parsed; the row carries a fallback state.
    Error,
}

impl IngestStatus {
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Ok => "ok",
            Self::Error => "error",
        }
    }

    pub fn parse(s: &str) -> Option<Self> {
        match s {
            "ok" => Some(Self::Ok),
            "error" => Some(Self::Error),
            _ => None,
        }
    }
}

/// Analysis pipeline state. This task only adds the column (default
/// `not_started`) and never mutates it — execution is a later milestone.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum AnalysisState {
    NotStarted,
    InProgress,
    Complete,
    Failed,
}

impl AnalysisState {
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::NotStarted => "not_started",
            Self::InProgress => "in_progress",
            Self::Complete => "complete",
            Self::Failed => "failed",
        }
    }

    pub fn parse(s: &str) -> Option<Self> {
        match s {
            "not_started" => Some(Self::NotStarted),
            "in_progress" => Some(Self::InProgress),
            "complete" => Some(Self::Complete),
            "failed" => Some(Self::Failed),
            _ => None,
        }
    }
}

/// Machine-readable reasons a file could not be ingested.
///
/// These are the exact strings allowed by the `tracks.ingest_error` CHECK
/// constraint; `None` (NULL) means the row has no ingest error.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum IngestErrorReason {
    /// `errno EACCES/EPERM` while opening or reading the file.
    PermissionDenied,
    /// The path no longer exists (row-level; also drives `availability`).
    MissingFile,
    /// The file exists but its container/codec could not be parsed.
    CorruptContainer,
    /// The file is readable but not a decodable audio container.
    UnreadableFile,
}

impl IngestErrorReason {
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::PermissionDenied => "permission_denied",
            Self::MissingFile => "missing_file",
            Self::CorruptContainer => "corrupt_container",
            Self::UnreadableFile => "unreadable_file",
        }
    }

    pub fn parse(s: &str) -> Option<Self> {
        match s {
            "permission_denied" => Some(Self::PermissionDenied),
            "missing_file" => Some(Self::MissingFile),
            "corrupt_container" => Some(Self::CorruptContainer),
            "unreadable_file" => Some(Self::UnreadableFile),
            _ => None,
        }
    }
}

/// One row of the `tracks` table (full shape).
#[derive(Debug, Clone, PartialEq)]
pub struct TrackRow {
    pub id: String,
    /// Canonicalized file path — the row's identity key.
    pub file_path: String,
    /// Hex SHA-256 of the file content once hashed; `None` until the
    /// background hash worker has processed the file.
    pub file_hash: Option<String>,
    pub file_size_bytes: u64,
    /// Unix seconds.
    pub modified_timestamp: i64,
    pub title: String,
    pub artist: String,
    pub album: Option<String>,
    pub genre: Option<String>,
    pub duration_seconds: Option<f64>,
    pub bpm: Option<f64>,
    pub key: Option<String>,
    pub camelot: Option<String>,
    pub profile_json: Option<String>,
    pub ingest_status: IngestStatus,
    pub ingest_error: Option<IngestErrorReason>,
    pub availability: Availability,
    pub analysis_state: AnalysisState,
    pub sample_rate: Option<u32>,
    pub channels: Option<u16>,
    /// Id of the canonical (lowest `ingest_seq`) row with the same content
    /// hash; `None` for canonical rows.
    pub duplicate_of: Option<String>,
    /// Monotonic ingestion order; makes duplicate canonical selection
    /// deterministic.
    pub ingest_seq: i64,
    pub created_at: Option<String>,
    pub updated_at: Option<String>,
}

/// Fields written by ingest/upsert (keyed on `file_path` in the DB).
///
/// `file_hash` *is* included: ingest writes it as the known value (`None` =
/// unhashed, or invalidated after a content change; the background hash
/// worker later overwrites it via [`super::cache::LibraryCache::apply_hash`]).
/// `duplicate_of`, `analysis_state`, `ingest_seq`, and the timestamps are
/// deliberately absent: the duplicate link is applied by a separate write
/// path, and analysis state is never mutated by this task.
#[derive(Debug, Clone, PartialEq)]
pub struct TrackUpsert {
    pub file_path: String,
    pub file_hash: Option<String>,
    pub file_size_bytes: u64,
    pub modified_timestamp: i64,
    pub title: String,
    pub artist: String,
    pub album: Option<String>,
    pub genre: Option<String>,
    pub duration_seconds: Option<f64>,
    pub sample_rate: Option<u32>,
    pub channels: Option<u16>,
    pub ingest_status: IngestStatus,
    pub ingest_error: Option<IngestErrorReason>,
    pub availability: Availability,
}

/// Outcome of a cache upsert, derived by comparing against the pre-stored row.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UpsertOutcome {
    /// No row existed for the path; it was inserted.
    Inserted,
    /// A row existed and at least one field changed.
    Changed,
    /// A row existed and no field changed.
    Unchanged,
}

/// IPC projection of a track for the library UI commands.
#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
pub struct TrackSummary {
    pub id: String,
    pub file_path: String,
    pub title: String,
    pub artist: String,
    pub album: Option<String>,
    pub genre: Option<String>,
    pub duration_seconds: Option<f64>,
    pub sample_rate: Option<u32>,
    pub channels: Option<u16>,
    pub file_size_bytes: u64,
    pub modified_timestamp: i64,
    pub file_hash: Option<String>,
    pub availability: Availability,
    pub ingest_status: IngestStatus,
    pub ingest_error: Option<IngestErrorReason>,
    pub duplicate_of: Option<String>,
    pub analysis_state: AnalysisState,
}

impl From<&TrackRow> for TrackSummary {
    fn from(row: &TrackRow) -> Self {
        Self {
            id: row.id.clone(),
            file_path: row.file_path.clone(),
            title: row.title.clone(),
            artist: row.artist.clone(),
            album: row.album.clone(),
            genre: row.genre.clone(),
            duration_seconds: row.duration_seconds,
            sample_rate: row.sample_rate,
            channels: row.channels,
            file_size_bytes: row.file_size_bytes,
            modified_timestamp: row.modified_timestamp,
            file_hash: row.file_hash.clone(),
            availability: row.availability,
            ingest_status: row.ingest_status,
            ingest_error: row.ingest_error,
            duplicate_of: row.duplicate_of.clone(),
            analysis_state: row.analysis_state,
        }
    }
}

/// One named entry in a [`ScanReport`] (errored or duplicated rows).
#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
pub struct ReportEntry {
    pub id: String,
    pub file_path: String,
    pub reason: String,
}

/// Transition counts for a single `library_add_folder` / `library_refresh`
/// run. Every count is *transitions this run*: `added` = rows inserted,
/// `updated` = rows whose state changed, `missing` = rows newly marked
/// missing, `duplicates` = rows newly linked to a canonical row, `errored` =
/// rows that ended or stayed errored this run. "Zero changes" (idempotency)
/// is `added == 0 && updated == 0`.
#[derive(Debug, Clone, PartialEq, Default, Serialize)]
#[serde(rename_all = "snake_case")]
pub struct ScanReport {
    pub added: u32,
    pub updated: u32,
    pub unchanged: u32,
    pub missing: u32,
    pub duplicates: u32,
    pub errored: u32,
    pub errored_entries: Vec<ReportEntry>,
    pub duplicate_entries: Vec<ReportEntry>,
}

/// Summary of a duplicate-linking pass (`flush_hashes` / background worker).
/// `duplicates` = rows whose `duplicate_of` link was set or re-set this pass.
#[derive(Debug, Clone, PartialEq, Default, Serialize)]
#[serde(rename_all = "snake_case")]
pub struct DedupSummary {
    pub duplicates: u32,
    pub entries: Vec<ReportEntry>,
}
#[cfg(test)]
mod tests {
    use super::*;

    /// Every enum variant round-trips through its wire string, and the
    /// parsers reject garbage, wrong-case, and empty input (the DB CHECK
    /// constraint is the backstop for anything else).
    #[test]
    fn test_enum_wire_roundtrips_and_rejections() {
        assert_eq!(
            Availability::parse("available"),
            Some(Availability::Available)
        );
        assert_eq!(Availability::parse("missing"), Some(Availability::Missing));
        assert_eq!(Availability::parse("Available"), None, "case-sensitive");
        assert_eq!(Availability::parse(""), None);
        assert_eq!(Availability::parse("available "), None, "no trimming");
        assert_eq!(Availability::Available.as_str(), "available");
        assert_eq!(Availability::Missing.as_str(), "missing");

        assert_eq!(IngestStatus::parse("ok"), Some(IngestStatus::Ok));
        assert_eq!(IngestStatus::parse("error"), Some(IngestStatus::Error));
        assert_eq!(IngestStatus::parse("Ok"), None);
        assert_eq!(IngestStatus::parse("errored"), None);

        assert_eq!(
            AnalysisState::parse("not_started"),
            Some(AnalysisState::NotStarted)
        );
        assert_eq!(
            AnalysisState::parse("in_progress"),
            Some(AnalysisState::InProgress)
        );
        assert_eq!(
            AnalysisState::parse("complete"),
            Some(AnalysisState::Complete)
        );
        assert_eq!(AnalysisState::parse("failed"), Some(AnalysisState::Failed));
        assert_eq!(AnalysisState::parse("done"), None, "no synonym parsing");
        assert_eq!(AnalysisState::parse("COMPLETE"), None);

        assert_eq!(
            IngestErrorReason::parse("permission_denied"),
            Some(IngestErrorReason::PermissionDenied)
        );
        assert_eq!(
            IngestErrorReason::parse("missing_file"),
            Some(IngestErrorReason::MissingFile)
        );
        assert_eq!(
            IngestErrorReason::parse("corrupt_container"),
            Some(IngestErrorReason::CorruptContainer)
        );
        assert_eq!(
            IngestErrorReason::parse("unreadable_file"),
            Some(IngestErrorReason::UnreadableFile)
        );
        assert_eq!(IngestErrorReason::parse("corrupt"), None);
        assert_eq!(IngestErrorReason::parse(""), None);
    }

    /// The IPC projection serializes enums as lowercase `snake_case` strings
    /// (the frontend contract) and `None` fields as JSON nulls.
    #[test]
    fn test_track_summary_serializes_wire_contract() {
        let row = TrackRow {
            id: "abc".to_string(),
            file_path: "/music/x.mp3".to_string(),
            file_hash: Some("h".to_string()),
            file_size_bytes: 123,
            modified_timestamp: 1700,
            title: "T".to_string(),
            artist: "A".to_string(),
            album: Some("Al".to_string()),
            genre: None,
            duration_seconds: Some(200.5),
            bpm: None,
            key: None,
            camelot: None,
            profile_json: None,
            ingest_status: IngestStatus::Error,
            ingest_error: Some(IngestErrorReason::CorruptContainer),
            availability: Availability::Missing,
            analysis_state: AnalysisState::InProgress,
            sample_rate: Some(44100),
            channels: Some(2),
            duplicate_of: Some("canon".to_string()),
            ingest_seq: 7,
            created_at: None,
            updated_at: None,
        };
        let summary = TrackSummary::from(&row);
        let json = serde_json::to_string(&summary).unwrap();
        assert!(json.contains(r#""ingest_status":"error""#), "{json}");
        assert!(
            json.contains(r#""ingest_error":"corrupt_container""#),
            "{json}"
        );
        assert!(json.contains(r#""availability":"missing""#), "{json}");
        assert!(json.contains(r#""analysis_state":"in_progress""#), "{json}");
        assert!(json.contains(r#""genre":null"#), "{json}");
        // bpb/key/camelot/profile are never part of the projection.
        assert!(!json.contains("bpm"), "{json}");
        assert!(!json.contains("profile"), "{json}");
    }

    /// Report shapes: a default `ScanReport` is all zeros with empty entry
    /// lists (the idempotent "zero changes" shape).
    #[test]
    fn test_scan_report_default_is_zero_changes() {
        let report = ScanReport::default();
        assert_eq!(report.added, 0);
        assert_eq!(report.updated, 0);
        assert!(report.errored_entries.is_empty());
        assert!(report.duplicate_entries.is_empty());
        let dedup = DedupSummary::default();
        assert_eq!(dedup.duplicates, 0);
        assert!(dedup.entries.is_empty());
    }
}
