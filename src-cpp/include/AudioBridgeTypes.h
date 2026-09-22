#pragma once

#include <cstdint>
#include <cstddef>
#include <type_traits>

#ifdef __cplusplus
extern "C" {
#endif

// Real-Time Audio Engine Configuration
typedef struct {
    uint32_t sample_rate;
    uint32_t buffer_size;
    uint32_t channel_count;
} AudioEngineConfigC;

// Per-Deck Telemetry & Real-Time Parameter State
typedef struct {
    uint8_t deck_id; // 0 = Deck A, 1 = Deck B
    uint8_t is_playing;
    uint8_t playback_state; // 0 = Empty, 1 = Loading, 2 = Ready, 3 = Playing, 4 = Paused, 5 = Error
    uint8_t preserve_pitch;
    double playback_position_seconds;
    double duration_seconds;
    double bpm;
    double tempo_ratio;
    float volume;
    float low_eq;
    float mid_eq;
    float high_eq;
    float filter;
    float vocal_stem_vol;
    float drum_stem_vol;
    float bass_stem_vol;
    float other_stem_vol;
} DeckStateC;

// Transition Execution Command (versioned full-plan layout)
//
// v2: the application layer (Rust core / DJ Brain) prepares ALL transition
// parameters ahead of time; the audio engine executes this plan deterministically.
// Layout is POD / 8-byte aligned / 120 bytes. Offsets: version@0, duration_seconds@16,
// transition_type@112. Field bounds are documented on the executor and are enforced by
// TransitionExecutor::startTransition sanitization (non-finite -> safe default, clamp).
typedef struct {
    uint32_t version;                    // 1 = v2. Any other value -> all fields sanitized to safe defaults (fail-safe, still executes)
    uint8_t source_deck;                 // 0 | 1 (must differ from destination_deck)
    uint8_t destination_deck;            // 0 | 1
    uint8_t crossfader_curve;            // CrossfaderCurveType: 0 = eq-power, 1 = linear, 2 = s-curve
    uint8_t flags;                       // bit0 = customCrossfader (else endpoints derived from deck ids)
    float _pad0;                         // 0.0f (8-byte alignment for duration_seconds)
    double duration_seconds;             // [0.1, 600.0]
    float src_tempo_ratio;               // [0.5, 2.0]
    float dst_tempo_ratio;               // [0.5, 2.0] (pre-stretched)
    float dst_tempo_ramp_seconds;        // [0.0, 300.0] (post-cut return to 1.0)
    float src_gain;                      // [0.0, 1.0]
    float dst_gain;                      // [0.0, 1.0]
    float src_low_eq;                    // [-1.0, 1.0]
    float src_mid_eq;                    // [-1.0, 1.0]
    float src_high_eq;                   // [-1.0, 1.0]
    float dst_low_eq;                    // [-1.0, 1.0]
    float dst_mid_eq;                    // [-1.0, 1.0]
    float dst_high_eq;                   // [-1.0, 1.0]
    float src_filter;                    // [-1.0, 1.0]
    float dst_filter;                    // [-1.0, 1.0]
    float src_vocal_stem;                // [0.0, 1.0]
    float dst_vocal_stem;                // [0.0, 1.0]
    float crossfader_start;              // [-1.0, 1.0]
    float crossfader_end;                // [-1.0, 1.0]
    float phase_sync_end;                // [0,1], default 0.500  (phase 1 -> 2 boundary)
    float phase_eq_end;                  // [0,1], default 0.875  (phase 2 -> 3 boundary)
    float phase_vocal_end;               // [0,1], default 1.000  (phase 3 -> 4 / cut)
    float bass_swap_point;               // [0.1, 0.9], default 0.50
    float bass_swap_window;              // [0.02, 0.50], default 0.10
    uint32_t transition_type;            // TransitionStrategyType (unknown values -> PhraseCrossfade)
    uint32_t _pad1;                      // 0
} TransitionCommandC;

// Master Audio Engine Operational Telemetry
typedef struct {
    uint32_t sample_rate;
    uint32_t buffer_size;
    uint32_t channel_count;
    uint8_t is_initialized;
    uint8_t is_running;
    uint8_t pad[2];
    uint64_t total_frames_processed;
    uint32_t underrun_count;
    float cpu_load;
} AudioEngineStatsC;

// Exported C FFI Functions
int pulse_audio_init(AudioEngineConfigC config);
int pulse_audio_start(void);
int pulse_audio_stop(void);
int pulse_audio_shutdown(void);
int pulse_audio_is_running(void);
int pulse_audio_is_initialized(void);
int pulse_audio_get_stats(AudioEngineStatsC* out_stats);
int pulse_audio_load_track(uint8_t deck_id, const char* file_path);
int pulse_audio_prepare_deck(uint8_t deck_id, const char* file_path, double cue_seconds, double tempo_ratio, uint8_t preserve_pitch);
int pulse_audio_play(uint8_t deck_id);
int pulse_audio_pause(uint8_t deck_id);
int pulse_audio_stop_deck(uint8_t deck_id);
int pulse_audio_seek(uint8_t deck_id, double position_seconds);
int pulse_audio_play_pause(uint8_t deck_id, uint8_t play);
int pulse_audio_set_volume(uint8_t deck_id, float volume);
int pulse_audio_set_eq(uint8_t deck_id, float low, float mid, float high);
int pulse_audio_set_filter(uint8_t deck_id, float filter_val);
int pulse_audio_set_tempo_ratio(uint8_t deck_id, double ratio);
int pulse_audio_set_pitch_preservation(uint8_t deck_id, uint8_t enabled);
int pulse_audio_set_stem_levels(uint8_t deck_id, float vocal, float drum, float bass, float other);
int pulse_audio_get_deck_state(uint8_t deck_id, DeckStateC* out_state);
int pulse_audio_is_deck_playing(uint8_t deck_id);
double pulse_audio_get_deck_position(uint8_t deck_id);
double pulse_audio_get_deck_duration(uint8_t deck_id);
int pulse_audio_execute_transition(TransitionCommandC command);

// Engine -> Application Event Contract (versioned, append-only)
//
// The engine pushes lifecycle/playback/error events into an internal fixed
// 512-slot lock-free queue; the application layer (Rust) pulls them with
// pulse_audio_drain_events. When the queue overflows, events are dropped
// (bounded) and the cumulative drop count is reported via out_dropped.
typedef enum {
    PULSE_EVT_ENGINE_STARTED = 1,
    PULSE_EVT_ENGINE_STOPPED = 2,
    PULSE_EVT_ENGINE_SHUTDOWN = 3,
    PULSE_EVT_UNDERRUN = 4,
    PULSE_EVT_TRACK_LOADED = 5,
    PULSE_EVT_TRACK_LOAD_FAILED = 6,
    PULSE_EVT_DECK_STATE_CHANGED = 7,
    PULSE_EVT_TRACK_ENDED = 8,
    PULSE_EVT_TRANSITION_STARTED = 9,
    PULSE_EVT_TRANSITION_COMPLETED = 10,
    PULSE_EVT_TRANSITION_REJECTED = 11
} AudioEventKindC;

// AudioEventC — 32 bytes, 8-byte aligned, append-only.
// Offsets: version@0, kind@4, deck_id@8, code@16, detail@24.
typedef struct {
    uint32_t version;      // @0 — must be 1
    uint32_t kind;         // @4 — AudioEventKindC
    uint8_t deck_id;       // @8 — 0|1; 255 = engine/transition-wide
    uint8_t _pad0[7];      // @9-15 (alignment for detail)
    int32_t code;          // @16-19 — semantics per kind (see Docs/Audio-Bridge-Contract.md)
    uint32_t _pad1;        // @20-23
    double detail;         // @24-31 — seconds (position/duration) or unused
} AudioEventC;

/**
 * Drain engine events into a caller-allocated buffer.
 * @param out Caller buffer (must be non-null when max_events > 0).
 * @param max_events Buffer capacity in events.
 * @param out_dropped Optional out-param receiving the cumulative dropped-event count.
 * @return Number of events copied (0..max_events). null/0 is a no-op returning 0.
 */
int pulse_audio_drain_events(AudioEventC* out, uint32_t max_events, uint32_t* out_dropped);

#ifdef __cplusplus
}

// Static assertions ensuring ABI compatibility and POD compliance
static_assert(std::is_standard_layout<AudioEngineConfigC>::value, "AudioEngineConfigC must be standard layout");
static_assert(std::is_standard_layout<DeckStateC>::value, "DeckStateC must be standard layout");
static_assert(std::is_standard_layout<TransitionCommandC>::value, "TransitionCommandC must be standard layout");
static_assert(sizeof(TransitionCommandC) == 120, "TransitionCommandC v2 layout must be 120 bytes");
static_assert(offsetof(TransitionCommandC, version) == 0, "TransitionCommandC v2: version at offset 0");
static_assert(offsetof(TransitionCommandC, duration_seconds) == 16, "TransitionCommandC v2: duration_seconds at offset 16");
static_assert(offsetof(TransitionCommandC, transition_type) == 112, "TransitionCommandC v2: transition_type at offset 112");
static_assert(std::is_standard_layout<AudioEngineStatsC>::value, "AudioEngineStatsC must be standard layout");
static_assert(std::is_standard_layout<AudioEventC>::value, "AudioEventC must be standard layout");
static_assert(sizeof(AudioEventC) == 32, "AudioEventC layout must be 32 bytes");
static_assert(offsetof(AudioEventC, version) == 0, "AudioEventC: version at offset 0");
static_assert(offsetof(AudioEventC, kind) == 4, "AudioEventC: kind at offset 4");
static_assert(offsetof(AudioEventC, deck_id) == 8, "AudioEventC: deck_id at offset 8");
static_assert(offsetof(AudioEventC, code) == 16, "AudioEventC: code at offset 16");
static_assert(offsetof(AudioEventC, detail) == 24, "AudioEventC: detail at offset 24");

#endif
