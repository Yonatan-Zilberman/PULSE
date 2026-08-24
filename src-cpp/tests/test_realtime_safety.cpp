#include "../include/AudioEngine.h"
#include "../include/WavWriter.h"
#include <iostream>
#include <cassert>
#include <cstdlib>
#include <new>
#include <atomic>
#include <vector>
#include <filesystem>

static std::string resolveTestFixture(const std::string& relPath, int trackId = 1) {
    if (std::filesystem::exists(relPath)) return relPath;
    std::string p2 = "../../" + relPath;
    if (std::filesystem::exists(p2)) return p2;
    std::string p3 = "../" + relPath;
    if (std::filesystem::exists(p3)) return p3;

    std::filesystem::path tempDir = std::filesystem::temp_directory_path() / "pulse_rtsafe_test";
    std::filesystem::create_directories(tempDir);
    std::string synthPath = (tempDir / ("synth_track_" + std::to_string(trackId) + ".wav")).string();
    pulse::audio::WavWriter::createBassHeavyFixture(synthPath, 60.0, 440.0 + trackId * 50.0, 10.0, 120.0, 0.8f, 48000);
    return synthPath;
}

static std::atomic<bool> g_trackAllocations{false};
static std::atomic<uint64_t> g_allocationCount{0};

void* operator new(std::size_t size) {
    if (g_trackAllocations.load(std::memory_order_relaxed)) {
        g_allocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    void* ptr = std::malloc(size);
    if (!ptr) throw std::bad_alloc();
    return ptr;
}

void operator delete(void* ptr) noexcept {
    std::free(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept {
    std::free(ptr);
}

void* operator new[](std::size_t size) {
    if (g_trackAllocations.load(std::memory_order_relaxed)) {
        g_allocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    void* ptr = std::malloc(size);
    if (!ptr) throw std::bad_alloc();
    return ptr;
}

void operator delete[](void* ptr) noexcept {
    std::free(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept {
    std::free(ptr);
}

int main() {
    std::cout << "==================================================" << std::endl;
    std::cout << "Running Real-Time Safety & Zero-Allocation Test..." << std::endl;
    std::cout << "==================================================" << std::endl;

    auto& engine = pulse::audio::AudioEngine::getInstance();
    AudioEngineConfigC config{48000, 256, 2};

    assert(engine.initialize(config) == 0);
    std::string fix1 = resolveTestFixture("tests/golden-set/golden_track_01.wav", 1);
    std::string fix2 = resolveTestFixture("tests/golden-set/golden_track_02.wav", 2);
    assert(engine.loadTrack(0, fix1));
    assert(engine.loadTrack(1, fix2));

    assert(engine.setPlaying(0, true));
    assert(engine.setPlaying(1, true));


    // Start a bass swap transition
    TransitionCommandC cmd{0, 1, 4.0, 2};
    assert(engine.executeTransition(cmd) == 0);

    assert(engine.start() == 0);

    // Warm up one block to prime any static buffers
    std::vector<float> masterOut(256 * 2, 0.0f);
    engine.processAudioBlock(masterOut.data(), 256, 2);

    // Scratch buffers for JUCE callback simulation
    std::vector<float> chanL(256, 0.0f);
    std::vector<float> chanR(256, 0.0f);
    float* outputChannels[2] = { chanL.data(), chanR.data() };
    juce::AudioIODeviceCallbackContext ctx{};

    std::cout << "Enabling allocation trap across 1,000 real-time audio blocks..." << std::endl;
    g_allocationCount.store(0, std::memory_order_relaxed);
    g_trackAllocations.store(true, std::memory_order_seq_cst);

    // 1. Process 500 blocks via processAudioBlock
    for (int i = 0; i < 500; ++i) {
        engine.processAudioBlock(masterOut.data(), 256, 2);
    }

    // 2. Process 500 blocks via audioDeviceIOCallbackWithContext
    for (int i = 0; i < 500; ++i) {
        engine.audioDeviceIOCallbackWithContext(nullptr, 0, outputChannels, 2, 256, ctx);
    }

    g_trackAllocations.store(false, std::memory_order_seq_cst);
    uint64_t totalAllocations = g_allocationCount.load(std::memory_order_relaxed);

    std::cout << "Real-time callback allocations detected: " << totalAllocations << std::endl;
    assert(totalAllocations == 0);

    assert(engine.shutdown() == 0);

    std::cout << "==================================================" << std::endl;
    std::cout << "Real-Time Safety Test PASSED: STRICT ZERO-ALLOCATION GUARANTEE VERIFIED!" << std::endl;
    std::cout << "==================================================" << std::endl;
    return 0;
}
