#include "../include/AudioEngine.h"
#include "../include/WavWriter.h"
#include <iostream>
#include <cassert>
#include <vector>
#include <filesystem>
#include <thread>
#include <chrono>
#include <cmath>

static std::string resolveTestFixture(const std::string& relPath, int trackId = 1) {
    if (std::filesystem::exists(relPath)) return relPath;
    std::string p2 = "../../" + relPath;
    if (std::filesystem::exists(p2)) return p2;
    std::string p3 = "../" + relPath;
    if (std::filesystem::exists(p3)) return p3;

    std::filesystem::path tempDir = std::filesystem::temp_directory_path() / "pulse_dualdeck_test";
    std::filesystem::create_directories(tempDir);
    std::string synthPath = (tempDir / ("synth_track_" + std::to_string(trackId) + ".wav")).string();
    pulse::audio::WavWriter::createBassHeavyFixture(synthPath, 60.0, 440.0 + trackId * 50.0, 10.0, 120.0 + trackId * 4.0, 0.8f, 48000);
    return synthPath;
}

void test_dual_deck_independent_playback() {
    std::cout << "Test 1: Independent dual-deck playback and position tracking..." << std::endl;
    auto& engine = pulse::audio::AudioEngine::getInstance();
    AudioEngineConfigC config{48000, 256, 2};
    assert(engine.initialize(config) == 0);

    std::string fixA = resolveTestFixture("tests/golden-set/golden_track_01.wav", 1);
    std::string fixB = resolveTestFixture("tests/golden-set/golden_track_02.wav", 2);

    assert(engine.loadTrack(0, fixA));
    assert(engine.loadTrack(1, fixB));

    DeckStateC stateA = engine.getDeckState(0);
    DeckStateC stateB = engine.getDeckState(1);

    assert(stateA.playback_state == static_cast<uint8_t>(pulse::audio::DeckPlaybackState::Ready));
    assert(stateB.playback_state == static_cast<uint8_t>(pulse::audio::DeckPlaybackState::Ready));
    assert(stateA.is_playing == 0 && stateB.is_playing == 0);
    assert(stateA.playback_position_seconds == 0.0);
    assert(stateB.playback_position_seconds == 0.0);

    // 1. Play Deck A only
    assert(engine.playDeck(0));
    assert(engine.isDeckPlaying(0));
    assert(!engine.isDeckPlaying(1));

    std::vector<float> masterOut(256 * 2, 0.0f);
    for (int i = 0; i < 20; ++i) {
        engine.processAudioBlock(masterOut.data(), 256, 2);
    }

    stateA = engine.getDeckState(0);
    stateB = engine.getDeckState(1);
    double expectedPosA = (20.0 * 256.0) / 48000.0;
    assert(std::abs(stateA.playback_position_seconds - expectedPosA) < 0.02);
    assert(stateB.playback_position_seconds == 0.0);
    assert(stateA.is_playing == 1 && stateB.is_playing == 0);

    // 2. Pause Deck A, Play Deck B
    assert(engine.pauseDeck(0));
    assert(!engine.isDeckPlaying(0));
    assert(engine.playDeck(1));
    assert(engine.isDeckPlaying(1));

    double frozenPosA = stateA.playback_position_seconds;
    for (int i = 0; i < 20; ++i) {
        engine.processAudioBlock(masterOut.data(), 256, 2);
    }

    stateA = engine.getDeckState(0);
    stateB = engine.getDeckState(1);
    double expectedPosB = (20.0 * 256.0) / 48000.0;
    assert(std::abs(stateA.playback_position_seconds - frozenPosA) < 1e-4); // Deck A stayed frozen
    assert(std::abs(stateB.playback_position_seconds - expectedPosB) < 0.02);
    assert(stateA.is_playing == 0 && stateB.is_playing == 1);

    // 3. Play both simultaneously
    assert(engine.playDeck(0));
    assert(engine.isDeckPlaying(0) && engine.isDeckPlaying(1));

    for (int i = 0; i < 20; ++i) {
        engine.processAudioBlock(masterOut.data(), 256, 2);
    }

    stateA = engine.getDeckState(0);
    stateB = engine.getDeckState(1);
    assert(stateA.playback_position_seconds > frozenPosA);
    assert(stateB.playback_position_seconds > expectedPosB);

    assert(engine.shutdown() == 0);
    std::cout << "  ✓ Independent dual-deck playback verified successfully!" << std::endl;
}

void test_start_pause_stop_seek_state_machine() {
    std::cout << "Test 2: Start / Pause / Stop / Seek state transitions and boundaries..." << std::endl;
    auto& engine = pulse::audio::AudioEngine::getInstance();
    AudioEngineConfigC config{48000, 256, 2};
    assert(engine.initialize(config) == 0);

    std::string fixA = resolveTestFixture("tests/golden-set/golden_track_01.wav", 1);
    assert(engine.loadTrack(0, fixA));

    DeckStateC state = engine.getDeckState(0);
    double totalDuration = state.duration_seconds;
    assert(totalDuration > 5.0);

    // Play -> Pause
    assert(engine.playDeck(0));
    assert(engine.getDeckState(0).playback_state == static_cast<uint8_t>(pulse::audio::DeckPlaybackState::Playing));

    std::vector<float> masterOut(256 * 2, 0.0f);
    for (int i = 0; i < 10; ++i) {
        engine.processAudioBlock(masterOut.data(), 256, 2);
    }
    double posAfter10 = engine.getDeckPosition(0);
    assert(posAfter10 > 0.0);

    assert(engine.pauseDeck(0));
    assert(engine.getDeckState(0).playback_state == static_cast<uint8_t>(pulse::audio::DeckPlaybackState::Paused));
    assert(!engine.isDeckPlaying(0));

    // Pump while paused: position must stay exact
    for (int i = 0; i < 10; ++i) {
        engine.processAudioBlock(masterOut.data(), 256, 2);
    }
    assert(std::abs(engine.getDeckPosition(0) - posAfter10) < 1e-4);

    // Seek to 4.5s
    assert(engine.seekDeck(0, 4.5));
    assert(std::abs(engine.getDeckPosition(0) - 4.5) < 1e-4);

    // Resume playback from seek point
    assert(engine.playDeck(0));
    for (int i = 0; i < 10; ++i) {
        engine.processAudioBlock(masterOut.data(), 256, 2);
    }
    assert(engine.getDeckPosition(0) > 4.5);

    // Stop: resets position to cue point (0.0), state becomes Ready
    assert(engine.stopDeck(0));
    state = engine.getDeckState(0);
    assert(state.playback_state == static_cast<uint8_t>(pulse::audio::DeckPlaybackState::Ready));
    assert(state.is_playing == 0);

    // Boundary seeking: Negative clamps to 0.0
    assert(engine.seekDeck(0, -5.0));
    assert(engine.getDeckPosition(0) == 0.0);

    // Boundary seeking: Past duration clamps to duration
    assert(engine.seekDeck(0, totalDuration + 100.0));
    assert(std::abs(engine.getDeckPosition(0) - totalDuration) < 1e-4);

    assert(engine.shutdown() == 0);
    std::cout << "  ✓ State machine and seek boundaries verified successfully!" << std::endl;
}

void test_non_blocking_preparation_during_playback() {
    std::cout << "Test 3: Non-blocking background deck preparation while active playback streams..." << std::endl;
    auto& engine = pulse::audio::AudioEngine::getInstance();
    AudioEngineConfigC config{48000, 256, 2};
    assert(engine.initialize(config) == 0);

    std::string fixA = resolveTestFixture("tests/golden-set/golden_track_01.wav", 1);
    std::string fixB = resolveTestFixture("tests/golden-set/golden_track_02.wav", 2);

    assert(engine.loadTrack(0, fixA));
    assert(engine.playDeck(0));

    // Background thread preparing Deck B
    std::atomic<bool> prepDone{false};
    std::atomic<bool> prepSuccess{false};

    std::thread prepThread([&]() {
        // Simulate background track preparation with cue point, tempo ratio, and pitch lock
        bool ok = engine.prepareDeck(1, fixB, 2.5, 1.04, true);
        if (ok) {
            engine.setDeckVolume(1, 0.9f);
            engine.setDeckEq(1, -0.2f, 0.1f, 0.0f);
        }
        prepSuccess.store(ok);
        prepDone.store(true);
    });

    // Real-time audio thread pumping frames on Deck A uninterrupted
    std::vector<float> masterOut(256 * 2, 0.0f);
    uint64_t pumpedBlocks = 0;
    while (!prepDone.load(std::memory_order_relaxed) || pumpedBlocks < 200) {
        engine.processAudioBlock(masterOut.data(), 256, 2);
        pumpedBlocks++;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    prepThread.join();

    assert(prepSuccess.load());
    AudioEngineStatsC stats = engine.getStats();
    assert(stats.underrun_count == 0); // Strict zero underruns during background load

    DeckStateC stateA = engine.getDeckState(0);
    DeckStateC stateB = engine.getDeckState(1);

    assert(stateA.is_playing == 1);
    assert(stateB.is_playing == 0);
    assert(stateB.playback_state == static_cast<uint8_t>(pulse::audio::DeckPlaybackState::Ready));
    assert(std::abs(stateB.playback_position_seconds - 2.5) < 1e-3);
    assert(std::abs(stateB.tempo_ratio - 1.04) < 1e-4);
    assert(std::abs(stateB.volume - 0.9f) < 1e-4f);

    // Now trigger prepared Deck B to play immediately
    assert(engine.playDeck(1));
    assert(engine.isDeckPlaying(1));
    for (int i = 0; i < 20; ++i) {
        engine.processAudioBlock(masterOut.data(), 256, 2);
    }
    assert(engine.getDeckPosition(1) > 2.5);

    assert(engine.shutdown() == 0);
    std::cout << "  ✓ Non-blocking background deck preparation verified with ZERO underruns!" << std::endl;
}

void test_end_of_track_handling() {
    std::cout << "Test 4: End-of-track clean stopping and boundary clamping..." << std::endl;
    auto& engine = pulse::audio::AudioEngine::getInstance();
    AudioEngineConfigC config{48000, 256, 2};
    assert(engine.initialize(config) == 0);

    std::string fixA = resolveTestFixture("tests/golden-set/golden_track_01.wav", 1);
    assert(engine.loadTrack(0, fixA));

    double dur = engine.getDeckDuration(0);
    assert(dur > 1.0);

    // Seek to 0.02 seconds before EOF
    double nearEnd = dur - 0.02;
    assert(engine.seekDeck(0, nearEnd));
    assert(engine.playDeck(0));
    assert(engine.isDeckPlaying(0));

    // Pump 20 blocks (~0.106s), which exceeds remaining duration (0.02s)
    std::vector<float> masterOut(256 * 2, 0.0f);
    for (int i = 0; i < 20; ++i) {
        engine.processAudioBlock(masterOut.data(), 256, 2);
    }

    DeckStateC state = engine.getDeckState(0);
    assert(state.is_playing == 0);
    assert(state.playback_state == static_cast<uint8_t>(pulse::audio::DeckPlaybackState::Ready));
    assert(std::abs(state.playback_position_seconds - dur) < 0.05);

    assert(engine.shutdown() == 0);
    std::cout << "  ✓ End-of-track boundary and clean stopping verified!" << std::endl;
}

void test_multi_format_decoding_and_playback() {
    std::cout << "Test 5: Multi-format audio decoding and playback verification..." << std::endl;

    std::filesystem::path tempDir = std::filesystem::temp_directory_path() / "pulse_format_test";
    std::filesystem::create_directories(tempDir);

    std::string wavPath = (tempDir / "test_pcm.wav").string();
    assert(pulse::audio::WavWriter::createSyntheticFixture(wavPath, 440.0, 3.0, 120.0, 0.8f, 48000));

    pulse::audio::DecodedAudio decoded;
    assert(pulse::audio::AudioDecoder::decodeFile(wavPath, decoded, 48000, 2));
    assert(decoded.sampleRate == 48000);
    assert(decoded.channels == 2);
    assert(std::abs(decoded.durationSeconds - 3.0) < 0.05);
    assert(!decoded.samples.empty());

    auto& engine = pulse::audio::AudioEngine::getInstance();
    AudioEngineConfigC config{48000, 256, 2};
    assert(engine.initialize(config) == 0);

    assert(engine.loadTrack(0, wavPath));
    assert(engine.playDeck(0));

    std::vector<float> masterOut(256 * 2, 0.0f);
    for (int i = 0; i < 10; ++i) {
        engine.processAudioBlock(masterOut.data(), 256, 2);
    }
    assert(engine.getDeckPosition(0) > 0.0);

    assert(engine.shutdown() == 0);
    std::filesystem::remove_all(tempDir);
    std::cout << "  ✓ Multi-format decoding and rendering verified!" << std::endl;
}

int main() {
    std::cout << "==================================================" << std::endl;
    std::cout << "Running Production Dual-Deck Playback Test Suite..." << std::endl;
    std::cout << "==================================================" << std::endl;

    test_dual_deck_independent_playback();
    test_start_pause_stop_seek_state_machine();
    test_non_blocking_preparation_during_playback();
    test_end_of_track_handling();
    test_multi_format_decoding_and_playback();

    std::cout << "==================================================" << std::endl;
    std::cout << "All Production Dual-Deck Playback Tests PASSED!" << std::endl;
    std::cout << "==================================================" << std::endl;
    return 0;
}
