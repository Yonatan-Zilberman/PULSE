#pragma once

#include <cstdint>
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
    double playback_position_seconds;
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

// Transition Execution Command
typedef struct {
    uint8_t source_deck;
    uint8_t destination_deck;
    double duration_seconds;
    uint32_t transition_type;
} TransitionCommandC;

// Exported C FFI Functions
int pulse_audio_init(AudioEngineConfigC config);
int pulse_audio_shutdown(void);
int pulse_audio_load_track(uint8_t deck_id, const char* file_path);
int pulse_audio_play_pause(uint8_t deck_id, uint8_t play);
int pulse_audio_get_deck_state(uint8_t deck_id, DeckStateC* out_state);
int pulse_audio_execute_transition(TransitionCommandC command);

#ifdef __cplusplus
}

// Static assertions ensuring ABI compatibility and POD compliance
static_assert(std::is_standard_layout<AudioEngineConfigC>::value, "AudioEngineConfigC must be standard layout");
static_assert(std::is_standard_layout<DeckStateC>::value, "DeckStateC must be standard layout");
static_assert(std::is_standard_layout<TransitionCommandC>::value, "TransitionCommandC must be standard layout");

#endif
