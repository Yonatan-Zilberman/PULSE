//! Library subsystem: folder discovery, embedded-tag ingestion, and the
//! persistent `tracks` index.
//!
//! Tag reading lives here (Rust) only — the C++ audio bridge is
//! playback-only (see `Docs/Audio-Bridge-Contract.md`).

pub mod cache;
pub mod errors;
pub mod scanner;
pub mod schema;
pub mod store;
pub mod tag_reader;
pub mod types;

pub use cache::LibraryCache;
pub use errors::LibraryError;
pub use scanner::{fs_state, scan_folder, validate_folder, DiscoveredFile, FsState};
pub use store::{
    compute_duplicate_links, derive_row_state, sha256_file, LibraryStore, RowChange, RowTransition,
    TagOutcome,
};
pub use tag_reader::{read as read_tags, AudioMetadata, TagReadError};
pub use types::{
    AnalysisState, Availability, DedupSummary, IngestErrorReason, IngestStatus, ReportEntry,
    ScanReport, TrackRow, TrackSummary, TrackUpsert, UpsertOutcome,
};
