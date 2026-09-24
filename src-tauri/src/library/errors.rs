//! Error type for the library subsystem.
//!
//! Follows the `audio_bridge` pattern: a `#[derive(Debug, Error, Serialize)]`
//! enum with string-carrying variants so the error serializes cleanly over
//! Tauri IPC as a message.

use serde::Serialize;
use thiserror::Error;

/// Errors surfaced by the `library_*` Tauri commands.
///
/// Per-file failures (corrupt, unreadable, permission) do **not** fail a
/// command — they degrade to an `ingest_status = 'error'` row. Only `Db`-level
/// or folder-level failures produce a [`LibraryError`].
#[derive(Debug, Error, Serialize)]
pub enum LibraryError {
    /// A database operation failed.
    #[error("library database error: {0}")]
    Db(String),

    /// A file system operation failed.
    #[error("library i/o error: {0}")]
    Io(String),

    /// The given folder path does not exist.
    #[error("folder not found: {0}")]
    FolderNotFound(String),

    /// The given path exists but is not a directory.
    #[error("not a directory: {0}")]
    NotADirectory(String),
}

impl From<std::io::Error> for LibraryError {
    fn from(err: std::io::Error) -> Self {
        Self::Io(err.to_string())
    }
}

impl From<rusqlite::Error> for LibraryError {
    fn from(err: rusqlite::Error) -> Self {
        Self::Db(err.to_string())
    }
}
