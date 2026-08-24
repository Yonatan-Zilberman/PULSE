#include "../include/AudioEngine.h"
#include "../include/WavWriter.h"
#include <iostream>
#include <cassert>
#include <vector>
#include <filesystem>

static std::string resolveTestFixture(const std::string& relPath) {
    if (std::filesystem::exists(relPath)) return relPath;
    std::string p2 = "../../" + relPath;
    if (std::filesystem::exists(p2)) return p2;
    std::string p3 = "../" + relPath;
    if (std::filesystem::exists(p3)) return p3;

    std::filesystem::path tempDir = std::filesystem::temp_directory_path() / "pulse_lifecycle_test";
    std::filesystem::create_directories(tempDir);
    std::string synthPath = (tempDir / "synth_track_01.wav").string();
    pulse::audio::WavWriter::createBassHeavyFixture(synthPath, 60.0, 440.0, 10.0, 120.0, 0.8f, 48000);
    return synthPath;
}

void test_initial_state() {
    auto& engine = pulse::audio::AudioEngine::getInstance();
    engine.shutdown();
    assert(!engine.isInitialized());
    assert(!engine.isRunning());
    std::cout << "  ✓ Initial state verified (uninitialized, stopped)" << std::endl;
}

void test_invalid_configurations() {
    auto& engine = pulse::audio::AudioEngine::getInstance();

    // Invalid sample rates
    assert(engine.initialize({0, 512, 2}) != 0);
    assert(engine.initialize({1000, 512, 2}) != 0);
    assert(engine.initialize({500000, 512, 2}) != 0);

    // Invalid buffer sizes
    assert(engine.initialize({48000, 0, 2}) != 0);
    assert(engine.initialize({48000, 8, 2}) != 0);
    assert(engine.initialize({48000, 16384, 2}) != 0);

    // Invalid channel counts
    assert(engine.initialize({48000, 512, 0}) != 0);
    assert(engine.initialize({48000, 512, 16}) != 0);

    assert(!engine.isInitialized());
    std::cout << "  ✓ Invalid configurations rejected cleanly" << std::endl;
}

void test_start_stop_lifecycle() {
    auto& engine = pulse::audio::AudioEngine::getInstance();
    AudioEngineConfigC config{48000, 256, 2};

    assert(engine.initialize(config) == 0);
    assert(engine.isInitialized());
    assert(!engine.isRunning());

    // Start streaming
    assert(engine.start() == 0);
    assert(engine.isRunning());

    // Duplicate start is idempotent
    assert(engine.start() == 0);
    assert(engine.isRunning());

    // Stop streaming
    assert(engine.stop() == 0);
    assert(!engine.isRunning());
    assert(engine.isInitialized());

    // Duplicate stop is idempotent
    assert(engine.stop() == 0);
    assert(!engine.isRunning());

    // Shutdown
    assert(engine.shutdown() == 0);
    assert(!engine.isInitialized());
    assert(!engine.isRunning());

    // Cannot start when uninitialized
    assert(engine.start() != 0);
    std::cout << "  ✓ Start/Stop/Shutdown lifecycle transitions verified" << std::endl;
}

void test_reinitialization() {
    auto& engine = pulse::audio::AudioEngine::getInstance();

    AudioEngineConfigC cfg48k{48000, 512, 2};
    assert(engine.initialize(cfg48k) == 0);
    assert(engine.start() == 0);
    assert(engine.isRunning());

    // Re-initialize while running (should cleanly stop, re-allocate, and configure)
    AudioEngineConfigC cfg44k{44100, 128, 2};
    assert(engine.initialize(cfg44k) == 0);
    assert(engine.isInitialized());
    assert(!engine.isRunning());
    assert(engine.getConfig().sample_rate == 44100);
    assert(engine.getConfig().buffer_size == 128);

    assert(engine.start() == 0);
    assert(engine.isRunning());

    assert(engine.shutdown() == 0);
    std::cout << "  ✓ Re-initialization under load verified" << std::endl;
}

void test_active_playback_teardown() {
    auto& engine = pulse::audio::AudioEngine::getInstance();
    AudioEngineConfigC config{48000, 512, 2};

    assert(engine.initialize(config) == 0);
    std::string fixture = resolveTestFixture("tests/golden-set/golden_track_01.wav");
    assert(engine.loadTrack(0, fixture));
    assert(engine.setPlaying(0, true));

    assert(engine.start() == 0);

    // Pump a few blocks
    std::vector<float> block(512 * 2, 0.0f);
    for (int i = 0; i < 10; ++i) {
        engine.processAudioBlock(block.data(), 512, 2);
    }

    // Direct shutdown during active playback
    assert(engine.shutdown() == 0);
    assert(!engine.isInitialized());
    assert(!engine.isRunning());
    std::cout << "  ✓ Teardown during active playback verified with zero deadlocks" << std::endl;
}


int main() {
    std::cout << "==================================================" << std::endl;
    std::cout << "Running AudioEngine Lifecycle Unit Tests..." << std::endl;
    std::cout << "==================================================" << std::endl;

    test_initial_state();
    test_invalid_configurations();
    test_start_stop_lifecycle();
    test_reinitialization();
    test_active_playback_teardown();

    std::cout << "==================================================" << std::endl;
    std::cout << "All AudioEngine Lifecycle Tests PASSED!" << std::endl;
    std::cout << "==================================================" << std::endl;
    return 0;
}
