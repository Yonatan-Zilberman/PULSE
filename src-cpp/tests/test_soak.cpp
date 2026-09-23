#include "../include/AudioEngine.h"
#include "../include/DeckPlayer.h"
#include "../include/Mixer.h"
#include "../include/WavWriter.h"
#include <iostream>
#include <fstream>
#include <new>
#include <atomic>
#include <vector>
#include <filesystem>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <cmath>

namespace {

// ---------------------------------------------------------------------------
// Real-time allocation trap (same pattern as test_realtime_safety, made
// thread-local): counts heap allocations made by THIS thread while
// g_trackAllocations is set. Thread-local because the engine's zero-allocation
// contract applies to the render thread specifically; the CoreAudio runtime's
// own housekeeping threads must not be attributed to the engine's RT path
// (would false-positive in long soak runs). Scoped to pure processAudioBlock
// pump windows only.
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

constexpr uint32_t kSampleRate = 48000;
constexpr uint32_t kBlockFrames = 512;
constexpr uint32_t kChannels = 2;
constexpr double kDt = static_cast<double>(kBlockFrames) / kSampleRate; // per-block audio time

struct SoakConfig {
    double durationSec{300.0};
    std::string reportPath{"soak_report.json"};
    uint32_t tracks{25};
};

void printUsage(const char* prog) {
    std::printf("Pulse Continuous Soak Harness (offline, deterministic ping-pong)\n"
                "Usage: %s [options]\n"
                "  --duration <sec>   Simulated audio seconds to render (default 300; PRD 12h = 43200)\n"
                "  --report <path>    JSON report path (default ./soak_report.json)\n"
                "  --tracks <n>       Queue length (default 25)\n", prog);
}

struct PumpState {
    pulse::audio::AudioEngine& engine;
    std::vector<float> blockBuf;
    alignas(32) AudioEventC evBuf[256];
    uint64_t drainedEvents{0};
    uint64_t nonFiniteSamples{0};
    double masterPeak{0.0};
    double deckPosWatermark[2]{0.0, 0.0};

    explicit PumpState(pulse::audio::AudioEngine& e)
        : engine(e), blockBuf(kBlockFrames * kChannels, 0.0f) {}

    // Renders one block: trap around processAudioBlock + drain, peak tracking.
    void renderBlock() {
        g_trackAllocations = true;
        engine.processAudioBlock(blockBuf.data(), kBlockFrames, kChannels);
        drainedEvents += engine.drainEvents(evBuf, 256, nullptr);
        g_trackAllocations = false;

        for (uint32_t s = 0; s < blockBuf.size(); ++s) {
            const float v = blockBuf[s];
            if (!std::isfinite(v)) {
                ++nonFiniteSamples;
            } else if (std::abs(v) > masterPeak) {
                masterPeak = std::abs(v);
            }
        }
        // No-stuck-deck watermark (control-plane atomic read, outside the trap).
        for (uint8_t d = 0; d < 2; ++d) {
            double pos = engine.getDeckPosition(d);
            if (pos > deckPosWatermark[d]) deckPosWatermark[d] = pos;
        }
    }
};

// Renders until simTime >= targetSec (offline: faster than real time; no sleeping).
void renderUntil(PumpState& ps, double& simTime, double targetSec,
                 std::vector<double>& blockTimes) {
    const auto clk = std::chrono::steady_clock::now;
    while (simTime < targetSec - 1e-9) {
        const auto t0 = clk();
        ps.renderBlock();
        const auto t1 = clk();
        blockTimes.push_back(std::chrono::duration<double>(t1 - t0).count());
        simTime += kDt;
    }
}

int runSoak(const SoakConfig& cfg) {
    using pulse::audio::AudioEngine;
    using pulse::audio::DeckPlayer;
    using pulse::audio::Mixer;
    using pulse::audio::WavWriter;

    if (cfg.tracks < 2) {
        std::fprintf(stderr, "ERROR: --tracks must be >= 2\n");
        return 2;
    }

    g_trackAllocations = false;
    g_allocationCount = 0;

    // --- Engine setup ---
    auto& engine = AudioEngine::getInstance();
    AudioEngineConfigC cfgEngine{kSampleRate, kBlockFrames, kChannels};
    if (engine.initialize(cfgEngine) != 0) {
        std::fprintf(stderr, "FATAL: engine initialize failed\n");
        return 1;
    }
    auto* mixer = engine.getMixer();
    auto* deckA = engine.getDeck(0);
    auto* deckB = engine.getDeck(1);
    mixer->setMasterVolume(1.0f);
    mixer->setCrossfader(-1.0f); // 100% deck A initially

    // --- Fixtures: trackLen scaled so a full queue pass fits the duration. ---
    const double trackLen = std::min(30.0, std::max(8.0, cfg.durationSec / static_cast<double>(cfg.tracks)));
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "pulse_soak_test";
    std::filesystem::create_directories(dir);

    std::vector<std::string> trackPaths;
    for (uint32_t i = 0; i < cfg.tracks; ++i) {
        std::string name = "soak_track_" + std::to_string(i) + ".wav";
        const std::string path = (dir / name).string();
        bool ok = (i % 2 == 0)
            ? WavWriter::createSyntheticFixture(path, 440.0 + 35.0 * i, trackLen * 1.5, 120.0 + 2.0 * i, 0.7f, kSampleRate)
            : WavWriter::createBassHeavyFixture(path, 60.0, 440.0 + 35.0 * i, trackLen * 1.5, 120.0 + 2.0 * i, 0.7f, kSampleRate);
        if (!ok) {
            std::fprintf(stderr, "FATAL: fixture generation failed for track %u\n", i);
            return 1;
        }
        trackPaths.push_back(path);
    }

    // --- Load first two tracks. ---
    if (!engine.loadTrack(0, trackPaths[0]) || !engine.loadTrack(1, trackPaths[1])) {
        std::fprintf(stderr, "FATAL: initial loadTrack failed\n");
        return 1;
    }
    deckA->setPlaybackPosition(0.0);
    deckA->setPlaying(true);
    deckB->setPlaying(false);

    std::vector<uint32_t> usedCount(cfg.tracks, 0);
    usedCount[0] = 1;
    usedCount[1] = 1;

    PumpState ps(engine);
    std::vector<double> blockTimes;
    blockTimes.reserve(static_cast<size_t>(cfg.durationSec / kDt) + 64);

    double simTime = 0.0;
    uint32_t nextIndex = 2;               // queue cursor (wraps)
    uint32_t transitionCount = 0;
    double currentTrackMasterStart = 0.0; // master time the incoming track started

    // --- Continuous ping-pong loop (structure per pulse_cli.cpp:596-800) ---
    while (simTime < cfg.durationSec - 1e-9) {
        const uint8_t srcDeckId = static_cast<uint8_t>(transitionCount % 2);
        const uint8_t dstDeckId = srcDeckId == 0 ? 1 : 0;
        auto* srcDeck = (srcDeckId == 0) ? deckA : deckB;
        auto* dstDeck = (srcDeckId == 0) ? deckB : deckA;

        // Bounded tempo match for the pair (control plane; trap OFF).
        engine.matchTempo(srcDeckId, dstDeckId);

        // Transition window: fixed 15 s or half the (stretched) track length.
        const auto& metaSrc = srcDeck->getDecodedAudio();
        const double effDurSrc = metaSrc.durationSeconds / srcDeck->getTempoRatio();
        const double transDur = std::max(0.5, std::min(15.0, 0.5 * effDurSrc));

        // Render until the mix trigger (last third of the active track), then start.
        const double mixStart = currentTrackMasterStart + effDurSrc - transDur;
        if (mixStart > simTime) {
            renderUntil(ps, simTime, std::min(mixStart, cfg.durationSec), blockTimes);
        }
        if (simTime >= cfg.durationSec) break; // track window ran past the end of the soak

        dstDeck->setPlaybackPosition(0.0);
        dstDeck->setPlaying(true);

        TransitionCommandC cmd{};
        cmd.version = 1;
        cmd.source_deck = srcDeckId;
        cmd.destination_deck = dstDeckId;
        cmd.duration_seconds = transDur;
        cmd.transition_type = (transitionCount % 2 == 0)
            ? static_cast<uint32_t>(pulse::audio::TransitionStrategyType::BassSwap)
            : static_cast<uint32_t>(pulse::audio::TransitionStrategyType::ClassicEqBlend);
        cmd.src_tempo_ratio = 1.0f;
        cmd.dst_tempo_ratio = 1.0f;
        // A rejection (busy executor / malformed plan) is tolerated: the soak then
        // continues with a plain crossfade via the fader below.
        engine.executeTransition(cmd);
        transitionCount++;

        // Render through the transition window.
        renderUntil(ps, simTime, std::min(mixStart + transDur, cfg.durationSec), blockTimes);

        // Complete the handoff: fader to destination, stop source, reset its EQ,
        // preload the next queued track into the now-idle source deck.
        mixer->setCrossfader(dstDeckId == 0 ? -1.0f : 1.0f);
        srcDeck->setPlaying(false);
        srcDeck->resetEq();
        currentTrackMasterStart = mixStart;

        const uint32_t idleDeck = srcDeckId;
        if (nextIndex >= cfg.tracks) {
            nextIndex = 0; // queue wraps — continuous coverage over a finite queue
        }
        usedCount[nextIndex]++;
        if (!engine.loadTrack(static_cast<uint8_t>(idleDeck), trackPaths[nextIndex])) {
            std::fprintf(stderr, "FATAL: preload of track %u into deck %u failed\n", nextIndex, idleDeck);
            return 1;
        }
        nextIndex = (nextIndex + 1) % cfg.tracks;
        dstDeck->setPlaying(true);
    }

    // --- Pass bars ---
    const AudioEngineStatsC stats = engine.getStats();
    const double audioSeconds = simTime;
    const uint64_t expectedFrames = static_cast<uint64_t>(std::llround(audioSeconds / kDt * kBlockFrames));

    bool allTracksUsed = true;
    for (uint32_t i = 0; i < cfg.tracks; ++i) {
        if (usedCount[i] < 1) allTracksUsed = false;
    }
    uint32_t tracksUsed = 0;
    for (uint32_t i = 0; i < cfg.tracks; ++i) {
        if (usedCount[i] >= 1) tracksUsed++;
    }
    // The full-queue coverage bar (every track used >= 1x) only applies when the
    // duration accommodates a complete pass; shorter sanity runs require >= 5
    // distinct tracks (per the plan's 60-second verification check).
    //
    // Deliberate zero-solo design: the mix trigger fires at effDurSrc - transDur
    // with transDur = 0.5 * effDurSrc, so each slot lasts 0.5 * effDurSrc =
    // 0.75 * trackLen (fixtures are 1.5x trackLen) and the next trigger lands
    // exactly at the end of the previous transition window — transitions chain
    // back-to-back and every track's solo (fully-faded-in) segment is ~0 s.
    // This maximizes control-plane churn (executor, deck swaps, preloads), which
    // is the reliability surface the soak exists to cover; steady-state solo
    // coverage is provided by the golden/stress harnesses instead.
    const double slotEstimate = 0.75 * trackLen;
    const double fullPassSec = static_cast<double>(cfg.tracks - 1) * slotEstimate;
    const bool fullPassExpected = cfg.durationSec >= fullPassSec;
    const bool tracksOk = fullPassExpected ? allTracksUsed : (tracksUsed >= 5);
    const bool decksAdvanced = (ps.deckPosWatermark[0] > 1.0) && (ps.deckPosWatermark[1] > 1.0);

    const auto stateOf = [](const DeckPlayer* d) {
        return static_cast<pulse::audio::DeckPlaybackState>(d->getState().playback_state);
    };
    const bool deckStatesOk = [&] {
        for (uint8_t d = 0; d < 2; ++d) {
            const auto st = stateOf(engine.getDeck(d));
            if (st != pulse::audio::DeckPlaybackState::Ready &&
                st != pulse::audio::DeckPlaybackState::Paused &&
                st != pulse::audio::DeckPlaybackState::Playing) {
                return false;
            }
        }
        return true;
    }();

    double sumBlockSec = 0.0;
    for (double t : blockTimes) sumBlockSec += t;
    const double meanBlockSec = blockTimes.empty() ? 0.0 : sumBlockSec / static_cast<double>(blockTimes.size());
    const double budgetSec = static_cast<double>(kBlockFrames) / kSampleRate;

    const bool underrunsOk = stats.underrun_count == 0;
    const bool dropsOk = engine.droppedEvents() == 0;
    const bool framesOk = stats.total_frames_processed == expectedFrames;
    // Engine limiter ceiling constant (0.999f promotes to 0.9990000119).
    const bool peakOk = ps.masterPeak <= 0.999f;
    const bool finiteOk = ps.nonFiniteSamples == 0;
    const bool allocsOk = g_allocationCount == 0;

    const bool pass = underrunsOk && dropsOk && framesOk && peakOk && finiteOk && allocsOk && tracksOk && deckStatesOk && decksAdvanced;

    std::printf("Soak summary: duration=%.1fs rendered=%.1fs transitions=%u blocks=%zu "
                "tracksUsed=%u/%u\n",
                cfg.durationSec, audioSeconds, transitionCount, blockTimes.size(),
                tracksUsed, cfg.tracks);
    std::printf("  frames:   %llu (expected %llu)  %s\n",
                (unsigned long long)stats.total_frames_processed,
                (unsigned long long)expectedFrames, framesOk ? "ok" : "FAIL");
    std::printf("  underruns: %u  %s\n", stats.underrun_count, underrunsOk ? "ok" : "FAIL");
    std::printf("  droppedEvents: %u (drained %llu)  %s\n",
                engine.droppedEvents(), (unsigned long long)ps.drainedEvents, dropsOk ? "ok" : "FAIL");
    std::printf("  masterPeak: %.6f (bar <= 0.999f)  %s\n", ps.masterPeak, peakOk ? "ok" : "FAIL");
    std::printf("  non-finite samples: %llu (bar == 0)  %s\n",
                (unsigned long long)ps.nonFiniteSamples, finiteOk ? "ok" : "FAIL");
    std::printf("  RT allocations: %llu  %s\n",
                (unsigned long long)g_allocationCount, allocsOk ? "ok" : "FAIL");
    std::printf("  tracks used: %u/%u (full pass %s)  %s\n",
                tracksUsed, cfg.tracks, fullPassExpected ? "expected" : "not expected in this duration",
                tracksOk ? "ok" : "FAIL");
    std::printf("  deck states: %u/%u (Ready/Paused/Playing only)  %s\n",
                (unsigned)stateOf(deckA), (unsigned)stateOf(deckB), deckStatesOk ? "ok" : "FAIL");
    std::printf("  deck advance: A=%.2fs B=%.2fs (both > 1s)  %s\n",
                ps.deckPosWatermark[0], ps.deckPosWatermark[1], decksAdvanced ? "ok" : "FAIL");
    std::printf("  cpu estimate: %.2f%% of realtime (advisory)\n", 100.0 * meanBlockSec / budgetSec);

    // --- JSON report (hand-rolled, always written) ---
    {
        std::ofstream out(cfg.reportPath);
        if (!out.is_open()) {
            std::fprintf(stderr, "ERROR: cannot open report path %s\n", cfg.reportPath.c_str());
            return 1;
        }
        out << "{\n";
        out << "  \"duration_seconds\": " << cfg.durationSec << ",\n";
        out << "  \"audio_seconds_rendered\": " << audioSeconds << ",\n";
        out << "  \"transitions_executed\": " << transitionCount << ",\n";
        out << "  \"tracks_total\": " << cfg.tracks << ",\n";
        out << "  \"tracks_used\": " << tracksUsed << ",\n";
        out << "  \"underruns\": " << stats.underrun_count << ",\n";
        out << "  \"dropped_events\": " << engine.droppedEvents() << ",\n";
        out << "  \"master_peak\": " << ps.masterPeak << ",\n";
        out << "  \"non_finite_samples\": " << ps.nonFiniteSamples << ",\n";
        out << "  \"cpu_estimate_pct\": " << (100.0 * meanBlockSec / budgetSec) << ",\n";
        out << "  \"pass\": " << (pass ? "true" : "false") << "\n";
        out << "}\n";
    }

    engine.shutdown();
    std::filesystem::remove_all(dir);
    std::printf(pass ? "Soak: PASS\n" : "Soak: FAIL\n");
    return pass ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[]) {
    SoakConfig cfg;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--duration" && i + 1 < argc) {
            cfg.durationSec = std::atof(argv[++i]);
        } else if (arg == "--report" && i + 1 < argc) {
            cfg.reportPath = argv[++i];
        } else if (arg == "--tracks" && i + 1 < argc) {
            const char* raw = argv[++i];
            char* end = nullptr;
            const long v = std::strtol(raw, &end, 10);
            if (end == raw || *end != '\0' || v < 2 || v > 1024) {
                std::fprintf(stderr, "ERROR: --tracks expects an integer in [2, 1024], got '%s'\n", raw);
                return 2;
            }
            cfg.tracks = static_cast<uint32_t>(v);
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "ERROR: unknown argument '%s'\n", arg.c_str());
            printUsage(argv[0]);
            return 2;
        }
    }
    if (cfg.durationSec <= 0.0 || !std::isfinite(cfg.durationSec)) {
        std::fprintf(stderr, "ERROR: --duration must be a positive finite number of seconds\n");
        return 2;
    }
    return runSoak(cfg);
}
