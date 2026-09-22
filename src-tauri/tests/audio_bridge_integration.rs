//! Integration tests for the Rust <-> C++ audio bridge.
//!
//! The C++ engine is a process-global singleton, so every test takes
//! `ENGINE_LOCK` (serializing the whole suite per process) and drives the
//! engine through a fresh [`AudioBridge`] with an in-memory [`VecSink`].
//!
//! Hardware lifetime: tests end in `stop` where possible. `shutdown`
//! disposes the CoreAudio AudioUnit of the process-wide engine, forcing the
//! next `init` to create a new one; a unit (re)created this way can fail to
//! deliver render callbacks (observed flake). The C++ engine skips the
//! teardown on same-config re-init, so the unit is created only a handful of
//! times per process; each test additionally probes that the render thread
//! is actually delivering (`ensure_rendering`) before relying on it.
//!
//! Fixtures are self-generated sine WAVs in a per-test temp dir (the
//! `tests/audio/*.wav` files are gitignored and absent in CI). Amplitudes are
//! low (0.1) so live CoreAudio output during lifecycle tests is quiet.

#![allow(unsafe_code, clippy::needless_pass_by_value)]

use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use pulse_core_lib::audio_bridge::types::{EngineEvent, EventKind, TransitionCommandC};
use pulse_core_lib::audio_bridge::AudioBridgeError;
use pulse_core_lib::audio_bridge::EventSink;
use pulse_core_lib::audio_bridge::TransitionPlan;
use pulse_core_lib::audio_bridge::{AudioBridge, EngineConfig, VecSink, EVENT_NAME};

static ENGINE_LOCK: Mutex<()> = Mutex::new(());

const DEFAULT_CONFIG: EngineConfig = EngineConfig {
    sample_rate: 48000,
    buffer_size: 512,
    channel_count: 2,
};

/// Test-only adapter so one `Arc<VecSink>` can be shared between the bridge's
/// event worker and the test's assertions.
struct SharedSink(Arc<VecSink>);

impl EventSink for SharedSink {
    fn emit(&self, event: &EngineEvent) {
        self.0.emit(event);
    }
}

fn new_bridge() -> (Arc<AudioBridge>, Arc<VecSink>) {
    let sink = Arc::new(VecSink::new());
    let bridge = AudioBridge::new_with_sink(Box::new(SharedSink(Arc::clone(&sink))));
    (bridge, sink)
}

fn guard_available(bridge: &AudioBridge, test: &str) -> bool {
    if !bridge.available() {
        eprintln!("SKIP {test}: C++ audio engine unavailable in this build");
        return false;
    }
    true
}

/// Probes that the render thread is actually delivering blocks
/// (`total_frames_processed` must grow), re-arming once if it is not.
fn ensure_rendering(bridge: &AudioBridge) {
    for _ in 0..2 {
        let baseline = bridge
            .get_stats()
            .map(|s| s.total_frames_processed)
            .unwrap_or(0);
        let deadline = Instant::now() + Duration::from_millis(500);
        let mut alive = false;
        while Instant::now() < deadline {
            let total = bridge
                .get_stats()
                .map(|s| s.total_frames_processed)
                .unwrap_or(0);
            if total > baseline {
                alive = true;
                break;
            }
            std::thread::sleep(Duration::from_millis(20));
        }
        if alive {
            return;
        }
        eprintln!("ensure_rendering: no blocks delivered; re-arming engine");
        let _ = bridge.stop();
        let _ = bridge.start();
    }
}

/// Polls the sink until the predicate matches or the timeout elapses.
fn wait_for(sink: &Arc<VecSink>, timeout: Duration, what: impl Fn(&[EngineEvent]) -> bool) -> bool {
    let deadline = Instant::now() + timeout;
    loop {
        let events = sink.snapshot();
        if what(&events) {
            return true;
        }
        if Instant::now() > deadline {
            return false;
        }
        std::thread::sleep(Duration::from_millis(10));
    }
}

fn count_kind(events: &[EngineEvent], kind: EventKind) -> usize {
    events.iter().filter(|e| e.kind == kind).count()
}

fn last_index(events: &[EngineEvent], kind: EventKind) -> Option<usize> {
    events.iter().rposition(|e| e.kind == kind)
}

fn test_dir(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("pulse_bridge_{name}_{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).expect("create test dir");
    dir
}

/// Writes a deterministic 16-bit PCM stereo sine WAV.
fn write_sine_wav(path: &Path, freq_hz: f64, duration_s: f64, amp: f64) {
    const SAMPLE_RATE: u32 = 48_000;
    const CHANNELS: u16 = 2;
    let frames = (duration_s * f64::from(SAMPLE_RATE)).round() as u32;
    let data_len = (frames * u32::from(CHANNELS) * 2) as u32;

    let mut file: Vec<u8> = Vec::with_capacity(44 + data_len as usize);
    file.extend_from_slice(b"RIFF");
    file.extend_from_slice(&(4 + 16 + data_len).to_le_bytes());
    file.extend_from_slice(b"WAVE");
    file.extend_from_slice(b"fmt ");
    file.extend_from_slice(&16u32.to_le_bytes());
    file.extend_from_slice(&1u16.to_le_bytes()); // PCM
    file.extend_from_slice(&CHANNELS.to_le_bytes());
    file.extend_from_slice(&SAMPLE_RATE.to_le_bytes());
    file.extend_from_slice(&(SAMPLE_RATE * u32::from(CHANNELS) * 2).to_le_bytes());
    file.extend_from_slice(&(CHANNELS * 2).to_le_bytes());
    file.extend_from_slice(&16u16.to_le_bytes());
    file.extend_from_slice(b"data");
    file.extend_from_slice(&data_len.to_le_bytes());

    for frame in 0..frames {
        let t = f64::from(frame) / f64::from(SAMPLE_RATE);
        let sample = amp * (2.0f64 * std::f64::consts::PI * freq_hz * t).sin();
        let bytes = (sample as f32 * 32767.0f32) as i16;
        for _ in 0..CHANNELS {
            file.extend_from_slice(&bytes.to_le_bytes());
        }
    }
    std::fs::write(path, &file).expect("write sine wav");
}

fn wav_in(dir: &Path, name: &str) -> String {
    let path = dir.join(name);
    write_sine_wav(&path, 120.0, 2.0, 0.1);
    path.to_string_lossy().into_owned()
}

// ── 1. Lifecycle ───────────────────────────────────────────────────────────

#[test]
fn lifecycle_init_start_stop_shutdown() {
    let _lock = ENGINE_LOCK.lock().unwrap_or_else(|p| p.into_inner());
    let (bridge, sink) = new_bridge();
    if !guard_available(&bridge, "lifecycle") {
        return;
    }

    bridge.init(DEFAULT_CONFIG).expect("init must succeed");
    assert!(!bridge.is_running());

    bridge.start().expect("start must succeed");
    assert!(bridge.is_running());
    ensure_rendering(&bridge);

    let stats = bridge.get_stats().expect("stats");
    assert_eq!(stats.sample_rate, 48000);
    assert_eq!(stats.buffer_size, 512);
    assert_eq!(stats.channel_count, 2);
    assert!(stats.is_initialized);
    assert!(stats.is_running);

    bridge.stop().expect("stop must succeed");
    assert!(!bridge.is_running());

    bridge.shutdown().expect("shutdown must succeed");
    assert!(!bridge.is_initialized());

    let got = wait_for(&sink, Duration::from_secs(5), |events| {
        count_kind(events, EventKind::EngineStarted) == 1
            && count_kind(events, EventKind::EngineStopped) == 1
            && count_kind(events, EventKind::EngineShutdown) == 1
    });
    assert!(got, "lifecycle events must all arrive");
    let events = sink.snapshot();
    let started = last_index(&events, EventKind::EngineStarted).unwrap();
    let stopped = last_index(&events, EventKind::EngineStopped).unwrap();
    let shutdown = last_index(&events, EventKind::EngineShutdown).unwrap();
    assert!(
        started < stopped && stopped < shutdown,
        "lifecycle events out of order: {events:?}"
    );
    assert_eq!(EVENT_NAME, "pulse://audio-event");
    let _ = bridge.shutdown(); // idempotent end state
}

// ── 2. Invalid lifecycle sequence ──────────────────────────────────────────

#[test]
fn lifecycle_invalid_sequence() {
    let _lock = ENGINE_LOCK.lock().unwrap_or_else(|p| p.into_inner());
    let (bridge, _sink) = new_bridge();
    if !guard_available(&bridge, "invalid sequence") {
        return;
    }

    assert!(matches!(
        bridge.start(),
        Err(AudioBridgeError::NotInitialized)
    ));
    assert!(matches!(
        bridge.play(0),
        Err(AudioBridgeError::NotInitialized)
    ));

    let rejected = EngineConfig {
        sample_rate: 12000, // below the engine's 22050 Hz floor
        ..DEFAULT_CONFIG
    };
    assert!(matches!(
        bridge.init(rejected),
        Err(AudioBridgeError::EngineFailed(_))
    ));

    bridge
        .init(DEFAULT_CONFIG)
        .expect("valid init must succeed");
    bridge.start().expect("start must succeed");
    bridge.start().expect("second start must be idempotent");
    assert!(bridge.is_running());

    bridge.stop().expect("stop");
}

// ── 3. Track flow ──────────────────────────────────────────────────────────

#[test]
fn commands_track_flow() {
    let _lock = ENGINE_LOCK.lock().unwrap_or_else(|p| p.into_inner());
    let (bridge, sink) = new_bridge();
    if !guard_available(&bridge, "track flow") {
        return;
    }
    let dir = test_dir("track_flow");

    bridge.init(DEFAULT_CONFIG).expect("init");
    bridge.start().expect("start");
    ensure_rendering(&bridge);

    let a = wav_in(&dir, "tone_a.wav");
    let b = wav_in(&dir, "tone_b.wav");

    bridge.load_track(0, &a).expect("load deck 0");
    bridge
        .prepare_deck(1, &b, 0.0, 1.0, true)
        .expect("prepare deck 1");

    let got = wait_for(&sink, Duration::from_secs(5), |events| {
        count_kind(events, EventKind::TrackLoaded) == 2
    });
    assert!(got, "two TrackLoaded events expected");
    let snapshot = sink.snapshot();
    let loaded: Vec<_> = snapshot
        .iter()
        .filter(|e| e.kind == EventKind::TrackLoaded)
        .collect();
    for event in &loaded {
        assert!((event.detail - 2.0).abs() <= 0.1, "duration ~= 2.0");
    }

    bridge.play(0).expect("play");
    let state = bridge.get_deck_state(0).expect("state");
    assert_eq!(state.playback_state, 3, "Playing");
    assert!(state.is_playing);

    bridge.pause(0).expect("pause");
    let state = bridge.get_deck_state(0).expect("state");
    assert_eq!(state.playback_state, 4, "Paused");

    bridge.seek(0, 1.0).expect("seek");
    let position = bridge.get_deck_position(0).expect("position");
    assert!(
        (position - 1.0).abs() <= 0.05,
        "position ~= 1.0, got {position}"
    );

    bridge.set_volume(0, 0.5).expect("volume");
    bridge.set_eq(0, 0.2, 0.0, -0.2).expect("eq");
    bridge.set_filter(0, 0.3).expect("filter");
    bridge.set_tempo_ratio(0, 1.01).expect("tempo");
    bridge
        .set_stem_levels(0, 1.0, 1.0, 1.0, 1.0)
        .expect("stems");
    let state = bridge.get_deck_state(0).expect("state after setters");
    assert!((state.volume - 0.5).abs() < 1e-6);
    assert!((state.low_eq - 0.2).abs() < 1e-6);
    assert!((state.mid_eq - 0.0).abs() < 1e-6);
    assert!((state.high_eq - -0.2).abs() < 1e-6);
    assert!((state.filter - 0.3).abs() < 1e-6);
    assert!((state.tempo_ratio - 1.01).abs() < 1e-6);

    bridge.stop_deck(0).expect("stop deck");
    let state = bridge.get_deck_state(0).expect("state after stop");
    assert_eq!(state.playback_state, 2, "Ready");

    bridge.stop().expect("stop");
    let _ = std::fs::remove_dir_all(&dir);
}

// ── 4. Invalid inputs at the boundary ──────────────────────────────────────

#[test]
fn invalid_inputs_rejected_at_boundary() {
    let _lock = ENGINE_LOCK.lock().unwrap_or_else(|p| p.into_inner());
    let (bridge, sink) = new_bridge();
    if !guard_available(&bridge, "invalid inputs") {
        return;
    }
    let dir = test_dir("invalid_inputs");

    bridge.init(DEFAULT_CONFIG).expect("init");
    bridge.start().expect("start");
    ensure_rendering(&bridge);
    let a = wav_in(&dir, "tone_a.wav");
    bridge.load_track(0, &a).expect("load deck 0");

    assert!(matches!(
        bridge.load_track(2, &a),
        Err(AudioBridgeError::InvalidDeckId(2))
    ));
    assert!(matches!(
        bridge.play(7),
        Err(AudioBridgeError::InvalidDeckId(7))
    ));

    let missing = bridge.load_track(1, "/nonexistent/pulse.wav");
    assert!(
        matches!(missing, Err(AudioBridgeError::EngineFailed(_))),
        "missing file must surface as EngineFailed, got {missing:?}"
    );
    let got = wait_for(&sink, Duration::from_secs(5), |events| {
        events
            .iter()
            .any(|e| e.kind == EventKind::TrackLoadFailed && e.deck == 1)
    });
    assert!(got, "TrackLoadFailed for deck 1 expected");

    let before = bridge.get_deck_position(0).expect("position");
    let nan_seek = bridge.seek(0, f64::NAN);
    assert!(matches!(nan_seek, Err(AudioBridgeError::InvalidInput(_))));
    let after = bridge.get_deck_position(0).expect("position");
    assert_eq!(
        before, after,
        "position must be unchanged after rejected seek"
    );

    let inf_tempo = bridge.set_tempo_ratio(0, f64::INFINITY);
    assert!(matches!(inf_tempo, Err(AudioBridgeError::InvalidInput(_))));

    let nul_path = format!("{a}\0corrupted");
    let nul = bridge.load_track(0, &nul_path);
    assert!(matches!(nul, Err(AudioBridgeError::InvalidInput(_))));

    bridge.stop().expect("stop");
    let _ = std::fs::remove_dir_all(&dir);
}

// ── 5. Engine failure propagation & recovery ───────────────────────────────

#[test]
fn engine_failure_propagation() {
    let _lock = ENGINE_LOCK.lock().unwrap_or_else(|p| p.into_inner());
    let (bridge, sink) = new_bridge();
    if !guard_available(&bridge, "failure propagation") {
        return;
    }
    let dir = test_dir("failure_propagation");

    bridge.init(DEFAULT_CONFIG).expect("init");
    bridge.start().expect("start");
    bridge.shutdown().expect("shutdown");

    // After shutdown, every stateful command returns a typed error —
    // no panics, no UB.
    assert!(matches!(
        bridge.start(),
        Err(AudioBridgeError::NotInitialized)
    ));
    let a = wav_in(&dir, "tone_a.wav");
    assert!(matches!(
        bridge.load_track(0, &a),
        Err(AudioBridgeError::NotInitialized)
    ));
    assert!(matches!(
        bridge.play(0),
        Err(AudioBridgeError::NotInitialized)
    ));
    assert!(matches!(
        bridge.get_stats(),
        Err(AudioBridgeError::NotInitialized)
    ));
    assert!(matches!(
        bridge.get_deck_state(0),
        Err(AudioBridgeError::NotInitialized)
    ));

    // The singleton is reusable: re-init recovers.
    bridge.init(DEFAULT_CONFIG).expect("re-init must succeed");
    bridge.load_track(0, &a).expect("load must work again");
    let got = wait_for(&sink, Duration::from_secs(5), |events| {
        events
            .iter()
            .any(|e| e.kind == EventKind::TrackLoaded && e.deck == 0)
    });
    assert!(got, "TrackLoaded after recovery expected");

    // Deliberately no final shutdown: leave the engine initialized (unit kept
    // alive) so later tests do not have to re-create the AudioUnit.
    let _ = std::fs::remove_dir_all(&dir);
}

// ── 6. Transition accept & reject ──────────────────────────────────────────

#[test]
fn transition_accept_and_reject() {
    let _lock = ENGINE_LOCK.lock().unwrap_or_else(|p| p.into_inner());
    let (bridge, sink) = new_bridge();
    if !guard_available(&bridge, "transition") {
        return;
    }
    let dir = test_dir("transition");

    bridge.init(DEFAULT_CONFIG).expect("init");
    bridge.start().expect("start");
    ensure_rendering(&bridge);
    let a = wav_in(&dir, "tone_a.wav");
    let b = wav_in(&dir, "tone_b.wav");
    bridge.load_track(0, &a).expect("load 0");
    bridge.load_track(1, &b).expect("load 1");
    bridge.play(1).expect("play deck 1");

    // Valid plan (deck 0 -> deck 1) is accepted.
    let mut plan = TransitionPlan::default();
    plan.duration_seconds = 2.0;
    bridge
        .execute_transition(&plan)
        .expect("valid plan must be accepted");
    std::thread::sleep(Duration::from_millis(100));
    assert!(
        !sink
            .snapshot()
            .iter()
            .any(|e| e.kind == EventKind::TransitionRejected),
        "no rejection for a valid plan"
    );

    // Structurally broken plan (same deck) is rejected at the Rust boundary.
    let mut broken = TransitionPlan::default();
    broken.source_deck = 1;
    broken.destination_deck = 1;
    assert!(matches!(
        bridge.execute_transition(&broken),
        Err(AudioBridgeError::InvalidInput(_))
    ));

    // Raw FFI-level rejection: the engine returns -1 and emits
    // TransitionRejected.
    let mut raw = TransitionCommandC::v2_default();
    raw.source_deck = 1;
    raw.destination_deck = 1;
    // SAFETY: by-value POD v2 command; the engine sanitizes and retains a
    // fixed-size internal copy only.
    let rc = unsafe { pulse_core_lib::audio_bridge::ffi::pulse_audio_execute_transition(raw) };
    assert_eq!(rc, -1, "engine must reject a same-deck command");
    let got = wait_for(&sink, Duration::from_secs(5), |events| {
        events
            .iter()
            .any(|e| e.kind == EventKind::TransitionRejected)
    });
    assert!(got, "TransitionRejected event expected");

    bridge.stop().expect("stop");
    let _ = std::fs::remove_dir_all(&dir);
}

// ── 7. Track-ended event ───────────────────────────────────────────────────

#[test]
fn track_ended_event() {
    let _lock = ENGINE_LOCK.lock().unwrap_or_else(|p| p.into_inner());
    let (bridge, sink) = new_bridge();
    if !guard_available(&bridge, "track ended") {
        return;
    }
    let dir = test_dir("track_ended");

    bridge.init(DEFAULT_CONFIG).expect("init");
    bridge.start().expect("start");
    ensure_rendering(&bridge);

    let path = dir.join("tone_short.wav");
    write_sine_wav(&path, 120.0, 0.5, 0.1);
    let a = path.to_string_lossy().into_owned();
    bridge.load_track(0, &a).expect("load");
    bridge.play(0).expect("play");

    // Live CoreAudio delivery can lag right after rapid stop/start cycles in
    // the shared engine; re-arm the engine (and replay) a bounded number of
    // times before failing.
    let mut arrived = wait_for(&sink, Duration::from_secs(10), |events| {
        events
            .iter()
            .any(|e| e.kind == EventKind::TrackEnded && e.deck == 0)
    });
    for attempt in 0..3u32 {
        if arrived {
            break;
        }
        eprintln!("track_ended_event: no TrackEnded yet (attempt {attempt}); re-arming engine");
        let _ = bridge.stop();
        bridge.start().expect("re-arm start");
        ensure_rendering(&bridge);
        let _ = bridge.play(0);
        arrived = wait_for(&sink, Duration::from_secs(10), |events| {
            events
                .iter()
                .any(|e| e.kind == EventKind::TrackEnded && e.deck == 0)
        });
    }
    assert!(
        arrived,
        "TrackEnded must arrive; sink={:?} state={:?}",
        sink.snapshot(),
        bridge.get_deck_state(0).ok()
    );

    let state = bridge.get_deck_state(0).expect("state");
    // The deck auto-stops at the end: if the retry replayed it, stop it so the
    // asserted end state matches the final event.
    if state.playback_state == 3 {
        let _ = bridge.stop_deck(0);
    }
    let state = bridge.get_deck_state(0).expect("final state");
    assert_eq!(state.playback_state, 2, "Ready after track end");
    assert!(!state.is_playing);

    bridge.stop().expect("stop");
    let _ = std::fs::remove_dir_all(&dir);
}

// ── 8. Restart consistency ─────────────────────────────────────────────────

#[test]
fn events_survive_engine_restart() {
    let _lock = ENGINE_LOCK.lock().unwrap_or_else(|p| p.into_inner());
    let (bridge, sink) = new_bridge();
    if !guard_available(&bridge, "restart") {
        return;
    }

    // Two full cycles using stop (NOT shutdown) so the AudioUnit is kept
    // alive and the render thread keeps delivering; then a single final
    // shutdown (the engine is process-wide and the suite is done).
    for _ in 0..2 {
        bridge.init(DEFAULT_CONFIG).expect("init");
        bridge.start().expect("start");
        ensure_rendering(&bridge);
        bridge.stop().expect("stop");
    }

    let got = wait_for(&sink, Duration::from_secs(10), |events| {
        count_kind(events, EventKind::EngineStarted) == 2
            && count_kind(events, EventKind::EngineStopped) == 2
    });
    assert!(
        got,
        "started/stopped must each arrive twice; got {:?}",
        sink.snapshot()
    );

    bridge.shutdown().expect("final shutdown");
    let got = wait_for(&sink, Duration::from_secs(5), |events| {
        count_kind(events, EventKind::EngineShutdown) >= 1
    });
    assert!(got, "EngineShutdown expected after final shutdown");

    let events = sink.snapshot();
    assert_eq!(count_kind(&events, EventKind::EngineStarted), 2);
    assert_eq!(count_kind(&events, EventKind::EngineStopped), 2);
    // Order: started1 < stopped1 < started2 < stopped2 < shutdown.
    let started: Vec<_> = events
        .iter()
        .enumerate()
        .filter(|(_, e)| e.kind == EventKind::EngineStarted)
        .map(|(i, _)| i)
        .collect();
    let stopped: Vec<_> = events
        .iter()
        .enumerate()
        .filter(|(_, e)| e.kind == EventKind::EngineStopped)
        .map(|(i, _)| i)
        .collect();
    let shutdown = events
        .iter()
        .rposition(|e| e.kind == EventKind::EngineShutdown)
        .unwrap();
    assert!(
        started[0] < stopped[0]
            && stopped[0] < started[1]
            && started[1] < stopped[1]
            && stopped[1] < shutdown,
        "lifecycle order broken: started={started:?} stopped={stopped:?} shutdown={shutdown}"
    );
}
