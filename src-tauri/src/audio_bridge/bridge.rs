//! Production boundary object between Tauri commands and the C++ audio engine.
//!
//! `AudioBridge` is the single application-level owner of engine lifecycle and
//! the only legal production caller of [`super::ffi`]. Ownership, thread
//! ownership, memory lifetime, cancellation, and the event contract are
//! specified in `Docs/Audio-Bridge-Contract.md`.
//!
//! Design (see the implementation plan):
//! - Lock-splitting: public `fn x(&self)` locks the small state mutex and
//!   delegates to a private `fn x_locked(&self, st: &mut Inner)`; `*_locked`
//!   helpers never call other public methods. The Rust mutex only serializes
//!   command interleaving and state bookkeeping — the engine's own internal
//!   atomics/seqlocks guard its control plane.
//! - Events: a dedicated non-blocking Rust worker thread drains the engine's
//!   fixed lock-free event queue and delivers typed [`EngineEvent`]s through
//!   an [`EventSink`] (Tauri `pulse://audio-event` in production, in-memory
//!   `VecSink` in tests). Cancellation is stop-flag + final drain + join.

#![allow(unsafe_code)]

use std::ffi::CString;
use std::os::raw::c_int;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, MutexGuard, PoisonError};
use std::thread::JoinHandle;
use std::time::Duration;

use serde::{Deserialize, Serialize};
use tauri::Emitter;

use super::errors::AudioBridgeError;
use super::ffi;
use super::plan::TransitionPlan;
use super::types::{AudioEngineConfigC, AudioEngineStatsC, AudioEventC, DeckStateC, EngineEvent};

/// Tauri event name on which engine events are emitted to the UI.
pub const EVENT_NAME: &str = "pulse://audio-event";

/// Engine lifecycle phase as tracked by the bridge.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum EnginePhase {
    Uninitialized,
    Ready,
    Running,
}

/// Friendly engine configuration wrapper over `AudioEngineConfigC`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub struct EngineConfig {
    pub sample_rate: u32,
    pub buffer_size: u32,
    pub channel_count: u32,
}

impl From<EngineConfig> for AudioEngineConfigC {
    fn from(config: EngineConfig) -> Self {
        Self {
            sample_rate: config.sample_rate,
            buffer_size: config.buffer_size,
            channel_count: config.channel_count,
        }
    }
}

/// Friendly engine telemetry wrapper over `AudioEngineStatsC`.
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
pub struct EngineStats {
    pub sample_rate: u32,
    pub buffer_size: u32,
    pub channel_count: u32,
    pub is_initialized: bool,
    pub is_running: bool,
    pub total_frames_processed: u64,
    pub underrun_count: u32,
    pub cpu_load: f64,
}

impl From<AudioEngineStatsC> for EngineStats {
    fn from(stats: AudioEngineStatsC) -> Self {
        Self {
            sample_rate: stats.sample_rate,
            buffer_size: stats.buffer_size,
            channel_count: stats.channel_count,
            is_initialized: stats.is_initialized != 0,
            is_running: stats.is_running != 0,
            total_frames_processed: stats.total_frames_processed,
            underrun_count: stats.underrun_count,
            cpu_load: f64::from(stats.cpu_load),
        }
    }
}

/// Friendly per-deck state wrapper over `DeckStateC`
/// (`playback_state`: 0 Empty, 1 Loading, 2 Ready, 3 Playing, 4 Paused, 5 Error).
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
pub struct DeckState {
    pub deck_id: u8,
    pub is_playing: bool,
    pub playback_state: u8,
    pub preserve_pitch: bool,
    pub playback_position_seconds: f64,
    pub duration_seconds: f64,
    pub bpm: f64,
    pub tempo_ratio: f64,
    pub volume: f64,
    pub low_eq: f64,
    pub mid_eq: f64,
    pub high_eq: f64,
    pub filter: f64,
    pub vocal_stem_vol: f64,
    pub drum_stem_vol: f64,
    pub bass_stem_vol: f64,
    pub other_stem_vol: f64,
}

impl From<DeckStateC> for DeckState {
    fn from(state: DeckStateC) -> Self {
        Self {
            deck_id: state.deck_id,
            is_playing: state.is_playing != 0,
            playback_state: state.playback_state,
            preserve_pitch: state.preserve_pitch != 0,
            playback_position_seconds: state.playback_position_seconds,
            duration_seconds: state.duration_seconds,
            bpm: state.bpm,
            tempo_ratio: state.tempo_ratio,
            volume: f64::from(state.volume),
            low_eq: f64::from(state.low_eq),
            mid_eq: f64::from(state.mid_eq),
            high_eq: f64::from(state.high_eq),
            filter: f64::from(state.filter),
            vocal_stem_vol: f64::from(state.vocal_stem_vol),
            drum_stem_vol: f64::from(state.drum_stem_vol),
            bass_stem_vol: f64::from(state.bass_stem_vol),
            other_stem_vol: f64::from(state.other_stem_vol),
        }
    }
}

/// Delivery seam for engine events. Production uses [`TauriSink`]; tests use
/// [`VecSink`]. Implementations must be non-blocking (the worker thread calls
/// them per drained event).
pub trait EventSink: Send + Sync {
    fn emit(&self, event: &EngineEvent);
}

/// Production sink: emits typed events as Tauri app events on [`EVENT_NAME`].
pub struct TauriSink {
    app: tauri::AppHandle,
}

impl TauriSink {
    pub fn new(app: tauri::AppHandle) -> Self {
        Self { app }
    }
}

impl EventSink for TauriSink {
    fn emit(&self, event: &EngineEvent) {
        let _ = self.app.emit(EVENT_NAME, event);
    }
}

/// In-memory sink for tests: collects every emitted event.
#[derive(Default)]
pub struct VecSink {
    events: Mutex<Vec<EngineEvent>>,
}

impl VecSink {
    pub fn new() -> Self {
        Self {
            events: Mutex::new(Vec::new()),
        }
    }

    /// Snapshot of all events received so far (in delivery order).
    #[must_use]
    pub fn snapshot(&self) -> Vec<EngineEvent> {
        self.events
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .clone()
    }
}

impl EventSink for VecSink {
    fn emit(&self, event: &EngineEvent) {
        self.events
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .push(*event);
    }
}

/// The bridge: lifecycle, validation, error mapping, and event delivery.
///
/// SAFETY of the FFI calls below rests on: (1) every struct is `#[repr(C)]`
/// and mirrors the C header field-for-field, with size/offset assertions on
/// both sides (C header `static_assert`s, `types.rs` layout tests);
/// (2) all pointer arguments are caller-owned buffers that stay alive for the
/// duration of the synchronous call — the engine copies or consumes them and
/// never retains them; (3) the engine's control plane is internally
/// thread-safe (atomics/seqlocks) and this object additionally serializes
/// command interleaving with its state mutex.
pub struct AudioBridge {
    available: bool,
    sink: Arc<dyn EventSink>,
    inner: Mutex<Inner>,
}

struct Inner {
    phase: EnginePhase,
    worker: Option<JoinHandle<()>>,
    worker_stop: Arc<AtomicBool>,
}

const DRAIN_BATCH: usize = 64;
const DRAIN_IDLE_SLEEP: Duration = Duration::from_millis(8);

impl AudioBridge {
    /// Production constructor: events are emitted as Tauri app events.
    /// No engine action happens here — the engine opens only on `init`.
    #[must_use]
    pub fn new(app: tauri::AppHandle) -> Arc<Self> {
        Self::new_with_sink(Box::new(TauriSink::new(app)))
    }

    /// Test constructor with an explicit event sink (no Tauri app required).
    #[must_use]
    pub fn new_with_sink(sink: Box<dyn EventSink>) -> Arc<Self> {
        Arc::new(Self {
            available: !cfg!(pulse_audio_engine_unavailable),
            sink: Arc::from(sink),
            inner: Mutex::new(Inner {
                phase: EnginePhase::Uninitialized,
                worker: None,
                worker_stop: Arc::new(AtomicBool::new(false)),
            }),
        })
    }

    /// False when this build was compiled without the C++ engine (`CMake` was
    /// unavailable); every command then returns `EngineNotAvailable`.
    #[must_use]
    pub fn available(&self) -> bool {
        self.available
    }

    // ── Lifecycle ─────────────────────────────────────────────────────────

    /// Opens the engine with the given configuration and starts the event
    /// worker. Does not start audio output (call `start`).
    pub fn init(&self, config: EngineConfig) -> Result<(), AudioBridgeError> {
        let mut st = self.lock();
        self.init_locked(config, &mut st)
    }

    fn init_locked(&self, config: EngineConfig, st: &mut Inner) -> Result<(), AudioBridgeError> {
        if !self.available {
            return Err(AudioBridgeError::EngineNotAvailable);
        }
        let c_config = AudioEngineConfigC::from(config);
        // SAFETY: by-value POD; the engine validates the fields and retains nothing.
        map_ffi("audio_init", unsafe { ffi::pulse_audio_init(c_config) })?;
        st.phase = EnginePhase::Ready;
        self.start_worker(st);
        Ok(())
    }

    /// Starts audio output. Idempotent (engine `start` succeeds when running).
    pub fn start(&self) -> Result<(), AudioBridgeError> {
        let mut st = self.lock();
        self.start_locked(&mut st)
    }

    fn start_locked(&self, st: &mut Inner) -> Result<(), AudioBridgeError> {
        if !self.available {
            return Err(AudioBridgeError::EngineNotAvailable);
        }
        if st.phase == EnginePhase::Uninitialized {
            return Err(AudioBridgeError::NotInitialized);
        }
        // SAFETY: no-argument control-plane function.
        map_ffi("audio_start", unsafe { ffi::pulse_audio_start() })?;
        st.phase = EnginePhase::Running;
        Ok(())
    }

    /// Halts audio rendering; the engine stays initialized.
    pub fn stop(&self) -> Result<(), AudioBridgeError> {
        let mut st = self.lock();
        self.stop_locked(&mut st)
    }

    fn stop_locked(&self, st: &mut Inner) -> Result<(), AudioBridgeError> {
        if !self.available {
            return Err(AudioBridgeError::EngineNotAvailable);
        }
        if st.phase == EnginePhase::Uninitialized {
            return Err(AudioBridgeError::NotInitialized);
        }
        // SAFETY: no-argument control-plane function.
        map_ffi("audio_stop", unsafe { ffi::pulse_audio_stop() })?;
        st.phase = EnginePhase::Ready;
        Ok(())
    }

    /// Hard stop: halts rendering, tears down hardware, and stops the event
    /// worker. Idempotent.
    pub fn shutdown(&self) -> Result<(), AudioBridgeError> {
        let mut st = self.lock();
        self.shutdown_locked(&mut st)
    }

    fn shutdown_locked(&self, st: &mut Inner) -> Result<(), AudioBridgeError> {
        if !self.available {
            return Err(AudioBridgeError::EngineNotAvailable);
        }
        if st.phase != EnginePhase::Uninitialized {
            // SAFETY: no-argument control-plane function.
            map_ffi("audio_shutdown", unsafe { ffi::pulse_audio_shutdown() })?;
        }
        st.phase = EnginePhase::Uninitialized;
        self.stop_worker(st);
        Ok(())
    }

    /// Whether the engine is started (queried from the engine itself).
    #[must_use]
    pub fn is_running(&self) -> bool {
        if !self.available {
            return false;
        }
        // SAFETY: no-argument query; the engine reads an internal atomic.
        unsafe { ffi::pulse_audio_is_running() != 0 }
    }

    /// Whether the engine has been initialized (queried from the engine itself).
    #[must_use]
    pub fn is_initialized(&self) -> bool {
        if !self.available {
            return false;
        }
        // SAFETY: no-argument query; the engine reads an internal atomic.
        unsafe { ffi::pulse_audio_is_initialized() != 0 }
    }

    /// Engine telemetry. Requires a prior `init`.
    pub fn get_stats(&self) -> Result<EngineStats, AudioBridgeError> {
        let st = self.lock();
        self.get_stats_locked(&st)
    }

    fn get_stats_locked(&self, st: &Inner) -> Result<EngineStats, AudioBridgeError> {
        if !self.available {
            return Err(AudioBridgeError::EngineNotAvailable);
        }
        if st.phase == EnginePhase::Uninitialized {
            return Err(AudioBridgeError::NotInitialized);
        }
        let mut out = AudioEngineStatsC::default();
        // SAFETY: caller-owned out-param; the engine fills it and retains nothing.
        map_ffi("get_stats", unsafe {
            ffi::pulse_audio_get_stats(&raw mut out)
        })?;
        Ok(EngineStats::from(out))
    }

    // ── Deck loading / preparation ────────────────────────────────────────

    /// Loads a track file onto a deck (deck 0 or 1).
    pub fn load_track(&self, deck_id: u8, path: &str) -> Result<(), AudioBridgeError> {
        validate_deck_id(deck_id)?;
        let c_path = to_c_string(path)?;
        let st = self.lock();
        self.load_track_locked(deck_id, &c_path, &st)
    }

    fn load_track_locked(
        &self,
        deck_id: u8,
        path: &CString,
        st: &Inner,
    ) -> Result<(), AudioBridgeError> {
        require_available_initialized(self, st)?;
        // SAFETY: NUL-terminated, valid for the call's duration; the engine
        // copies the path into a std::string and retains nothing.
        let rc = unsafe { ffi::pulse_audio_load_track(deck_id, path.as_ptr()) };
        if rc != 0 {
            return Err(AudioBridgeError::EngineFailed("load_track".to_owned()));
        }
        Ok(())
    }

    /// Loads a track and applies cue/tempo/pitch settings in one step.
    pub fn prepare_deck(
        &self,
        deck_id: u8,
        path: &str,
        cue_seconds: f64,
        tempo_ratio: f64,
        preserve_pitch: bool,
    ) -> Result<(), AudioBridgeError> {
        validate_deck_id(deck_id)?;
        validate_finite("cue_seconds", cue_seconds)?;
        validate_finite("tempo_ratio", tempo_ratio)?;
        let c_path = to_c_string(path)?;
        let st = self.lock();
        self.prepare_deck_locked(
            deck_id,
            &c_path,
            cue_seconds,
            tempo_ratio,
            preserve_pitch,
            &st,
        )
    }

    fn prepare_deck_locked(
        &self,
        deck_id: u8,
        path: &CString,
        cue_seconds: f64,
        tempo_ratio: f64,
        preserve_pitch: bool,
        st: &Inner,
    ) -> Result<(), AudioBridgeError> {
        require_available_initialized(self, st)?;
        // SAFETY: NUL-terminated path valid for the call; by-value POD scalars.
        let rc = unsafe {
            ffi::pulse_audio_prepare_deck(
                deck_id,
                path.as_ptr(),
                cue_seconds,
                tempo_ratio,
                u8::from(preserve_pitch),
            )
        };
        if rc != 0 {
            return Err(AudioBridgeError::EngineFailed("prepare_deck".to_owned()));
        }
        Ok(())
    }

    // ── Playback control ──────────────────────────────────────────────────

    pub fn play(&self, deck_id: u8) -> Result<(), AudioBridgeError> {
        validate_deck_id(deck_id)?;
        let st = self.lock();
        self.play_locked(deck_id, &st)
    }

    fn play_locked(&self, deck_id: u8, st: &Inner) -> Result<(), AudioBridgeError> {
        require_available_initialized(self, st)?;
        // SAFETY: engine-owned deck validated above; no pointers.
        map_ffi("play", unsafe { ffi::pulse_audio_play(deck_id) })
    }

    pub fn pause(&self, deck_id: u8) -> Result<(), AudioBridgeError> {
        validate_deck_id(deck_id)?;
        let st = self.lock();
        self.pause_locked(deck_id, &st)
    }

    fn pause_locked(&self, deck_id: u8, st: &Inner) -> Result<(), AudioBridgeError> {
        require_available_initialized(self, st)?;
        // SAFETY: engine-owned deck validated above; no pointers.
        map_ffi("pause", unsafe { ffi::pulse_audio_pause(deck_id) })
    }

    pub fn stop_deck(&self, deck_id: u8) -> Result<(), AudioBridgeError> {
        validate_deck_id(deck_id)?;
        let st = self.lock();
        self.stop_deck_locked(deck_id, &st)
    }

    fn stop_deck_locked(&self, deck_id: u8, st: &Inner) -> Result<(), AudioBridgeError> {
        require_available_initialized(self, st)?;
        // SAFETY: engine-owned deck validated above; no pointers.
        map_ffi("stop_deck", unsafe { ffi::pulse_audio_stop_deck(deck_id) })
    }

    /// Seeks a deck to an absolute position in seconds (>= 0, finite).
    pub fn seek(&self, deck_id: u8, position_seconds: f64) -> Result<(), AudioBridgeError> {
        validate_deck_id(deck_id)?;
        let position = validate_seek_position(position_seconds)?;
        let st = self.lock();
        self.seek_locked(deck_id, position, &st)
    }

    fn seek_locked(&self, deck_id: u8, position: f64, st: &Inner) -> Result<(), AudioBridgeError> {
        require_available_initialized(self, st)?;
        // SAFETY: by-value POD scalars; the engine clamps to the track bounds.
        map_ffi("seek", unsafe { ffi::pulse_audio_seek(deck_id, position) })
    }

    // ── Deck parameters ───────────────────────────────────────────────────

    pub fn set_volume(&self, deck_id: u8, volume: f64) -> Result<(), AudioBridgeError> {
        validate_deck_id(deck_id)?;
        validate_finite("volume", volume)?;
        let st = self.lock();
        self.set_volume_locked(deck_id, volume, &st)
    }

    fn set_volume_locked(
        &self,
        deck_id: u8,
        volume: f64,
        st: &Inner,
    ) -> Result<(), AudioBridgeError> {
        require_available_initialized(self, st)?;
        // SAFETY: by-value POD scalar; the engine clamps to [0, 1].
        map_ffi("set_volume", unsafe {
            ffi::pulse_audio_set_volume(deck_id, volume as f32)
        })
    }

    pub fn set_eq(
        &self,
        deck_id: u8,
        low: f64,
        mid: f64,
        high: f64,
    ) -> Result<(), AudioBridgeError> {
        validate_deck_id(deck_id)?;
        validate_finite("low_eq", low)?;
        validate_finite("mid_eq", mid)?;
        validate_finite("high_eq", high)?;
        let st = self.lock();
        self.set_eq_locked(deck_id, low, mid, high, &st)
    }

    fn set_eq_locked(
        &self,
        deck_id: u8,
        low: f64,
        mid: f64,
        high: f64,
        st: &Inner,
    ) -> Result<(), AudioBridgeError> {
        require_available_initialized(self, st)?;
        // SAFETY: by-value POD scalars; the engine clamps to [-1, 1].
        map_ffi("set_eq", unsafe {
            ffi::pulse_audio_set_eq(deck_id, low as f32, mid as f32, high as f32)
        })
    }

    pub fn set_filter(&self, deck_id: u8, filter: f64) -> Result<(), AudioBridgeError> {
        validate_deck_id(deck_id)?;
        validate_finite("filter", filter)?;
        let st = self.lock();
        self.set_filter_locked(deck_id, filter, &st)
    }

    fn set_filter_locked(
        &self,
        deck_id: u8,
        filter: f64,
        st: &Inner,
    ) -> Result<(), AudioBridgeError> {
        require_available_initialized(self, st)?;
        // SAFETY: by-value POD scalar; the engine clamps to [-1, 1].
        map_ffi("set_filter", unsafe {
            ffi::pulse_audio_set_filter(deck_id, filter as f32)
        })
    }

    pub fn set_tempo_ratio(&self, deck_id: u8, ratio: f64) -> Result<(), AudioBridgeError> {
        validate_deck_id(deck_id)?;
        validate_finite("tempo_ratio", ratio)?;
        let st = self.lock();
        self.set_tempo_ratio_locked(deck_id, ratio, &st)
    }

    fn set_tempo_ratio_locked(
        &self,
        deck_id: u8,
        ratio: f64,
        st: &Inner,
    ) -> Result<(), AudioBridgeError> {
        require_available_initialized(self, st)?;
        // SAFETY: by-value POD scalar; the engine clamps to [0.25, 4.0].
        map_ffi("set_tempo_ratio", unsafe {
            ffi::pulse_audio_set_tempo_ratio(deck_id, ratio)
        })
    }

    pub fn set_pitch_preservation(
        &self,
        deck_id: u8,
        enabled: bool,
    ) -> Result<(), AudioBridgeError> {
        validate_deck_id(deck_id)?;
        let st = self.lock();
        self.set_pitch_preservation_locked(deck_id, enabled, &st)
    }

    fn set_pitch_preservation_locked(
        &self,
        deck_id: u8,
        enabled: bool,
        st: &Inner,
    ) -> Result<(), AudioBridgeError> {
        require_available_initialized(self, st)?;
        // SAFETY: by-value POD scalars.
        map_ffi("set_pitch_preservation", unsafe {
            ffi::pulse_audio_set_pitch_preservation(deck_id, u8::from(enabled))
        })
    }

    pub fn set_stem_levels(
        &self,
        deck_id: u8,
        vocal: f64,
        drum: f64,
        bass: f64,
        other: f64,
    ) -> Result<(), AudioBridgeError> {
        validate_deck_id(deck_id)?;
        validate_finite("vocal_stem", vocal)?;
        validate_finite("drum_stem", drum)?;
        validate_finite("bass_stem", bass)?;
        validate_finite("other_stem", other)?;
        let st = self.lock();
        self.set_stem_levels_locked(deck_id, vocal, drum, bass, other, &st)
    }

    fn set_stem_levels_locked(
        &self,
        deck_id: u8,
        vocal: f64,
        drum: f64,
        bass: f64,
        other: f64,
        st: &Inner,
    ) -> Result<(), AudioBridgeError> {
        require_available_initialized(self, st)?;
        // SAFETY: by-value POD scalars; the engine clamps to [0, 1].
        map_ffi("set_stem_levels", unsafe {
            ffi::pulse_audio_set_stem_levels(
                deck_id,
                vocal as f32,
                drum as f32,
                bass as f32,
                other as f32,
            )
        })
    }

    // ── Deck telemetry ────────────────────────────────────────────────────

    /// Toggles a deck between playing and paused.
    pub fn play_pause(&self, deck_id: u8, play: bool) -> Result<(), AudioBridgeError> {
        validate_deck_id(deck_id)?;
        let st = self.lock();
        self.play_pause_locked(deck_id, play, &st)
    }

    fn play_pause_locked(
        &self,
        deck_id: u8,
        play: bool,
        st: &Inner,
    ) -> Result<(), AudioBridgeError> {
        require_available_initialized(self, st)?;
        // SAFETY: by-value POD scalars.
        map_ffi("play_pause", unsafe {
            ffi::pulse_audio_play_pause(deck_id, u8::from(play))
        })
    }

    /// Whether a deck is currently playing.
    pub fn is_deck_playing(&self, deck_id: u8) -> bool {
        if !self.available || deck_id > 1 {
            return false;
        }
        // SAFETY: engine-owned deck validated above; returns 0/1.
        unsafe { ffi::pulse_audio_is_deck_playing(deck_id) != 0 }
    }

    pub fn get_deck_state(&self, deck_id: u8) -> Result<DeckState, AudioBridgeError> {
        validate_deck_id(deck_id)?;
        let st = self.lock();
        self.get_deck_state_locked(deck_id, &st)
    }

    fn get_deck_state_locked(
        &self,
        deck_id: u8,
        st: &Inner,
    ) -> Result<DeckState, AudioBridgeError> {
        require_available_initialized(self, st)?;
        let mut out = DeckStateC::default();
        // SAFETY: caller-owned out-param; the engine fills it and retains nothing.
        map_ffi("get_deck_state", unsafe {
            ffi::pulse_audio_get_deck_state(deck_id, &raw mut out)
        })?;
        Ok(DeckState::from(out))
    }

    pub fn get_deck_position(&self, deck_id: u8) -> Result<f64, AudioBridgeError> {
        validate_deck_id(deck_id)?;
        let st = self.lock();
        self.get_deck_position_locked(deck_id, &st)
    }

    fn get_deck_position_locked(&self, deck_id: u8, st: &Inner) -> Result<f64, AudioBridgeError> {
        require_available_initialized(self, st)?;
        // SAFETY: engine-owned deck validated above; returns by value.
        Ok(unsafe { ffi::pulse_audio_get_deck_position(deck_id) })
    }

    pub fn get_deck_duration(&self, deck_id: u8) -> Result<f64, AudioBridgeError> {
        validate_deck_id(deck_id)?;
        let st = self.lock();
        self.get_deck_duration_locked(deck_id, &st)
    }

    fn get_deck_duration_locked(&self, deck_id: u8, st: &Inner) -> Result<f64, AudioBridgeError> {
        require_available_initialized(self, st)?;
        // SAFETY: engine-owned deck validated above; returns by value.
        Ok(unsafe { ffi::pulse_audio_get_deck_duration(deck_id) })
    }

    // ── Transitions ───────────────────────────────────────────────────────

    /// Executes a fully precomputed transition plan on the engine.
    ///
    /// Pre-validates the plan structure at the Rust boundary; the C++ executor
    /// re-sanitizes every parameter (defense in depth, matching `plan.rs`).
    pub fn execute_transition(&self, plan: &TransitionPlan) -> Result<(), AudioBridgeError> {
        if !plan.is_valid_structure() {
            return Err(AudioBridgeError::InvalidInput(
                "transition plan is structurally invalid (deck ids must be 0/1 and differ)"
                    .to_owned(),
            ));
        }
        let st = self.lock();
        self.execute_transition_locked(plan, &st)
    }

    fn execute_transition_locked(
        &self,
        plan: &TransitionPlan,
        st: &Inner,
    ) -> Result<(), AudioBridgeError> {
        require_available_initialized(self, st)?;
        let command = plan.into_transition_command();
        // SAFETY: by-value POD v2 command; the engine sanitizes it and retains
        // only a fixed-size internal copy.
        map_ffi("execute_transition", unsafe {
            ffi::pulse_audio_execute_transition(command)
        })
    }

    // ── Event worker ──────────────────────────────────────────────────────

    fn start_worker(&self, st: &mut Inner) {
        if st.worker.is_some() {
            return;
        }
        st.worker_stop.store(false, Ordering::SeqCst);
        let stop = Arc::clone(&st.worker_stop);
        let sink = Arc::clone(&self.sink);
        let handle = std::thread::Builder::new()
            .name("pulse-audio-events".to_owned())
            .spawn(move || event_worker(stop, sink))
            .expect("event worker thread spawn (OS thread creation cannot fail silently)");
        st.worker = Some(handle);
    }

    fn stop_worker(&self, st: &mut Inner) {
        if let Some(handle) = st.worker.take() {
            st.worker_stop.store(true, Ordering::SeqCst);
            self.final_drain();
            let _ = handle.join();
        }
    }

    /// One last drain on the shutting-down thread so events emitted just
    /// before shutdown (e.g. `EngineStopped`/`EngineShutdown`) still reach the
    /// sink even if the worker has already exited.
    fn final_drain(&self) {
        let mut buffer = [AudioEventC::default(); DRAIN_BATCH];
        let mut dropped: u32 = 0;
        // SAFETY: caller-owned buffer; the engine writes at most its length in
        // records and retains nothing. `dropped` is a caller-owned out-param.
        let count = unsafe {
            ffi::pulse_audio_drain_events(buffer.as_mut_ptr(), DRAIN_BATCH as u32, &raw mut dropped)
        };
        for record in buffer.iter().take(bounded_count(count)) {
            // Safe: the engine wrote exactly `count` records at the start of
            // the buffer; `AudioEventC` is `Copy` and the buffer is local.
            self.sink.emit(&EngineEvent::from(*record));
        }
    }

    fn lock(&self) -> MutexGuard<'_, Inner> {
        self.inner.lock().unwrap_or_else(PoisonError::into_inner)
    }
}

impl Drop for AudioBridge {
    fn drop(&mut self) {
        // Best-effort hard stop: ignore errors (the engine may already be
        // gone, or the build may be degraded). Idempotent by construction.
        let _ = self.shutdown();
    }
}

// The worker owns its `Arc`s for the lifetime of the thread, so by-value is
// intentional despite only being read inside the loop.
#[allow(clippy::needless_pass_by_value)]
fn event_worker(stop: Arc<AtomicBool>, sink: Arc<dyn EventSink>) {
    let mut buffer = [AudioEventC::default(); DRAIN_BATCH];
    let mut dropped: u32 = 0;
    loop {
        // SAFETY: caller-owned buffer; the engine writes at most its length in
        // records and retains nothing. `dropped` is a caller-owned out-param.
        let count = unsafe {
            ffi::pulse_audio_drain_events(buffer.as_mut_ptr(), DRAIN_BATCH as u32, &raw mut dropped)
        };
        for record in buffer.iter().take(bounded_count(count)) {
            // Safe: the engine wrote exactly `count` records at the start of
            // the buffer; `AudioEventC` is `Copy` and the buffer is local.
            sink.emit(&EngineEvent::from(*record));
        }
        if stop.load(Ordering::SeqCst) {
            break;
        }
        if count == 0 {
            std::thread::sleep(DRAIN_IDLE_SLEEP);
        }
    }
}

/// Clamps an FFI count (contract: 0..=`DRAIN_BATCH`) defensively.
#[allow(clippy::cast_possible_wrap, clippy::cast_sign_loss)]
fn bounded_count(count: c_int) -> usize {
    // The contract bound (64) fits in any `c_int` on supported targets.
    count.clamp(0, DRAIN_BATCH as c_int) as usize
}

/// Maps a C control-plane result: 0 -> `Ok`, anything else -> `EngineFailed`.
fn map_ffi(operation: &str, result: c_int) -> Result<(), AudioBridgeError> {
    if result == 0 {
        Ok(())
    } else {
        Err(AudioBridgeError::EngineFailed(operation.to_owned()))
    }
}

fn validate_deck_id(deck_id: u8) -> Result<u8, AudioBridgeError> {
    if deck_id > 1 {
        Err(AudioBridgeError::InvalidDeckId(deck_id))
    } else {
        Ok(deck_id)
    }
}

fn validate_finite(name: &str, value: f64) -> Result<(), AudioBridgeError> {
    if value.is_finite() {
        Ok(())
    } else {
        Err(AudioBridgeError::InvalidInput(format!(
            "{name} must be a finite number"
        )))
    }
}

fn validate_seek_position(position: f64) -> Result<f64, AudioBridgeError> {
    if !position.is_finite() {
        return Err(AudioBridgeError::InvalidInput(
            "seek position must be a finite number".to_owned(),
        ));
    }
    if position < 0.0 {
        return Err(AudioBridgeError::InvalidInput(
            "seek position must be >= 0".to_owned(),
        ));
    }
    Ok(position)
}

fn to_c_string(path: &str) -> Result<CString, AudioBridgeError> {
    CString::new(path)
        .map_err(|_| AudioBridgeError::InvalidInput("path contains a NUL byte".to_owned()))
}

fn require_available_initialized(bridge: &AudioBridge, st: &Inner) -> Result<(), AudioBridgeError> {
    if !bridge.available {
        return Err(AudioBridgeError::EngineNotAvailable);
    }
    if st.phase == EnginePhase::Uninitialized {
        return Err(AudioBridgeError::NotInitialized);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::audio_bridge::types::EventKind;

    #[test]
    fn error_mapping_minus_one_is_engine_failed() {
        assert!(matches!(
            map_ffi("audio_start", -1),
            Err(AudioBridgeError::EngineFailed(op)) if op == "audio_start"
        ));
        assert!(map_ffi("audio_start", 0).is_ok());
        assert!(matches!(
            map_ffi("seek", 7),
            Err(AudioBridgeError::EngineFailed(op)) if op == "seek"
        ));
    }

    #[test]
    fn validation_rejects_invalid_deck_ids_without_engine() {
        assert!(validate_deck_id(0).is_ok());
        assert!(validate_deck_id(1).is_ok());
        for bad in [2u8, 7, 255] {
            assert!(
                matches!(validate_deck_id(bad), Err(AudioBridgeError::InvalidDeckId(d)) if d == bad),
                "deck {bad} must be rejected"
            );
        }
    }

    #[test]
    fn validation_rejects_non_finite_and_negative_params_without_engine() {
        assert!(validate_finite("tempo_ratio", 1.0).is_ok());
        assert!(matches!(
            validate_finite("tempo_ratio", f64::NAN),
            Err(AudioBridgeError::InvalidInput(_))
        ));
        assert!(matches!(
            validate_finite("volume", f64::INFINITY),
            Err(AudioBridgeError::InvalidInput(_))
        ));
        assert!(validate_seek_position(1.5).is_ok());
        assert!(matches!(
            validate_seek_position(f64::NAN),
            Err(AudioBridgeError::InvalidInput(_))
        ));
        assert!(matches!(
            validate_seek_position(-0.1),
            Err(AudioBridgeError::InvalidInput(_))
        ));
    }

    #[test]
    fn engine_event_serde_round_trip_every_kind() {
        let kinds = [
            EventKind::EngineStarted,
            EventKind::EngineStopped,
            EventKind::EngineShutdown,
            EventKind::Underrun,
            EventKind::TrackLoaded,
            EventKind::TrackLoadFailed,
            EventKind::DeckStateChanged,
            EventKind::TrackEnded,
            EventKind::TransitionStarted,
            EventKind::TransitionCompleted,
            EventKind::TransitionRejected,
            EventKind::Unknown(99),
        ];
        for kind in kinds {
            let event = EngineEvent {
                kind,
                deck: 1,
                code: 42,
                detail: 3.25,
            };
            let json = serde_json::to_string(&event).expect("serialize EngineEvent");
            let back: EngineEvent = serde_json::from_str(&json).expect("deserialize EngineEvent");
            assert_eq!(back.kind, kind, "kind must round-trip");
            assert_eq!(back.deck, 1);
            assert_eq!(back.code, 42);
            assert_eq!(back.detail, 3.25);
        }
    }

    #[test]
    fn unavailable_bridge_reports_engine_not_available() {
        // Pure Rust: no engine touched. A degraded-build bridge must refuse
        // every command with EngineNotAvailable.
        let bridge = AudioBridge::new_with_sink(Box::new(VecSink::new()));
        assert_eq!(bridge.available(), !cfg!(pulse_audio_engine_unavailable));
        if !bridge.available() {
            assert!(matches!(
                bridge.init(EngineConfig {
                    sample_rate: 48000,
                    buffer_size: 512,
                    channel_count: 2
                }),
                Err(AudioBridgeError::EngineNotAvailable)
            ));
            assert!(matches!(
                bridge.start(),
                Err(AudioBridgeError::EngineNotAvailable)
            ));
            assert!(matches!(
                bridge.stop(),
                Err(AudioBridgeError::EngineNotAvailable)
            ));
            assert!(matches!(
                bridge.shutdown(),
                Err(AudioBridgeError::EngineNotAvailable)
            ));
            assert!(matches!(
                bridge.load_track(0, "/x.wav"),
                Err(AudioBridgeError::EngineNotAvailable)
            ));
            assert!(matches!(
                bridge.get_stats(),
                Err(AudioBridgeError::EngineNotAvailable)
            ));
            assert!(!bridge.is_running());
            assert!(!bridge.is_initialized());
            assert!(!bridge.is_deck_playing(0));
        }
    }
}
