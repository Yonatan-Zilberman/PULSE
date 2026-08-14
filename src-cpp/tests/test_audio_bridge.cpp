#include "../include/AudioBridgeTypes.h"
#include <iostream>
#include <cassert>

int main() {
    std::cout << "Running PULSE C++ Audio Engine Smoke Test..." << std::endl;

    // 1. Test ABI Sizes
    std::cout << "Testing struct alignments and sizes..." << std::endl;
    assert(sizeof(AudioEngineConfigC) == 12);
    assert(sizeof(DeckStateC) >= 48);

    // 2. Test Audio Engine Initialization
    AudioEngineConfigC config{48000, 512, 2};
    int initRes = pulse_audio_init(config);
    assert(initRes == 0);
    std::cout << "Audio engine initialized successfully." << std::endl;

    // 3. Test Deck State Retrieval
    DeckStateC deckAState{};
    int stateRes = pulse_audio_get_deck_state(0, &deckAState);
    assert(stateRes == 0);
    assert(deckAState.deck_id == 0);
    assert(deckAState.is_playing == 0);
    std::cout << "Deck A state verified: ID=" << static_cast<int>(deckAState.deck_id)
              << ", is_playing=" << static_cast<int>(deckAState.is_playing) << std::endl;

    // 4. Test Play/Pause
    int playRes = pulse_audio_play_pause(0, 1);
    assert(playRes == 0);
    pulse_audio_get_deck_state(0, &deckAState);
    assert(deckAState.is_playing == 1);
    std::cout << "Deck A playback state toggled to PLAYING." << std::endl;

    // 5. Test Transition Command Execution
    TransitionCommandC cmd{0, 1, 15.0, 1};
    int transRes = pulse_audio_execute_transition(cmd);
    assert(transRes == 0);
    std::cout << "Transition command executed successfully." << std::endl;

    // 6. Test Shutdown
    int shutRes = pulse_audio_shutdown();
    assert(shutRes == 0);
    std::cout << "Audio engine shutdown cleanly." << std::endl;

    std::cout << "All C++ Audio Engine tests PASSED." << std::endl;
    return 0;
}
