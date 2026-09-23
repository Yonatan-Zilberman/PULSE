#include "../include/AudioEngine.h"
#include "../include/WavWriter.h"
#include <iostream>
#include <new>
#include <atomic>
#include <vector>
#include <filesystem>
#include <string>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <chrono>

namespace {

// ---------------------------------------------------------------------------
// Real-time allocation trap (same pattern as test_realtime_safety, made
// thread-local): counts heap allocations made by THIS (real-time) thread while
// g_trackAllocations is set. Thread-local because the engine's zero-allocation
// contract applies to the render thread specifically; the CoreAudio runtime
// spins its own housekeeping threads after AudioUnit creation, and their
// sporadic mallocs must not be attributed to the engine's RT path (observed
// false positives in multi-second trap windows). Scoped to pure
// processAudioBlock pump windows only — control-plane churn (which decodes
// and allocates) runs with the trap OFF.
// ---------------------------------------------------------------------------
static thread_local bool g_trackAllocations{false};
static thread_local uint64_t g_allocationCount{0};

} // namespace

void* operator new(std::size_t size) {
    if (g_trackAllocations) {
        g_allocationCount += 1;
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
    if (g_trackAllocations) {
        g_allocationCount += 1;
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

namespace {

// Deterministic LCG — fixed seed constant, same sequence every run.
struct Lcg {
    uint32_t s{0xC0FFEE01u};
    float unit() {
        s = s * 1664525u + 1013904223u;
        return float(s >> 8) / float(1u << 24);
    }
};

constexpr double kSecondsPerCell = 5.0;

int runCell(pulse::audio::AudioEngine& engine, uint32_t sampleRate, uint32_t blockSize,
            const std::string& fixA, const std::string& fixB, const std::string& fixC,
            Lcg& rng) {
    using pulse::audio::AudioEngine;

    uint64_t blocks = static_cast<uint64_t>(kSecondsPerCell * static_cast<double>(sampleRate) /
                                            static_cast<double>(blockSize));
    if (blocks == 0) blocks = 1;

    const uint32_t channels = 2;
    std::vector<float> blockBuf(blockSize * channels, 0.0f);
    alignas(32) AudioEventC evBuf[256];

    // Control-plane setup runs with the trap OFF (decode allocates).
    g_trackAllocations = false;
    g_allocationCount = 0;

    // One engine init per sample rate: capture the frame ledger baseline so each
    // block-size cell asserts its exact own contribution.
    const uint64_t framesBefore = engine.getStats().total_frames_processed;
    const uint32_t underrunsBefore = engine.getStats().underrun_count;
    const uint32_t dropsBefore = engine.droppedEvents();

    if (!engine.loadTrack(0, fixA)) { std::fprintf(stderr, "FATAL: loadTrack fixA\n"); std::exit(1); }
    if (!engine.loadTrack(1, fixB)) { std::fprintf(stderr, "FATAL: loadTrack fixB\n"); std::exit(1); }
    engine.playDeck(0);
    engine.playDeck(1);
    engine.setDeckTempoRatio(0, 1.04);
    engine.setDeckTempoRatio(1, 0.96);
    engine.setDeckPitchPreservation(0, true);
    engine.setDeckPitchPreservation(1, true);

    uint64_t rejectedTransitions = 0;
    uint64_t drainedEvents = 0;
    uint64_t nonFiniteSamples = 0;
    double masterPeak = 0.0;
    double sumBlockSec = 0.0;
    std::vector<double> blockTimes;
    blockTimes.reserve(static_cast<size_t>(blocks));
    const auto& clk = std::chrono::steady_clock::now;

    for (uint64_t i = 0; i < blocks; ++i) {
        // --- Real-time pump window: trap ON (this thread only). ---
        g_trackAllocations = true;
        const auto t0 = clk();
        engine.processAudioBlock(blockBuf.data(), blockSize, channels);
        const auto t1 = clk();
        drainedEvents += engine.drainEvents(evBuf, 256, nullptr);
        g_trackAllocations = false;

        const double blockSec = std::chrono::duration<double>(t1 - t0).count();
        sumBlockSec += blockSec;
        blockTimes.push_back(blockSec);

        for (uint32_t s = 0; s < blockSize * channels; ++s) {
            const double v = blockBuf[s];
            if (!std::isfinite(v)) {
                ++nonFiniteSamples;
            } else if (std::abs(v) > masterPeak) {
                masterPeak = std::abs(v);
            }
        }

        // --- Deterministic control-plane churn every 16 blocks (trap OFF). ---
        if ((i % 16) == 15) {
            uint8_t deck = static_cast<uint8_t>(rng.unit() < 0.5f ? 0 : 1);
            switch (static_cast<int>(rng.unit() * 8.0f)) {
                case 0:
                    engine.setDeckVolume(deck, 0.6f + 0.4f * rng.unit());
                    break;
                case 1:
                    engine.setDeckEq(deck, rng.unit() * 2.0f - 1.0f,
                                     rng.unit() * 2.0f - 1.0f,
                                     rng.unit() * 2.0f - 1.0f);
                    break;
                case 2:
                    engine.setDeckFilter(deck, rng.unit() * 2.0f - 1.0f);
                    break;
                case 3:
                    engine.setDeckTempoRatio(deck, 0.90 + 0.20 * rng.unit());
                    break;
                case 4: {
                    double pos = engine.getDeckPosition(deck);
                    engine.seekDeck(deck, 0.5 * pos * rng.unit());
                    break;
                }
                case 5:
                    engine.setPlaying(deck, engine.isDeckPlaying(deck) ? false : true);
                    break;
                case 6: {
                    uint8_t src = static_cast<uint8_t>(rng.unit() < 0.5f ? 0 : 1);
                    uint8_t dst = src == 0 ? 1 : 0;
                    TransitionCommandC cmd{};
                    cmd.version = 1;
                    cmd.source_deck = src;
                    cmd.destination_deck = dst;
                    cmd.duration_seconds = (rng.unit() < 0.5f) ? 1.5 : 2.0;
                    cmd.transition_type = (src == 0)
                        ? static_cast<uint32_t>(pulse::audio::TransitionStrategyType::ClassicEqBlend)
                        : static_cast<uint32_t>(pulse::audio::TransitionStrategyType::BassSwap);
                    cmd.src_tempo_ratio = 1.0f;
                    cmd.dst_tempo_ratio = 1.0f;
                    if (engine.executeTransition(cmd) != 0) {
                        rejectedTransitions++; // rejected-but-healthy; never assert success
                    }
                    break;
                }
                default:
                    // Deck-swap reload of the 60 s fixture into a random deck.
                    engine.loadTrack(deck, (rng.unit() < 0.5f) ? fixA : fixC);
                    engine.playDeck(deck);
                    break;
            }
            // Reset tempos to the cell baseline after any churn step.
            engine.setDeckTempoRatio(0, 1.04);
            engine.setDeckTempoRatio(1, 0.96);
        }
    }

    // --- Cell invariants ---
    AudioEngineStatsC stats = engine.getStats();
    const uint64_t cellFrames = stats.total_frames_processed - framesBefore;
    const double budgetSec = static_cast<double>(blockSize) / sampleRate;
    const double meanBlockSec = sumBlockSec / static_cast<double>(blocks);

    std::sort(blockTimes.begin(), blockTimes.end());
    const size_t p95Idx = static_cast<size_t>(0.95 * (blockTimes.size() - 1));
    const double p95BlockSec = blockTimes[p95Idx];

    const bool framesOk = cellFrames == blocks * blockSize;
    const bool underrunsOk = stats.underrun_count == underrunsBefore && stats.underrun_count == 0;
    const bool dropsOk = engine.droppedEvents() == dropsBefore && engine.droppedEvents() == 0;
    const bool allocsOk = g_allocationCount == 0;
    // Note: compared against the engine's own limiter ceiling constant (0.999f) —
    // the float literal promotes to 0.9990000119, above decimal 0.999.
    const bool peakOk = masterPeak <= 0.999f;
    const bool finiteOk = nonFiniteSamples == 0;
    // ADVISORY per plan F2: the >=10x CPU-headroom bar (mean + p95 <= 10% of block
    // budget) is unachievable in the unoptimized CTest build and is flaky on shared
    // runners even optimized; it is measured and reported below, and its failure is
    // recorded as an engine-optimization candidate. It does NOT gate the exit code —
    // the hard RT-safety invariants do.
    const bool cpuOk = (meanBlockSec <= 0.10 * budgetSec) && (p95BlockSec <= 0.10 * budgetSec);

    const bool pass = framesOk && underrunsOk && dropsOk && allocsOk && peakOk && finiteOk;

    std::printf("Cell sr=%-6u block=%-5u | frames=%llu (expect %llu) underruns=%u drops=%u "
                "allocs=%llu peak=%.6f nonFinite=%llu cpuMean=%.1f%% p95=%.1f%% of budget(%.1fus) "
                "rejectedTrans=%llu -> %s%s\n",
                sampleRate, blockSize,
                (unsigned long long)cellFrames,
                (unsigned long long)(blocks * blockSize),
                stats.underrun_count, (unsigned)engine.droppedEvents(),
                (unsigned long long)g_allocationCount,
                masterPeak, (unsigned long long)nonFiniteSamples,
                100.0 * meanBlockSec / budgetSec, 100.0 * p95BlockSec / budgetSec,
                budgetSec * 1e6, (unsigned long long)rejectedTransitions,
                pass ? "PASS" : "FAIL",
                cpuOk ? "" : "  [advisory: >10% CPU budget - engine optimization candidate]");

    if (!pass) {
        std::fprintf(stderr,
                     "STRESS FAILURE in cell (sr=%u, blockSize=%u):\n"
                     "  total_frames_processed=%llu (expected %llu)  %s\n"
                     "  underrun_count=%u                                   %s\n"
                     "  droppedEvents=%u (drained=%llu)                       %s\n"
                     "  allocations in RT windows=%llu                        %s\n"
                     "  masterPeak=%.9f (bar <= 0.999f)                       %s\n"
                     "  non-finite samples=%llu (bar == 0)                     %s\n"
                     "  cpu mean=%.4f p95=%.4f of budget %fus (advisory 10%%) %s\n",
                     sampleRate, blockSize,
                     (unsigned long long)cellFrames,
                     (unsigned long long)(blocks * blockSize), framesOk ? "" : "FAIL",
                     stats.underrun_count, underrunsOk ? "" : "FAIL",
                     engine.droppedEvents(), (unsigned long long)drainedEvents, dropsOk ? "" : "FAIL",
                     (unsigned long long)g_allocationCount,
                     allocsOk ? "" : "FAIL", masterPeak, peakOk ? "" : "FAIL",
                     (unsigned long long)nonFiniteSamples, finiteOk ? "" : "FAIL",
                     meanBlockSec, p95BlockSec, budgetSec, cpuOk ? "" : "ADVISORY-ONLY");
        return 1;
    }
    return 0;
}

} // namespace

int main() {
    using pulse::audio::AudioEngine;
    using pulse::audio::WavWriter;

    Lcg rng;

    // 60 s synthetic fixtures in a temp dir (deterministic content).
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "pulse_stress_test";
    std::filesystem::create_directories(dir);
    const std::string fixA = (dir / "fixA.wav").string();
    const std::string fixB = (dir / "fixB.wav").string();
    const std::string fixC = (dir / "fixC.wav").string();
    if (!WavWriter::createSyntheticFixture(fixA, 440.0, 60.0, 120.0, 0.7f, 48000) ||
        !WavWriter::createBassHeavyFixture(fixB, 60.0, 660.0, 60.0, 128.0, 0.7f, 48000) ||
        !WavWriter::createSyntheticFixture(fixC, 523.25, 60.0, 124.0, 0.7f, 48000)) {
        std::fprintf(stderr, "FATAL: stress fixture generation failed\n");
        return 1;
    }

    auto& engine = AudioEngine::getInstance();
    const uint32_t sampleRates[] = {44100, 48000, 96000};
    const uint32_t blockSizes[] = {64, 256, 512, 2048};

    for (uint32_t sr : sampleRates) {
        // Single engine init per sample rate; initialize() never starts the
        // AudioUnit and resets the frame ledger, so each cell starts clean.
        AudioEngineConfigC cfg{sr, 2048, 2};
        if (engine.initialize(cfg) != 0) {
            std::fprintf(stderr, "FATAL: engine initialize failed at sr=%u\n", sr);
            std::filesystem::remove_all(dir);
            return 1;
        }
        for (uint32_t blockSize : blockSizes) {
            if (runCell(engine, sr, blockSize, fixA, fixB, fixC, rng) != 0) {
                engine.shutdown();
                std::filesystem::remove_all(dir);
                return 1;
            }
        }
        engine.shutdown();
    }

    // Final sanity: fresh re-init at 48k/512, one warm block, exact ledger.
    if (engine.initialize(AudioEngineConfigC{48000, 512, 2}) != 0) {
        std::fprintf(stderr, "FATAL: final sanity initialize failed\n");
        std::filesystem::remove_all(dir);
        return 1;
    }
    std::vector<float> warm(512 * 2, 0.0f);
    engine.processAudioBlock(warm.data(), 512, 2);
    if (engine.getStats().total_frames_processed != 512) {
        std::fprintf(stderr, "FATAL: final sanity frame ledger incorrect\n");
        engine.shutdown();
        std::filesystem::remove_all(dir);
        return 1;
    }
    engine.shutdown();
    std::filesystem::remove_all(dir);

    std::printf("Stress DSP matrix: ALL 12 CELLS PASS\n");
    return 0;
}
