//! Tauri command surface.
//!
//! Every audio command goes through the [`AudioBridge`] boundary object
//! (managed in Tauri state); no command calls the C FFI directly. See
//! `Docs/Audio-Bridge-Contract.md`.

// Tauri commands always receive `State` and deserialized arguments by value
// (macro contract); the bridge borrows internally, so pass-by-value is
// intentional.
#![allow(clippy::needless_pass_by_value)]

use std::sync::Arc;

use tauri::State;

use crate::audio_bridge::{
    AudioBridge, AudioBridgeError, DeckState, EngineConfig, EngineStats, TransitionPlan,
};
use crate::library::{LibraryError, LibraryStore, ScanReport, TrackSummary};
use crate::models::TrackMetadata;

#[tauri::command]
pub fn get_app_version() -> String {
    env!("CARGO_PKG_VERSION").to_string()
}

#[tauri::command]
pub fn get_sample_track() -> TrackMetadata {
    TrackMetadata {
        title: "PULSE Baseline".to_string(),
        artist: "Core Engine".to_string(),
        album: Some("Zero Cloud Initializer".to_string()),
        genre: Some("Electronic".to_string()),
        duration_seconds: 300.0,
        artwork_uri: None,
        file_path: "/dummy/path.wav".to_string(),
        sample_rate: 48000,
        channels: 2,
    }
}

/// Read-only engine status probe. Always succeeds; `stats` is `None` when the
/// engine is uninitialized or this build has no C++ engine.
#[derive(Debug, serde::Serialize)]
pub struct AudioStatus {
    pub available: bool,
    pub initialized: bool,
    pub running: bool,
    pub stats: Option<EngineStats>,
}

#[tauri::command]
pub fn audio_status(bridge: State<'_, Arc<AudioBridge>>) -> AudioStatus {
    AudioStatus {
        available: bridge.available(),
        initialized: bridge.is_initialized(),
        running: bridge.is_running(),
        stats: bridge.get_stats().ok(),
    }
}

// ── Lifecycle ─────────────────────────────────────────────────────────────

#[tauri::command]
pub fn audio_init(
    bridge: State<'_, Arc<AudioBridge>>,
    sample_rate: u32,
    buffer_size: u32,
    channel_count: u32,
) -> Result<(), AudioBridgeError> {
    bridge.init(EngineConfig {
        sample_rate,
        buffer_size,
        channel_count,
    })
}

#[tauri::command]
pub fn audio_start(bridge: State<'_, Arc<AudioBridge>>) -> Result<(), AudioBridgeError> {
    bridge.start()
}

#[tauri::command]
pub fn audio_stop(bridge: State<'_, Arc<AudioBridge>>) -> Result<(), AudioBridgeError> {
    bridge.stop()
}

#[tauri::command]
pub fn audio_shutdown(bridge: State<'_, Arc<AudioBridge>>) -> Result<(), AudioBridgeError> {
    bridge.shutdown()
}

// ── Telemetry ─────────────────────────────────────────────────────────────

#[tauri::command]
pub fn audio_get_stats(
    bridge: State<'_, Arc<AudioBridge>>,
) -> Result<EngineStats, AudioBridgeError> {
    bridge.get_stats()
}

#[tauri::command]
pub fn audio_is_deck_playing(bridge: State<'_, Arc<AudioBridge>>, deck: u8) -> bool {
    bridge.is_deck_playing(deck)
}

#[tauri::command]
pub fn audio_play_pause(
    bridge: State<'_, Arc<AudioBridge>>,
    deck: u8,
    play: bool,
) -> Result<(), AudioBridgeError> {
    bridge.play_pause(deck, play)
}

#[tauri::command]
pub fn audio_get_deck_state(
    bridge: State<'_, Arc<AudioBridge>>,
    deck: u8,
) -> Result<DeckState, AudioBridgeError> {
    bridge.get_deck_state(deck)
}

#[tauri::command]
pub fn audio_get_deck_position(
    bridge: State<'_, Arc<AudioBridge>>,
    deck: u8,
) -> Result<f64, AudioBridgeError> {
    bridge.get_deck_position(deck)
}

#[tauri::command]
pub fn audio_get_deck_duration(
    bridge: State<'_, Arc<AudioBridge>>,
    deck: u8,
) -> Result<f64, AudioBridgeError> {
    bridge.get_deck_duration(deck)
}

// ── Loading / preparation ─────────────────────────────────────────────────

#[tauri::command]
pub fn audio_load_track(
    bridge: State<'_, Arc<AudioBridge>>,
    deck: u8,
    path: String,
) -> Result<(), AudioBridgeError> {
    bridge.load_track(deck, &path)
}

#[tauri::command]
pub fn audio_prepare_deck(
    bridge: State<'_, Arc<AudioBridge>>,
    deck: u8,
    path: String,
    cue_seconds: f64,
    tempo_ratio: f64,
    preserve_pitch: bool,
) -> Result<(), AudioBridgeError> {
    bridge.prepare_deck(deck, &path, cue_seconds, tempo_ratio, preserve_pitch)
}

// ── Playback control ──────────────────────────────────────────────────────

#[tauri::command]
pub fn audio_play(bridge: State<'_, Arc<AudioBridge>>, deck: u8) -> Result<(), AudioBridgeError> {
    bridge.play(deck)
}

#[tauri::command]
pub fn audio_pause(bridge: State<'_, Arc<AudioBridge>>, deck: u8) -> Result<(), AudioBridgeError> {
    bridge.pause(deck)
}

#[tauri::command]
pub fn audio_stop_deck(
    bridge: State<'_, Arc<AudioBridge>>,
    deck: u8,
) -> Result<(), AudioBridgeError> {
    bridge.stop_deck(deck)
}

#[tauri::command]
pub fn audio_seek(
    bridge: State<'_, Arc<AudioBridge>>,
    deck: u8,
    position_seconds: f64,
) -> Result<(), AudioBridgeError> {
    bridge.seek(deck, position_seconds)
}

// ── Deck parameters ───────────────────────────────────────────────────────

#[tauri::command]
pub fn audio_set_volume(
    bridge: State<'_, Arc<AudioBridge>>,
    deck: u8,
    volume: f64,
) -> Result<(), AudioBridgeError> {
    bridge.set_volume(deck, volume)
}

#[tauri::command]
pub fn audio_set_eq(
    bridge: State<'_, Arc<AudioBridge>>,
    deck: u8,
    low: f64,
    mid: f64,
    high: f64,
) -> Result<(), AudioBridgeError> {
    bridge.set_eq(deck, low, mid, high)
}

#[tauri::command]
pub fn audio_set_filter(
    bridge: State<'_, Arc<AudioBridge>>,
    deck: u8,
    filter: f64,
) -> Result<(), AudioBridgeError> {
    bridge.set_filter(deck, filter)
}

#[tauri::command]
pub fn audio_set_tempo_ratio(
    bridge: State<'_, Arc<AudioBridge>>,
    deck: u8,
    ratio: f64,
) -> Result<(), AudioBridgeError> {
    bridge.set_tempo_ratio(deck, ratio)
}

#[tauri::command]
pub fn audio_set_pitch_preservation(
    bridge: State<'_, Arc<AudioBridge>>,
    deck: u8,
    enabled: bool,
) -> Result<(), AudioBridgeError> {
    bridge.set_pitch_preservation(deck, enabled)
}

#[tauri::command]
pub fn audio_set_stem_levels(
    bridge: State<'_, Arc<AudioBridge>>,
    deck: u8,
    vocal: f64,
    drum: f64,
    bass: f64,
    other: f64,
) -> Result<(), AudioBridgeError> {
    bridge.set_stem_levels(deck, vocal, drum, bass, other)
}

// ── Transitions ───────────────────────────────────────────────────────────

/// Executes a fully precomputed transition plan on the C++ audio engine.
/// The plan is validated at the Rust boundary and sanitized onto the
/// versioned v2 `TransitionCommandC`; the engine re-sanitizes (defense in
/// depth) and executes it on the real-time thread.
#[tauri::command]
pub fn audio_execute_transition(
    bridge: State<'_, Arc<AudioBridge>>,
    plan: TransitionPlan,
) -> Result<(), AudioBridgeError> {
    bridge.execute_transition(&plan)
}

// ── Library ───────────────────────────────────────────────────────────────
//
// Library commands are synchronous: Tauri runs them on its blocking worker
// pool. All state transitions flow through the pure `library` state
// machine; content hashing happens in the background hash worker, never on
// the command critical path.

/// Register a folder as a library folder (idempotent) and ingest its
/// contents. Returns the transition counts for this run; duplicate links
/// for freshly hashed files land on the next dedup pass (background worker
/// or an explicit `library_refresh`).
#[tauri::command]
pub fn library_add_folder(
    store: State<'_, Arc<LibraryStore>>,
    path: String,
) -> Result<ScanReport, LibraryError> {
    store.add_folder(&path)
}

/// Re-validate every registered folder and every indexed row (errored rows
/// are retried for re-tagging; vanished files are marked missing).
#[tauri::command]
pub fn library_refresh(store: State<'_, Arc<LibraryStore>>) -> Result<ScanReport, LibraryError> {
    store.refresh()
}

/// All tracks (including duplicates, flagged via `duplicate_of`), sorted by
/// path. Rows whose file vanished are lazily marked `missing`.
#[tauri::command]
pub fn library_list(
    store: State<'_, Arc<LibraryStore>>,
) -> Result<Vec<TrackSummary>, LibraryError> {
    store.list()
}

/// One track by id; unknown ids fail with a message-carrying `Db` error.
#[tauri::command]
pub fn library_get(
    store: State<'_, Arc<LibraryStore>>,
    id: String,
) -> Result<TrackSummary, LibraryError> {
    store.get(&id)
}
