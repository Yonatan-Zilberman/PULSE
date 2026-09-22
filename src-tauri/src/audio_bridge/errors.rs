//! Error type for the audio bridge boundary.
//!
//! Serializes to the JS side as a string (Tauri IPC) via `serde::Serialize` +
//! `thiserror`. See `Docs/Audio-Bridge-Contract.md` for the error-code table.

use serde::Serialize;
use thiserror::Error;

/// Errors surfaced by [`crate::audio_bridge::bridge::AudioBridge`] commands.
#[derive(Debug, Error, Serialize)]
pub enum AudioBridgeError {
    /// A lifecycle command ran before `audio_init`.
    #[error("audio engine is not initialized (call audio_init first)")]
    NotInitialized,

    /// A command requires the engine to be started (`audio_start`).
    #[error("audio engine is not running (call audio_start first)")]
    NotRunning,

    /// Deck ids must be 0 or 1.
    #[error("invalid deck id {0} (must be 0 or 1)")]
    InvalidDeckId(u8),

    /// A numeric or string parameter failed validation at the Rust boundary
    /// (non-finite value, negative position, NUL byte in a path, …).
    #[error("invalid input: {0}")]
    InvalidInput(String),

    /// The C++ engine reported a failure (-1) for a control-plane operation.
    #[error("audio engine failed: {0}")]
    EngineFailed(String),

    /// This build has no C++ audio engine (`CMake` unavailable at build time).
    #[error("C++ audio engine is unavailable in this build")]
    EngineNotAvailable,
}
