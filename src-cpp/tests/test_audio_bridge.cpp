#include "../include/AudioBridgeTypes.h"
#include "../include/WavWriter.h"
#include <iostream>
#include <cassert>
#include <filesystem>
#include <cmath>

int main() {
    std::cout << "Running PULSE C++ Audio Engine Smoke Test..." << std::endl;

    // 1. Test ABI Sizes
    std::cout << "Testing struct alignments and sizes..." << std::endl;
    assert(sizeof(AudioEngineConfigC) == 12);
    assert(sizeof(DeckStateC) >= 48);
    assert(sizeof(AudioEngineStatsC) == 32);

    // 2. Test Audio Engine Initialization
    AudioEngineConfigC config{48000, 512, 2};
    int initRes = pulse_audio_init(config);
    assert(initRes == 0);
    assert(pulse_audio_is_initialized() == 1);
    assert(pulse_audio_is_running() == 0);
    std::cout << "Audio engine initialized successfully." << std::endl;

    // 3. Test Start / Stop Lifecycle
    int startRes = pulse_audio_start();
    assert(startRes == 0);
    assert(pulse_audio_is_running() == 1);
    std::cout << "Audio engine started successfully." << std::endl;

    AudioEngineStatsC stats{};
    int statsRes = pulse_audio_get_stats(&stats);
    assert(statsRes == 0);
    assert(stats.sample_rate == 48000);
    assert(stats.buffer_size == 512);
    assert(stats.channel_count == 2);
    assert(stats.is_initialized == 1);
    assert(stats.is_running == 1);

    int stopRes = pulse_audio_stop();
    assert(stopRes == 0);
    assert(pulse_audio_is_running() == 0);
    std::cout << "Audio engine stopped successfully." << std::endl;

    // 4. Test Deck State Retrieval
    DeckStateC deckAState{};
    int stateRes = pulse_audio_get_deck_state(0, &deckAState);
    assert(stateRes == 0);
    assert(deckAState.deck_id == 0);
    assert(deckAState.is_playing == 0);
    std::cout << "Deck A state verified: ID=" << static_cast<int>(deckAState.deck_id)
              << ", is_playing=" << static_cast<int>(deckAState.is_playing) << std::endl;

    // 5. Test Track Loading via C FFI
    std::filesystem::path tempDir = std::filesystem::temp_directory_path() / "pulse_bridge_test";
    std::filesystem::create_directories(tempDir);
    std::string testWav = (tempDir / "bridge_test.wav").string();
    pulse::audio::WavWriter::createSyntheticFixture(testWav, 440.0, 5.0, 120.0, 0.8f, 48000);

    assert(pulse_audio_load_track(0, testWav.c_str()) == 0);
    pulse_audio_get_deck_state(0, &deckAState);
    assert(deckAState.playback_state == 2); // Ready
    assert(deckAState.duration_seconds > 4.5);

    // 6. Test Play/Pause/Stop/Seek & Parameter Controls
    int playRes = pulse_audio_play(0);
    assert(playRes == 0);
    assert(pulse_audio_is_deck_playing(0) == 1);
    pulse_audio_get_deck_state(0, &deckAState);
    assert(deckAState.is_playing == 1);
    assert(deckAState.playback_state == 3); // Playing
    std::cout << "Deck A playback state toggled to PLAYING via pulse_audio_play." << std::endl;

    assert(pulse_audio_set_volume(0, 0.75f) == 0);
    assert(pulse_audio_set_eq(0, -0.5f, 0.2f, 0.1f) == 0);
    assert(pulse_audio_set_tempo_ratio(0, 1.05) == 0);
    assert(pulse_audio_set_pitch_preservation(0, 1) == 0);
    assert(pulse_audio_set_stem_levels(0, 1.0f, 0.8f, 0.9f, 1.0f) == 0);

    pulse_audio_get_deck_state(0, &deckAState);
    assert(std::abs(deckAState.volume - 0.75f) < 1e-4f);
    assert(std::abs(deckAState.low_eq - (-0.5f)) < 1e-4f);
    assert(std::abs(deckAState.tempo_ratio - 1.05) < 1e-4);
    assert(deckAState.preserve_pitch == 1);

    assert(pulse_audio_seek(0, 2.5) == 0);
    assert(pulse_audio_pause(0) == 0);
    assert(pulse_audio_is_deck_playing(0) == 0);
    pulse_audio_get_deck_state(0, &deckAState);
    assert(deckAState.is_playing == 0);

    assert(pulse_audio_stop_deck(0) == 0);
    assert(pulse_audio_is_deck_playing(0) == 0);

    // 6. Test Transition Command Execution (v2 full-plan command; remaining fields keep safe defaults)
    TransitionCommandC cmd{};
    cmd.version = 1;
    cmd.source_deck = 0;
    cmd.destination_deck = 1;
    cmd.duration_seconds = 15.0;
    cmd.transition_type = 1; // EqCrossfade
    int transRes = pulse_audio_execute_transition(cmd);
    assert(transRes == 0);
    std::cout << "Transition command executed successfully." << std::endl;

    // 7. Test Shutdown
    int shutRes = pulse_audio_shutdown();
    assert(shutRes == 0);
    assert(pulse_audio_is_initialized() == 0);
    assert(pulse_audio_is_running() == 0);
    std::cout << "Audio engine shutdown cleanly." << std::endl;

    std::cout << "All C++ Audio Engine tests PASSED." << std::endl;
    return 0;
}
