#include "../include/AudioBridgeTypes.h"
#include <iostream>
#include <cassert>

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

    // 5. Test Play/Pause
    int playRes = pulse_audio_play_pause(0, 1);
    assert(playRes == 0);
    pulse_audio_get_deck_state(0, &deckAState);
    assert(deckAState.is_playing == 1);
    std::cout << "Deck A playback state toggled to PLAYING." << std::endl;

    // 6. Test Transition Command Execution
    TransitionCommandC cmd{0, 1, 15.0, 1};
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
