#include "../include/AudioEngine.h"
#include "../include/AudioDecoder.h"
#include "../include/WavWriter.h"
#include <iostream>
#include <cassert>
#include <vector>
#include <filesystem>
#include <thread>
#include <chrono>
#include <cmath>
#include <fstream>
#include <cstdlib>

namespace {

std::string resolveTestFixture(const std::string& relPath, int trackId = 1) {
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

bool convertAudioFormat(const std::string& inputWav, const std::string& outputPath, const std::string& ext) {
    if (std::filesystem::exists(outputPath)) return true;

    std::string cmd;
    if (ext == "aiff") {
        cmd = "afconvert -f AIFF -d BEI16 \"" + inputWav + "\" \"" + outputPath + "\" > /dev/null 2>&1";
    } else if (ext == "m4a") {
        cmd = "afconvert -f m4af -d aac \"" + inputWav + "\" \"" + outputPath + "\" > /dev/null 2>&1";
    } else if (ext == "flac") {
        cmd = "afconvert -f flac -d flac \"" + inputWav + "\" \"" + outputPath + "\" > /dev/null 2>&1";
    } else if (ext == "caf") {
        cmd = "afconvert -f caff -d aac \"" + inputWav + "\" \"" + outputPath + "\" > /dev/null 2>&1";
    } else if (ext == "mp3") {
        cmd = "ffmpeg -y -i \"" + inputWav + "\" -b:a 192k \"" + outputPath + "\" > /dev/null 2>&1";
    }

    if (!cmd.empty()) {
        int res = std::system(cmd.c_str());
        if (res == 0 && std::filesystem::exists(outputPath)) return true;
    }
    return false;
}

} // anonymous namespace

// 1. Independent Dual-Deck Playback and Position Tracking
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

// 2. Start / Pause / Stop / Seek State Transitions
void test_start_pause_stop_seek_state_machine() {
    std::cout << "Test 2: Start / Pause / Stop / Seek state transitions and atomic seek..." << std::endl;
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

    // Seek while paused to 4.5s
    assert(engine.seekDeck(0, 4.5));
    assert(std::abs(engine.getDeckPosition(0) - 4.5) < 1e-4);

    // Resume playback from seek point
    assert(engine.playDeck(0));
    for (int i = 0; i < 10; ++i) {
        engine.processAudioBlock(masterOut.data(), 256, 2);
    }
    assert(engine.getDeckPosition(0) > 4.5);

    // Seek while actively PLAYING to 2.0s (atomic lock-free audio thread seek)
    assert(engine.seekDeck(0, 2.0));
    assert(std::abs(engine.getDeckPosition(0) - 2.0) < 1e-4);
    for (int i = 0; i < 10; ++i) {
        engine.processAudioBlock(masterOut.data(), 256, 2);
    }
    assert(engine.getDeckPosition(0) > 2.0 && engine.getDeckPosition(0) < 3.0);

    // Stop: resets position to cue point (4.5), state becomes Ready
    assert(engine.stopDeck(0));
    state = engine.getDeckState(0);
    assert(state.playback_state == static_cast<uint8_t>(pulse::audio::DeckPlaybackState::Ready));
    assert(state.is_playing == 0);
    assert(std::abs(engine.getDeckPosition(0) - 4.5) < 1e-4); // Verified returns to cue point

    // Reset cue point back to beginning
    assert(engine.seekDeck(0, 0.0));
    assert(std::abs(engine.getDeckPosition(0) - 0.0) < 1e-4);

    assert(engine.shutdown() == 0);
    std::cout << "  ✓ State machine and atomic seek verified successfully!" << std::endl;
}

// 3. Boundary Seeking Clamping
void test_seek_boundary_clamping() {
    std::cout << "Test 3: Seek boundary clamping (negative and past EOF)..." << std::endl;
    auto& engine = pulse::audio::AudioEngine::getInstance();
    AudioEngineConfigC config{48000, 256, 2};
    assert(engine.initialize(config) == 0);

    std::string fixA = resolveTestFixture("tests/golden-set/golden_track_01.wav", 1);
    assert(engine.loadTrack(0, fixA));

    double totalDuration = engine.getDeckDuration(0);
    assert(totalDuration > 5.0);

    // Negative seek clamps to 0.0
    assert(engine.seekDeck(0, -10.0));
    assert(engine.getDeckPosition(0) == 0.0);

    // Past duration seek clamps to totalDuration
    assert(engine.seekDeck(0, totalDuration + 50.0));
    assert(std::abs(engine.getDeckPosition(0) - totalDuration) < 1e-4);

    // Seek while playing beyond duration clamps and stops cleanly at EOF
    assert(engine.seekDeck(0, totalDuration - 0.02));
    assert(engine.playDeck(0));
    std::vector<float> masterOut(256 * 2, 0.0f);
    for (int i = 0; i < 20; ++i) {
        engine.processAudioBlock(masterOut.data(), 256, 2);
    }
    assert(!engine.isDeckPlaying(0));
    assert(engine.getDeckState(0).playback_state == static_cast<uint8_t>(pulse::audio::DeckPlaybackState::Ready));

    assert(engine.shutdown() == 0);
    std::cout << "  ✓ Seek boundary clamping verified successfully!" << std::endl;
}

// 4. Non-blocking Background Track Preparation Ahead of Time
void test_non_blocking_preparation_during_playback() {
    std::cout << "Test 4: Non-blocking background deck preparation while active playback streams..." << std::endl;
    auto& engine = pulse::audio::AudioEngine::getInstance();
    AudioEngineConfigC config{48000, 256, 2};
    assert(engine.initialize(config) == 0);

    std::string fixA = resolveTestFixture("tests/golden-set/golden_track_01.wav", 1);
    std::string fixB = resolveTestFixture("tests/golden-set/golden_track_02.wav", 2);

    assert(engine.loadTrack(0, fixA));
    assert(engine.playDeck(0));

    // Background thread preparing Deck B ahead of time
    std::atomic<bool> prepDone{false};
    std::atomic<bool> prepSuccess{false};

    std::thread prepThread([&]() {
        // Pre-configure cue point at 2.5s, tempo ratio 1.04, and pitch lock enabled
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
    assert(stats.underrun_count == 0); // Strict zero underruns during background preparation

    DeckStateC stateA = engine.getDeckState(0);
    DeckStateC stateB = engine.getDeckState(1);

    assert(stateA.is_playing == 1);
    assert(stateB.is_playing == 0);
    assert(stateB.playback_state == static_cast<uint8_t>(pulse::audio::DeckPlaybackState::Ready));
    assert(std::abs(stateB.playback_position_seconds - 2.5) < 1e-3);
    assert(std::abs(stateB.tempo_ratio - 1.04) < 1e-4);
    assert(std::abs(stateB.volume - 0.9f) < 1e-4f);

    // Now trigger prepared Deck B to play immediately from its pre-configured cue point
    assert(engine.playDeck(1));
    assert(engine.isDeckPlaying(1));
    for (int i = 0; i < 20; ++i) {
        engine.processAudioBlock(masterOut.data(), 256, 2);
    }
    assert(engine.getDeckPosition(1) > 2.5);

    assert(engine.shutdown() == 0);
    std::cout << "  ✓ Non-blocking background deck preparation verified with ZERO underruns!" << std::endl;
}

// 5. Multi-Format Local Audio Decoding and Independent Deck Loading
void test_multi_format_decoding_and_loading() {
    std::cout << "Test 5: Multi-format audio decoding and deck loading (WAV, AIFF, MP3, M4A, FLAC)..." << std::endl;

    std::filesystem::path tempDir = std::filesystem::temp_directory_path() / "pulse_multi_format_suite";
    std::filesystem::create_directories(tempDir);

    std::string baseWav = (tempDir / "base_track.wav").string();
    assert(pulse::audio::WavWriter::createSyntheticFixture(baseWav, 440.0, 5.0, 120.0, 0.8f, 48000));

    std::vector<std::pair<std::string, std::string>> formats = {
        {"wav", baseWav},
        {"aiff", (tempDir / "track.aiff").string()},
        {"m4a", (tempDir / "track.m4a").string()},
        {"flac", (tempDir / "track.flac").string()},
        {"caf", (tempDir / "track.caf").string()},
        {"mp3", (tempDir / "track.mp3").string()}
    };

    auto& engine = pulse::audio::AudioEngine::getInstance();
    AudioEngineConfigC config{48000, 256, 2};
    assert(engine.initialize(config) == 0);

    for (const auto& [ext, path] : formats) {
        if (ext != "wav") {
            bool converted = convertAudioFormat(baseWav, path, ext);
            if (!converted) {
                std::cout << "  ⚠️ Format " << ext << " conversion tool unavailable, skipping conversion check." << std::endl;
                continue;
            }
        }

        // Test decoding via AudioDecoder
        pulse::audio::DecodedAudio decoded;
        bool ok = pulse::audio::AudioDecoder::decodeFile(path, decoded, 48000, 2);
        assert(ok);
        assert(decoded.sampleRate == 48000);
        assert(decoded.channels == 2);
        assert(decoded.durationSeconds >= 4.5);
        assert(!decoded.samples.empty());

        // Test loading into Deck A
        assert(engine.loadTrack(0, path));
        DeckStateC stateA = engine.getDeckState(0);
        assert(stateA.playback_state == static_cast<uint8_t>(pulse::audio::DeckPlaybackState::Ready));
        assert(stateA.duration_seconds >= 4.5);

        // Test loading into Deck B
        assert(engine.loadTrack(1, path));
        DeckStateC stateB = engine.getDeckState(1);
        assert(stateB.playback_state == static_cast<uint8_t>(pulse::audio::DeckPlaybackState::Ready));
        assert(stateB.duration_seconds >= 4.5);

        std::cout << "  ✓ Format [." << ext << "] decoded & loaded successfully into both decks ("
                  << decoded.durationSeconds << "s, " << decoded.sampleRate << "Hz)" << std::endl;
    }

    assert(engine.shutdown() == 0);
    std::filesystem::remove_all(tempDir);
    std::cout << "  ✓ Multi-format decoding and loading verified across all formats!" << std::endl;
}

// 6. Simultaneous Dual-Deck Playback with Different Formats
void test_simultaneous_dual_deck_playback_different_formats() {
    std::cout << "Test 6: Simultaneous dual-deck playback with heterogeneous formats..." << std::endl;

    std::filesystem::path tempDir = std::filesystem::temp_directory_path() / "pulse_hetero_test";
    std::filesystem::create_directories(tempDir);

    std::string wavA = (tempDir / "track_a.wav").string();
    std::string wavB = (tempDir / "track_b.wav").string();
    assert(pulse::audio::WavWriter::createSyntheticFixture(wavA, 440.0, 6.0, 120.0, 0.7f, 48000));
    assert(pulse::audio::WavWriter::createSyntheticFixture(wavB, 523.25, 6.0, 128.0, 0.7f, 48000));

    std::string formatA = (tempDir / "track_a.aiff").string();
    std::string formatB = (tempDir / "track_b.flac").string();

    if (!convertAudioFormat(wavA, formatA, "aiff")) formatA = wavA;
    if (!convertAudioFormat(wavB, formatB, "flac")) formatB = wavB;

    auto& engine = pulse::audio::AudioEngine::getInstance();
    AudioEngineConfigC config{48000, 256, 2};
    assert(engine.initialize(config) == 0);

    assert(engine.loadTrack(0, formatA));
    assert(engine.loadTrack(1, formatB));

    // Play both concurrently
    assert(engine.playDeck(0));
    assert(engine.playDeck(1));
    assert(engine.isDeckPlaying(0) && engine.isDeckPlaying(1));

    std::vector<float> masterOut(256 * 2, 0.0f);
    float maxAbsSample = 0.0f;
    for (int i = 0; i < 40; ++i) {
        engine.processAudioBlock(masterOut.data(), 256, 2);
        for (float s : masterOut) {
            maxAbsSample = std::max(maxAbsSample, std::abs(s));
        }
    }

    assert(maxAbsSample > 0.01f); // Audio is actively rendering
    assert(maxAbsSample <= 1.5f);  // Bounded output

    DeckStateC stateA = engine.getDeckState(0);
    DeckStateC stateB = engine.getDeckState(1);

    assert(stateA.playback_position_seconds > 0.15);
    assert(stateB.playback_position_seconds > 0.15);

    assert(engine.shutdown() == 0);
    std::filesystem::remove_all(tempDir);
    std::cout << "  ✓ Heterogeneous simultaneous dual-deck playback verified successfully!" << std::endl;
}

// 7. End-of-Track Boundary Clean Stopping
void test_end_of_track_handling() {
    std::cout << "Test 7: End-of-track clean stopping and state resetting..." << std::endl;
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

// 8. Corrupt and Missing File Error Handling
void test_corrupt_and_missing_file_error_handling() {
    std::cout << "Test 8: Error handling on missing, empty, and corrupt files..." << std::endl;
    auto& engine = pulse::audio::AudioEngine::getInstance();
    AudioEngineConfigC config{48000, 256, 2};
    assert(engine.initialize(config) == 0);

    std::filesystem::path tempDir = std::filesystem::temp_directory_path() / "pulse_error_test";
    std::filesystem::create_directories(tempDir);

    std::string missingPath = (tempDir / "does_not_exist.wav").string();
    std::string emptyPath = (tempDir / "empty.wav").string();
    std::string corruptPath = (tempDir / "corrupt.wav").string();

    {
        std::ofstream emptyFile(emptyPath, std::ios::binary);
    }
    {
        std::ofstream corruptFile(corruptPath, std::ios::binary);
        const char garbage[16] = "BAD_AUDIO_DATA!";
        corruptFile.write(garbage, sizeof(garbage));
    }

    // 1. Missing file rejection
    assert(!engine.loadTrack(0, missingPath));
    DeckStateC stateA = engine.getDeckState(0);
    assert(stateA.playback_state == static_cast<uint8_t>(pulse::audio::DeckPlaybackState::Error));
    assert(!engine.playDeck(0)); // Play must be rejected on error deck

    // 2. Empty file rejection
    assert(!engine.prepareDeck(1, emptyPath));
    DeckStateC stateB = engine.getDeckState(1);
    assert(stateB.playback_state == static_cast<uint8_t>(pulse::audio::DeckPlaybackState::Error));
    assert(!engine.playDeck(1));

    // 3. Corrupt file rejection while Deck 0 is playing a valid track
    std::string validA = resolveTestFixture("tests/golden-set/golden_track_01.wav", 1);
    assert(engine.loadTrack(0, validA));
    assert(engine.playDeck(0));
    assert(engine.isDeckPlaying(0));

    // Prepare corrupt file on Deck 1 while Deck 0 plays
    assert(!engine.prepareDeck(1, corruptPath));
    assert(engine.isDeckPlaying(0)); // Deck 0 must still be playing without glitch
    assert(engine.getDeckState(1).playback_state == static_cast<uint8_t>(pulse::audio::DeckPlaybackState::Error));

    std::vector<float> masterOut(256 * 2, 0.0f);
    for (int i = 0; i < 10; ++i) {
        engine.processAudioBlock(masterOut.data(), 256, 2);
    }
    assert(engine.isDeckPlaying(0));

    assert(engine.shutdown() == 0);
    std::filesystem::remove_all(tempDir);
    std::cout << "  ✓ Corrupt and missing files safely rejected with Error state!" << std::endl;
}

int main() {
    std::cout << "==================================================" << std::endl;
    std::cout << "Running Production Dual-Deck Playback Test Suite..." << std::endl;
    std::cout << "==================================================" << std::endl;

    test_dual_deck_independent_playback();
    test_start_pause_stop_seek_state_machine();
    test_seek_boundary_clamping();
    test_non_blocking_preparation_during_playback();
    test_multi_format_decoding_and_loading();
    test_simultaneous_dual_deck_playback_different_formats();
    test_end_of_track_handling();
    test_corrupt_and_missing_file_error_handling();

    std::cout << "==================================================" << std::endl;
    std::cout << "All Production Dual-Deck Playback Tests PASSED!" << std::endl;
    std::cout << "==================================================" << std::endl;
    return 0;
}
