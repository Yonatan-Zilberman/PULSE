# Audio Bridge Contract (Rust ↔ C++)

Living reference for the stable boundary between the Rust application layer
(`src-tauri/src/audio_bridge/`) and the C++ real-time audio engine
(`src-cpp/`). This document is the contract: any change to the items below
requires an explicit contract revision and a bump of the affected version tag.

Supersedes (and does not rewrite) the dated findings in `BASELINE.md`.

---

## 1. Frozen C ABI

The C ABI is declared in `src-cpp/include/AudioBridgeTypes.h` (26 exported
functions) and mirrored in `src-tauri/src/audio_bridge/ffi.rs`. All functions
return `int` status codes except the two `double` position/duration getters.

### 1.1 Functions

| Function | Signature (abridged) | Thread |
| --- | --- | --- |
| `pulse_audio_init` | `(AudioEngineConfigC) -> int` | control |
| `pulse_audio_start` | `(void) -> int` | control |
| `pulse_audio_stop` | `(void) -> int` | control |
| `pulse_audio_shutdown` | `(void) -> int` | control |
| `pulse_audio_is_running` | `(void) -> int` | any |
| `pulse_audio_is_initialized` | `(void) -> int` | any |
| `pulse_audio_get_stats` | `(AudioEngineStatsC*) -> int` | any |
| `pulse_audio_load_track` | `(uint8_t deck, const char* path) -> int` | control |
| `pulse_audio_prepare_deck` | `(uint8_t deck, const char* path, double cue, double tempo, uint8_t preservePitch) -> int` | control |
| `pulse_audio_play` | `(uint8_t deck) -> int` | control |
| `pulse_audio_pause` | `(uint8_t deck) -> int` | control |
| `pulse_audio_stop_deck` | `(uint8_t deck) -> int` | control |
| `pulse_audio_seek` | `(uint8_t deck, double pos) -> int` | control |
| `pulse_audio_play_pause` | `(uint8_t deck, uint8_t play) -> int` | control |
| `pulse_audio_set_volume` | `(uint8_t deck, float) -> int` | control |
| `pulse_audio_set_eq` | `(uint8_t deck, float low, float mid, float high) -> int` | control |
| `pulse_audio_set_filter` | `(uint8_t deck, float) -> int` | control |
| `pulse_audio_set_tempo_ratio` | `(uint8_t deck, double) -> int` | control |
| `pulse_audio_set_pitch_preservation` | `(uint8_t deck, uint8_t) -> int` | control |
| `pulse_audio_set_stem_levels` | `(uint8_t deck, float vocal, float drum, float bass, float other) -> int` | control |
| `pulse_audio_get_deck_state` | `(uint8_t deck, DeckStateC*) -> int` | any |
| `pulse_audio_is_deck_playing` | `(uint8_t deck) -> int` | any |
| `pulse_audio_get_deck_position` | `(uint8_t deck) -> double` | any |
| `pulse_audio_get_deck_duration` | `(uint8_t deck) -> double` | any |
| `pulse_audio_execute_transition` | `(TransitionCommandC) -> int` | control |
| `pulse_audio_drain_events` | `(AudioEventC* out, uint32_t max, uint32_t* out_dropped) -> int` | any (sole consumer) |

"control" = must be called from the control plane (see §4); "any" = safe from
any thread (lock-free atomics / seqlocks inside the engine).

### 1.2 Structs

| Struct | Size | Alignment | Version tag | Notes |
| --- | --- | --- | --- | --- |
| `AudioEngineConfigC` | 12 | 4 | — (append-only) | `sample_rate`, `buffer_size`, `channel_count` (`uint32_t`) |
| `DeckStateC` | 64 | 8 | — (append-only) | ids/flags `uint8_t`, position/duration/bpm/tempo `double`, gains `float` |
| `TransitionCommandC` | 120 | 8 | `version` @ 0 (v2) | `version != 1` ⇒ all fields sanitized to safe defaults (fail-safe, still executes) |
| `AudioEngineStatsC` | 24 | 8 | — (append-only) | config echo, `is_initialized`/`is_running`, `total_frames_processed`, `underrun_count`, `cpu_load` |
| `AudioEventC` | 32 | 8 | `version` @ 0 (v1) | see §2 |

All structs are standard-layout POD; `static_assert`s on size and field
offsets exist on both sides (C++: `AudioBridgeTypes.h`; Rust:
`audio_bridge::types` layout tests) and CI catches any drift.

### 1.3 Versioning policy

- **Append-only**: new fields are added at the end of a struct only; fields
  are never inserted, reordered, or removed.
- **Version-tagged structs** (`TransitionCommandC`, `AudioEventC`) carry a
  `version` in their first field. A receiver that sees an unknown version
  fails safe (sanitizes / ignores), never crashes.
- **Functions**: the symbol set is append-only. The Rust side links against
  the symbols it knows; a newer engine may add symbols, an older engine simply
  lacks them (the Rust `ffi` stubs degrade to `EngineNotAvailable`).
- Rust mirrors the layout with `#[repr(C)]` structs; offset tests in
  `audio_bridge::types` are part of the contract's enforcement.

---

## 2. Event contract (`AudioEventC`)

```
offset  field       size
@0      version     u32   (must be 1)
@4      kind        u32   (AudioEventKindC)
@8      deck_id     u8    (0 | 1; 255 = engine/transition-wide)
@9–15   _pad0       7     (alignment for detail)
@16     code        i32   (semantics per kind, below)
@20     _pad1       u32
@24     detail      f64   (seconds, or 0.0 when unused)
```

Transport: the engine pushes into a fixed 512-slot lock-free MPMC ring
(`EventQueue`, seqlock-guarded, bounded 8-CAS push, drop-on-overflow with a
cumulative drop counter). The **Rust event worker** is the sole consumer via
`pulse_audio_drain_events`. Overflow drops silently count — never blocks RT.

### 2.1 Kinds and payload semantics

| kind | value | deck_id | code | detail |
| --- | --- | --- | --- | --- |
| `EngineStarted` | 1 | 255 | 0 | 0.0 |
| `EngineStopped` | 2 | 255 | 0 | 0.0 |
| `EngineShutdown` | 3 | 255 | 0 | 0.0 |
| `Underrun` | 4 | 255 | 0 | 0.0 (RT edge: one event per underrun block; `out_dropped` reports queue drops) |
| `TrackLoaded` | 5 | 0 \| 1 | 0 | track duration (s) |
| `TrackLoadFailed` | 6 | 0 \| 1 | −1 | 0.0 |
| `DeckStateChanged` | 7 | 0 \| 1 | new `playback_state` (0 Empty, 1 Loading, 2 Ready, 3 Playing, 4 Paused, 5 Error) | playback position (s) |
| `TrackEnded` | 8 | 0 \| 1 | 0 | position (s) at end |
| `TransitionStarted` | 9 | 255 | 0 | 0.0 |
| `TransitionCompleted` | 10 | 255 | 0 | 0.0 |
| `TransitionRejected` | 11 | 255 | −1 | 0.0 |

Kinds emitted by newer engines than a given Rust binary knows are surfaced as
`EventKind::Unknown(n)` and still delivered.

### 2.2 Emission points

- **Control plane** (after the engine call, on the caller thread): lifecycle
  (`start`/`stop`/`shutdown`), load/prepare success & failure, deck state
  changes for `play`/`pause`/`stop_deck`/`seek`, transition rejection.
- **Real-time** (render thread, before/after block processing): edge-delta of
  sampled state (`RtEventSample` seqlock) → `TransitionStarted` /
  `TransitionCompleted` / `TrackEnded` / `Underrun`. Sampling is *around*
  existing DSP calls only; no allocation, no locks, no IO on RT.

Ordering is total across all emission points (single queue); within one
render block, RT edges follow block processing order.

---

## 3. Ownership

- The C++ `AudioEngine` is a **process singleton owned by no one**: no C++
  API exposes it by pointer; everything goes through the C functions.
- The Rust `AudioBridge` (stored in Tauri managed state, `Arc<AudioBridge>`)
  is the **single application-level owner** of the engine lifecycle and the
  **only legal caller** of `audio_bridge::ffi` in production code.
- `AudioBridge::Drop` performs `stop()` + `shutdown()` (idempotent on the C++
  side) and joins the event worker — the engine is never left running past
  process exit paths, and tests rely on this for cleanup.

## 4. Thread ownership

| Plane | Thread(s) | Rules |
| --- | --- | --- |
| Control | Tauri main / command threads | All control-plane calls are serialized by the `AudioBridge` internal mutex (lock-splitting: FFI call happens while locked, event drain does not hold it). |
| Real-time | CoreAudio render thread | Engine-internal; atomics + seqlocks only. Never blocks, never allocates (bounded 8-CAS queue push). |
| Event worker | One dedicated Rust `std::thread` | Sole drain consumer; polls `pulse_audio_drain_events` on a short sleep; forwards typed `EngineEvent`s to the configured sink; cancelled via stop-flag + join in `Drop`. |

The `AudioBridge` mutex is **never held** while the event worker runs and
never held across a drain; the RT thread never touches Rust.

## 5. Memory lifetime

- The ABI is **by-value POD or caller-allocated out-params** only. Out
  buffers (`AudioEngineStatsC*`, `DeckStateC*`, `AudioEventC*` arrays) are
  owned by the Rust caller for the duration of the call; C++ never retains a
  caller pointer past the call.
- `CString` lifetimes are scoped to the individual FFI call that uses them.
- The engine event queue is **fixed at construction** (512 slots, no growth,
  no allocation on push); overflow drops the newest event and increments the
  drop counter.
- `AudioBridge` drop order: signal worker stop → join worker (final drain) →
  `stop()` → `shutdown()`.

## 6. Cancellation

- **Transitions are preempted, not cancelled** (per `TransitionExecutor.h`):
  issuing a new `execute_transition` overwrites the pending slot on the next
  RT block; there is no in-flight abort. `TransitionRejected` is emitted
  when a transition cannot start (invalid plan / engine not running).
- `stop()` halts rendering (engine → ready, `EngineStopped`); `shutdown()`
  is the hard stop (`EngineShutdown`) and is idempotent.
- The event worker cancels via a stop-flag + join and the whole shutdown is
  idempotent in `Drop` (safe under double-drop guards and test teardown).

## 7. Error codes

Rust `AudioBridgeError` (serialized for the JS layer; `thiserror` +
`Serialize`):

| Variant | Meaning |
| --- | --- |
| `NotInitialized` | A lifecycle command ran before `audio_init`. |
| `NotRunning` | A command requires `audio_start`. |
| `InvalidDeckId(u8)` | Deck ids must be 0 or 1. |
| `InvalidInput(String)` | A numeric/string parameter failed validation at the Rust boundary (non-finite value, negative position, NUL byte in path, …). |
| `EngineFailed(String)` | The C++ engine reported a failure for a control-plane operation. |
| `EngineNotAvailable` | This build has no C++ audio engine (`CMake` unavailable at build time). |

C-side: all functions return `0` on success, negative on error; the drain
call returns `0..=max_events` copied.

## 8. `pulse://audio-event` payload (JS layer)

The production sink emits the typed `EngineEvent` as a Tauri app event named
`"pulse://audio-event"`:

```json
{
  "kind": "track_ended",
  "deck": 0,
  "code": 0,
  "detail": 128.42
}
```

- `kind`: snake-case `EventKind` (`engine_started`, `engine_stopped`,
  `engine_shutdown`, `underrun`, `track_loaded`, `track_load_failed`,
  `deck_state_changed`, `track_ended`, `transition_started`,
  `transition_completed`, `transition_rejected`; unknown C kind values
  serialize as `{"unknown": <raw kind>}`).
- `deck`: `0 | 1`, or `255` for engine/transition-wide events.
- `code` / `detail`: as in §2.1.

The UI subscribes with `listen("pulse://audio-event", …)`; event delivery is
best-effort — a dropped queue event or a failed Tauri emit is not retried.
