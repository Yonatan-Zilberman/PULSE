// Production TransitionExecutor test suite.
//
// Cases:
//  a. Sanitization matrix (NaN / Inf / out-of-bounds per v2 field)
//  b. Malformed-plan rejection (source==dest, deck > 1)
//  c. ClassicEqBlend determinism (bit-identical re-render after re-init)
//  d. Real-time audio-thread contract: zero allocations across the full
//     ClassicEqBlend (incl. post-cut ramp) + no seek/reload/stop behavior
//  f. Engine live advance via the IO callback path
//  g. C FFI pulse_audio_execute_transition accept/reject
//
// All cases are deterministic and offline (synthesized fixtures, no hardware).

#include "../include/AudioEngine.h"
#include "../include/DeckPlayer.h"
#include "../include/Mixer.h"
#include "../include/WavWriter.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <new>
#include <string>
#include <vector>

using pulse::audio::AudioEngine;
using pulse::audio::DecodedAudio;
using pulse::audio::DeckPlayer;
using pulse::audio::Mixer;
using pulse::audio::TransitionExecutor;
using pulse::audio::WavWriter;
using TransitionCommandC = ::TransitionCommandC;

// ---- Zero-allocation trap (same TU-global pattern as test_realtime_safety.cpp) ----
static std::atomic<bool> g_trackAllocations{false};
// True on the thread currently driving the simulated real-time callback. The trap
// counts allocations from THAT thread only: CoreAudio's own background
// MIG/dispatch threads can allocate (and, via flat-namespace interposition, through
// this TU's operator new) whenever the system audio config changes — e.g. when
// other test binaries start/stop live audio units. That is system machinery
// outside the engine's real-time path and must not fail the real-time contract.
static thread_local bool t_inRealTimeBlock = false;
static std::atomic<uint64_t> g_allocationCount{0};

#include <execinfo.h>

static void dumpBacktraceOnce() {
    static std::atomic<int> s_dumped{0};
    int expected = 0;
    if (!s_dumped.compare_exchange_strong(expected, 1)) return;
    void* frames[24];
    int n = backtrace(frames, 24);
    char** syms = backtrace_symbols(frames, n);
    for (int i = 0; i < n; ++i) {
        std::cerr << "    [bt] " << (syms ? syms[i] : "?") << std::endl;
    }
    std::free(syms);
}

void* operator new(std::size_t size) {
    if (g_trackAllocations.load(std::memory_order_relaxed) && t_inRealTimeBlock) {
        if (g_allocationCount.fetch_add(1, std::memory_order_relaxed) == 0) {
            dumpBacktraceOnce();
        }
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
    if (g_trackAllocations.load(std::memory_order_relaxed) && t_inRealTimeBlock) {
        if (g_allocationCount.fetch_add(1, std::memory_order_relaxed) == 0) {
            dumpBacktraceOnce();
        }
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

// ---- Fixtures ----
void createToneWav(const std::string& path, double freqHz, double durationSec, float amp = 0.85f, uint32_t sampleRate = 48000) {
    uint64_t totalFrames = static_cast<uint64_t>(durationSec * sampleRate);
    std::vector<float> samples(totalFrames * 2, 0.0f);
    constexpr double twoPi = 6.28318530717958647692;
    for (uint64_t f = 0; f < totalFrames; ++f) {
        double t = static_cast<double>(f) / sampleRate;
        float val = static_cast<float>(std::sin(twoPi * freqHz * t) * amp);
        samples[f * 2 + 0] = val;
        samples[f * 2 + 1] = val;
    }
    WavWriter::writeWav16(path, samples.data(), totalFrames, sampleRate, 2);
}

TransitionCommandC makeClassicPlan() {
    TransitionCommandC c{};
    c.version = 1;
    c.source_deck = 0;
    c.destination_deck = 1;
    c.crossfader_curve = 0; // equal power
    c.duration_seconds = 4.0;
    c.src_tempo_ratio = 1.0f;
    c.dst_tempo_ratio = 0.95f;
    c.dst_tempo_ramp_seconds = 2.0f;
    c.src_gain = 1.0f;
    c.dst_gain = 0.9f;
    c.transition_type = 9; // ClassicEqBlend
    return c;
}

} // namespace

// ============================================================================
// Case a: Sanitization matrix
// ============================================================================
static void testSanitizationMatrix() {
    std::cout << "[a] Sanitization matrix (non-finite / out-of-bounds v2 fields)..." << std::endl;

    Mixer mixer;
    DeckPlayer deckA(0);
    DeckPlayer deckB(1);
    mixer.setCrossfader(-1.0f);
    mixer.setMasterVolume(1.0f);

    int failures = 0;

    auto runCase = [&](const char* name, const TransitionCommandC& in, const std::function<bool(const TransitionCommandC&)>& check) {
        TransitionExecutor exec;
        const int rc = exec.startTransition(in);
        exec.processBlock(1, 48000, mixer, &deckA, &deckB); // consumes the pending plan
        const bool ok = (rc == 0) && exec.isTransitionActive() && check(exec.getActiveCommand());
        if (!ok) {
            ++failures;
            std::cout << "  FAIL: " << name << " (rc=" << rc << ", active=" << exec.isTransitionActive() << ")" << std::endl;
        }
    };

    // One struct per case; everything else zero-initialized (version 0 -> treated as
    // unknown layout -> all defaults), except where the case sets version = 1.
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();

    // version != 1 -> all documented defaults, still executes
    {
        TransitionCommandC c{}; // version 0
        c.source_deck = 1;
        c.destination_deck = 0;
        c.duration_seconds = 99.0;
        c.src_gain = 0.25f;
        c.transition_type = 9;
        runCase("version=0 -> all defaults", c, [](const TransitionCommandC& a) {
            return a.version == 1 && a.source_deck == 0 && a.destination_deck == 1 &&
                   a.duration_seconds == 16.0 && a.src_gain == 1.0f &&
                   a.dst_tempo_ratio == 1.0f && a.transition_type == 0;
        });
    }
    {
        TransitionCommandC c{};
        c.version = 7; // future/unknown
        c.source_deck = 0;
        c.destination_deck = 1;
        runCase("version=7 -> all defaults", c, [](const TransitionCommandC& a) {
            return a.version == 1 && a.duration_seconds == 16.0 && a.src_filter == 0.0f && a.crossfader_end == 1.0f;
        });
    }

    auto mk = [](const char* n) {
        TransitionCommandC c{};
        c.version = 1;
        c.source_deck = 0;
        c.destination_deck = 1;
        c.duration_seconds = 16.0;
        c.src_tempo_ratio = 1.0f;
        c.dst_tempo_ratio = 1.0f;
        c.src_gain = 1.0f;
        c.dst_gain = 1.0f;
        c.src_vocal_stem = 1.0f;
        c.dst_vocal_stem = 1.0f;
        c.crossfader_start = -1.0f;
        c.crossfader_end = 1.0f;
        c.phase_sync_end = 0.5f;
        c.phase_eq_end = 0.875f;
        c.phase_vocal_end = 1.0f;
        c.bass_swap_point = 0.5f;
        c.bass_swap_window = 0.1f;
        c.transition_type = 9;
        (void)n;
        return c;
    };

    const auto close = [](double a, double b) { return std::abs(a - b) < 1e-6; };

    // duration
    { auto c = mk("d"); c.duration_seconds = nan;
      runCase("duration NaN -> 16.0", c, [&](const TransitionCommandC& a) { return close(a.duration_seconds, 16.0); }); }
    { auto c = mk("d"); c.duration_seconds = inf;
      runCase("duration +Inf -> 16.0 default", c, [&](const TransitionCommandC& a) { return close(a.duration_seconds, 16.0); }); }
    { auto c = mk("d"); c.duration_seconds = -inf;
      runCase("duration -Inf -> 16.0 default", c, [&](const TransitionCommandC& a) { return close(a.duration_seconds, 16.0); }); }
    { auto c = mk("d"); c.duration_seconds = 0.0; // just below bound
      runCase("duration 0.0 -> 0.1", c, [&](const TransitionCommandC& a) { return close(a.duration_seconds, 0.1); }); }
    { auto c = mk("d"); c.duration_seconds = 700.0;
      runCase("duration 700 -> 600", c, [&](const TransitionCommandC& a) { return close(a.duration_seconds, 600.0); }); }

    // tempos
    { auto c = mk("t"); c.src_tempo_ratio = nan;
      runCase("src_tempo NaN -> 1.0", c, [&](const TransitionCommandC& a) { return close(a.src_tempo_ratio, 1.0); }); }
    { auto c = mk("t"); c.src_tempo_ratio = 3.0f;
      runCase("src_tempo 3.0 -> 2.0", c, [&](const TransitionCommandC& a) { return close(a.src_tempo_ratio, 2.0); }); }
    { auto c = mk("t"); c.dst_tempo_ratio = 0.25f;
      runCase("dst_tempo 0.25 -> 0.5", c, [&](const TransitionCommandC& a) { return close(a.dst_tempo_ratio, 0.5); }); }
    { auto c = mk("t"); c.dst_tempo_ramp_seconds = 400.0f;
      runCase("ramp 400 -> 300", c, [&](const TransitionCommandC& a) { return close(a.dst_tempo_ramp_seconds, 300.0); }); }
    { auto c = mk("t"); c.dst_tempo_ramp_seconds = -1.0f;
      runCase("ramp -1 -> 0", c, [&](const TransitionCommandC& a) { return close(a.dst_tempo_ramp_seconds, 0.0); }); }

    // gains
    { auto c = mk("g"); c.src_gain = 1.5f;
      runCase("src_gain 1.5 -> 1.0", c, [&](const TransitionCommandC& a) { return close(a.src_gain, 1.0); }); }
    { auto c = mk("g"); c.dst_gain = -0.2f;
      runCase("dst_gain -0.2 -> 0", c, [&](const TransitionCommandC& a) { return close(a.dst_gain, 0.0); }); }
    { auto c = mk("g"); c.src_gain = nan;
      runCase("src_gain NaN -> 1.0", c, [&](const TransitionCommandC& a) { return close(a.src_gain, 1.0); }); }

    // EQ / filter / stems
    { auto c = mk("e"); c.src_low_eq = 1.5f;
      runCase("src_low 1.5 -> 1", c, [&](const TransitionCommandC& a) { return close(a.src_low_eq, 1.0); }); }
    { auto c = mk("e"); c.src_low_eq = -1.5f;
      runCase("src_low -1.5 -> -1", c, [&](const TransitionCommandC& a) { return close(a.src_low_eq, -1.0); }); }
    { auto c = mk("e"); c.dst_mid_eq = nan;
      runCase("dst_mid NaN -> 0", c, [&](const TransitionCommandC& a) { return close(a.dst_mid_eq, 0.0); }); }
    { auto c = mk("e"); c.src_high_eq = inf;
      runCase("src_high +Inf -> 0.0 default", c, [&](const TransitionCommandC& a) { return close(a.src_high_eq, 0.0); }); }
    { auto c = mk("e"); c.dst_filter = -inf;
      runCase("dst_filter -Inf -> 0", c, [&](const TransitionCommandC& a) { return close(a.dst_filter, 0.0); }); }
    { auto c = mk("s"); c.src_vocal_stem = 1.5f;
      runCase("src_vocal 1.5 -> 1", c, [&](const TransitionCommandC& a) { return close(a.src_vocal_stem, 1.0); }); }
    { auto c = mk("s"); c.dst_vocal_stem = -0.5f;
      runCase("dst_vocal -0.5 -> 0", c, [&](const TransitionCommandC& a) { return close(a.dst_vocal_stem, 0.0); }); }

    // crossfader endpoints
    { auto c = mk("x"); c.crossfader_start = nan;
      runCase("xfader_start NaN -> -1", c, [&](const TransitionCommandC& a) { return close(a.crossfader_start, -1.0); }); }
    { auto c = mk("x"); c.crossfader_end = 2.0f;
      runCase("xfader_end 2 -> 1", c, [&](const TransitionCommandC& a) { return close(a.crossfader_end, 1.0); }); }
    { auto c = mk("x"); c.crossfader_start = -2.0f;
      runCase("xfader_start -2 -> -1", c, [&](const TransitionCommandC& a) { return close(a.crossfader_start, -1.0); }); }

    // curve mapping
    { auto c = mk("cv"); c.crossfader_curve = 7;
      runCase("curve 7 -> 2 (clamped)", c, [](const TransitionCommandC& a) { return a.crossfader_curve == 2; }); }

    // monotonic phase cascade
    { auto c = mk("ph"); c.phase_sync_end = 0.9f; c.phase_eq_end = 0.3f; c.phase_vocal_end = 0.2f;
      runCase("phases 0.9/0.3/0.2 -> cascade 0.9/0.9/0.9", c, [&](const TransitionCommandC& a) {
          return close(a.phase_sync_end, 0.9) && close(a.phase_eq_end, 0.9) && close(a.phase_vocal_end, 0.9);
      }); }
    { auto c = mk("ph"); c.phase_sync_end = 0.2f; c.phase_eq_end = 0.9f; c.phase_vocal_end = 0.5f;
      runCase("phases 0.2/0.9/0.5 -> 0.2/0.9/0.9", c, [&](const TransitionCommandC& a) {
          return close(a.phase_sync_end, 0.2) && close(a.phase_eq_end, 0.9) && close(a.phase_vocal_end, 0.9);
      }); }
    { auto c = mk("ph"); c.phase_sync_end = nan; c.phase_eq_end = nan; c.phase_vocal_end = nan;
      runCase("phases NaN -> 0.5/0.875/1.0", c, [&](const TransitionCommandC& a) {
          return close(a.phase_sync_end, 0.5) && close(a.phase_eq_end, 0.875) && close(a.phase_vocal_end, 1.0);
      }); }

    // bass swap
    { auto c = mk("bs"); c.bass_swap_point = 1.2f;
      runCase("swap_point 1.2 -> 0.9", c, [&](const TransitionCommandC& a) { return close(a.bass_swap_point, 0.9); }); }
    { auto c = mk("bs"); c.bass_swap_window = 0.001f;
      runCase("swap_window 0.001 -> 0.02", c, [&](const TransitionCommandC& a) { return close(a.bass_swap_window, 0.02); }); }
    { auto c = mk("bs"); c.bass_swap_window = 0.9f;
      runCase("swap_window 0.9 -> 0.5", c, [&](const TransitionCommandC& a) { return close(a.bass_swap_window, 0.5); }); }

    // unknown transition type -> accepted, raw value retained, executor active
    { auto c = mk("ty"); c.transition_type = 1234;
      runCase("unknown type 1234 -> accepted + active", c, [](const TransitionCommandC& a) { return a.transition_type == 1234; }); }

    if (failures == 0) {
        std::cout << "  All sanitization matrix cases passed." << std::endl;
    }
    assert(failures == 0);
}

// ============================================================================
// Case b: Malformed-plan rejection
// ============================================================================
static void testMalformedRejection() {
    std::cout << "[b] Malformed-plan rejection (structural)..." << std::endl;

    Mixer mixer;
    DeckPlayer deckA(0);
    DeckPlayer deckB(1);
    mixer.setCrossfader(0.3f);
    mixer.setMasterVolume(1.0f);
    deckA.setEq(0.25f, -0.4f, 0.1f);
    deckA.setVolume(0.6f);
    deckB.setFilter(-0.5f);

    TransitionExecutor exec;

    auto checkUntouched = [&]() {
        return mixer.getCrossfader() == 0.3f &&
               std::abs(deckA.getEqLow() - 0.25f) < 1e-6f &&
               std::abs(deckA.getEqMid() - (-0.4f)) < 1e-6f &&
               std::abs(deckA.getEqHigh() - 0.1f) < 1e-6f &&
               std::abs(deckA.getVolume() - 0.6f) < 1e-6f &&
               std::abs(deckB.getFilter() - (-0.5f)) < 1e-6f;
    };

    // source == destination
    {
        TransitionCommandC c = makeClassicPlan();
        c.source_deck = 1;
        c.destination_deck = 1;
        assert(exec.startTransition(c) == -1);
        exec.processBlock(512, 48000, mixer, &deckA, &deckB);
        assert(!exec.isTransitionActive());
        assert(checkUntouched());
        std::cout << "  source==destination rejected, mixer+decks untouched" << std::endl;
    }
    // source_deck out of range
    {
        TransitionCommandC c = makeClassicPlan();
        c.source_deck = 7;
        assert(exec.startTransition(c) == -1);
        exec.processBlock(512, 48000, mixer, &deckA, &deckB);
        assert(!exec.isTransitionActive());
        assert(checkUntouched());
        std::cout << "  source_deck=7 rejected, mixer+decks untouched" << std::endl;
    }
    // destination_deck out of range
    {
        TransitionCommandC c = makeClassicPlan();
        c.destination_deck = 3;
        assert(exec.startTransition(c) == -1);
        exec.processBlock(512, 48000, mixer, &deckA, &deckB);
        assert(!exec.isTransitionActive());
        assert(checkUntouched());
        std::cout << "  destination_deck=3 rejected, mixer+decks untouched" << std::endl;
    }
    // valid minimal plan accepted
    {
        TransitionCommandC c{};
        c.version = 1;
        c.source_deck = 0;
        c.destination_deck = 1;
        c.duration_seconds = 2.0;
        c.transition_type = 0;
        assert(exec.startTransition(c) == 0);
        exec.processBlock(512, 48000, mixer, &deckA, &deckB);
        assert(exec.isTransitionActive());
        std::cout << "  valid minimal plan accepted and active" << std::endl;
    }
}

// ============================================================================
// Engine-state normalization (determinism helper)
// ============================================================================
static void resetEngineCanonical(AudioEngine& engine, const std::string& file0, const std::string& file1) {
    engine.stopDeck(0);
    engine.stopDeck(1);

    // 1) Neutral parameters FIRST: prepareDeck re-snaps every per-sample smoother to
    //    the current atomic values, so the atoms must already be canonical (this is
    //    what makes two runs bit-identical regardless of the previous run's state).
    for (uint8_t id : {0, 1}) {
        DeckPlayer* d = engine.getDeck(id);
        d->setVolume(1.0f);
        d->setEq(0.0f, 0.0f, 0.0f);
        d->setFilter(0.0f);
        d->setStemLevels(1.0f, 1.0f, 1.0f, 1.0f);
        d->setTempoRatio(1.0);
    }

    // 2) Fresh DSP state (stretch engine cleared, biquad state zeroed, smoothers snapped).
    engine.prepareDeck(0, file0, 0.0, 1.0, true);
    engine.prepareDeck(1, file1, 0.0, 1.0, true);

    // 3) Position back to 0 + queues cleared.
    engine.stopDeck(0);
    engine.stopDeck(1);

    // 4) Mixer: canonical position + snap all per-sample smoothers to it.
    Mixer* m = engine.getMixer();
    m->setCrossfader(-1.0f);
    m->setMasterVolume(1.0f);
    m->init(48000);

    // 5) Play both decks from 0.
    engine.playDeck(0);
    engine.playDeck(1);
}

struct BlockSnapshot {
    float xfader;
    float vol[2];
    float low[2];
    float mid[2];
    float high[2];
    float filter[2];
    float vocal[2];
    float drum[2];
    float bass[2];
    float other[2];
    double tempo[2];
};

static BlockSnapshot takeSnapshot(AudioEngine& engine) {
    BlockSnapshot s{};
    s.xfader = engine.getMixer()->getCrossfader();
    for (uint8_t id : {0, 1}) {
        int i = id;
        const DeckPlayer* d = engine.getDeck(id);
        s.vol[i] = d->getVolume();
        s.low[i] = d->getEqLow();
        s.mid[i] = d->getEqMid();
        s.high[i] = d->getEqHigh();
        s.filter[i] = d->getFilter();
        s.vocal[i] = d->getVocalStem();
        s.drum[i] = d->getDrumStem();
        s.bass[i] = d->getBassStem();
        s.other[i] = d->getOtherStem();
        s.tempo[i] = d->getTempoRatio();
    }
    return s;
}

static bool snapshotEquals(const BlockSnapshot& a, const BlockSnapshot& b) {
    return a.xfader == b.xfader &&
           std::equal(std::begin(a.vol), std::end(a.vol), std::begin(b.vol)) &&
           std::equal(std::begin(a.low), std::end(a.low), std::begin(b.low)) &&
           std::equal(std::begin(a.mid), std::end(a.mid), std::begin(b.mid)) &&
           std::equal(std::begin(a.high), std::end(a.high), std::begin(b.high)) &&
           std::equal(std::begin(a.filter), std::end(a.filter), std::begin(b.filter)) &&
           std::equal(std::begin(a.vocal), std::end(a.vocal), std::begin(b.vocal)) &&
           std::equal(std::begin(a.drum), std::end(a.drum), std::begin(b.drum)) &&
           std::equal(std::begin(a.bass), std::end(a.bass), std::begin(b.bass)) &&
           std::equal(std::begin(a.other), std::end(a.other), std::begin(b.other)) &&
           std::equal(std::begin(a.tempo), std::end(a.tempo), std::begin(b.tempo));
}

// ============================================================================
// Case c: ClassicEqBlend determinism (bit-identical re-render)
// ============================================================================
static void testDeterminism(AudioEngine& engine, const std::string& file0, const std::string& file1) {
    std::cout << "[c] ClassicEqBlend determinism (bit-identical re-render after re-init)..." << std::endl;

    constexpr uint32_t kRate = 48000;
    constexpr uint32_t kBlock = 512;
    constexpr uint32_t kChannels = 2;
    // 4.0s transition + 2.0s post-cut ramp == 6.0s
    constexpr int kBlocks = 570; // 6.0s / (512/48000)

    const AudioEngineConfigC cfg{kRate, kBlock, kChannels};

    // Run 1
    {
        engine.initialize(cfg);
        resetEngineCanonical(engine, file0, file1);
        assert(engine.executeTransition(makeClassicPlan()) == 0);
    }
    std::vector<float> out1;
    std::vector<BlockSnapshot> snap1;
    out1.reserve(static_cast<size_t>(kBlocks) * kBlock * kChannels);
    {
        std::vector<float> block(kBlock * kChannels, 0.0f);
        for (int i = 0; i < kBlocks; ++i) {
            engine.processAudioBlock(block.data(), kBlock, kChannels);
            out1.insert(out1.end(), block.begin(), block.end());
            snap1.push_back(takeSnapshot(engine));
        }
    }

    // Re-init: shutdown + initialize + reload + re-request the identical plan.
    assert(engine.shutdown() == 0);
    engine.initialize(cfg);
    resetEngineCanonical(engine, file0, file1);
    assert(engine.executeTransition(makeClassicPlan()) == 0);

    // Run 2
    std::vector<float> out2;
    std::vector<BlockSnapshot> snap2;
    out2.reserve(out1.size());
    bool paramsIdentical = true;
    float maxSampleDiff = 0.0f;
    {
        std::vector<float> block(kBlock * kChannels, 0.0f);
        const size_t blockSamples = static_cast<size_t>(kBlock) * kChannels;
        for (int i = 0; i < kBlocks; ++i) {
            engine.processAudioBlock(block.data(), kBlock, kChannels);
            for (size_t s = 0; s < block.size(); ++s) {
                const float diff = std::abs(block[s] - out1[i * blockSamples + s]);
                if (diff > maxSampleDiff) maxSampleDiff = diff;
            }
            out2.insert(out2.end(), block.begin(), block.end());
            snap2.push_back(takeSnapshot(engine));
            if (!snapshotEquals(snap1[i], snap2[i])) {
                paramsIdentical = false;
            }
        }
    }

    std::cout << std::fixed << std::setprecision(6)
              << "  Max sample diff across " << out1.size() << " samples: " << maxSampleDiff << std::endl;
    assert(maxSampleDiff == 0.0f); // bit-identical
    assert(paramsIdentical);
    assert(out1.size() == out2.size());
    std::cout << "  Per-block parameter sequences identical; output bit-identical." << std::endl;
}

// ============================================================================
// Case d: Real-time audio-thread contract (zero allocation, no planning behavior)
// ============================================================================
static void testRealtimeContract(AudioEngine& engine, const std::string& file0, const std::string& file1) {
    std::cout << "[d] Real-time contract: zero allocations + no seek/reload/stop during ClassicEqBlend..." << std::endl;

    constexpr uint32_t kRate = 48000;
    constexpr uint32_t kBlock = 2048;
    constexpr uint32_t kChannels = 2;
    constexpr int kBlocks = 1500; // ~64s: covers the full 20s transition incl. post-ramp + idle

    const AudioEngineConfigC cfg{kRate, kBlock, kChannels};
    engine.initialize(cfg);
    resetEngineCanonical(engine, file0, file1);

    TransitionCommandC plan = makeClassicPlan();
    plan.duration_seconds = 16.0;
    plan.dst_tempo_ratio = 1.0f; // no stretch -> deterministic per-block position deltas
    plan.dst_tempo_ramp_seconds = 4.0;
    assert(engine.executeTransition(plan) == 0);

    DeckPlayer* deckA = engine.getDeck(0);
    DeckPlayer* deckB = engine.getDeck(1);

    const DecodedAudio* audioA = deckA->getLoadedAudio(); // stable pointer; must not change mid-transition
    const DecodedAudio* audioB = deckB->getLoadedAudio();
    assert(deckA->getPlaybackState() == pulse::audio::DeckPlaybackState::Playing);
    assert(deckB->getPlaybackState() == pulse::audio::DeckPlaybackState::Playing);

    std::vector<float> masterOut(kBlock * kChannels, 0.0f);
    engine.processAudioBlock(masterOut.data(), kBlock, kChannels); // warm-up, trap off

    std::vector<float> chanL(kBlock, 0.0f);
    std::vector<float> chanR(kBlock, 0.0f);
    float* outputChannels[2] = {chanL.data(), chanR.data()};
    juce::AudioIODeviceCallbackContext ctx{};

    std::cout << "Enabling allocation trap across " << kBlocks << " real-time IO-callback blocks..." << std::endl;
    g_allocationCount.store(0, std::memory_order_relaxed);
    g_trackAllocations.store(true, std::memory_order_seq_cst);

    double expectedDelta = static_cast<double>(kBlock) / kRate;
    double oneSample = 1.0 / kRate;
    double prevPosA = deckA->getPlaybackPosition();
    double prevPosB = deckB->getPlaybackPosition();
    const double durationA = deckA->getDuration();
    const double durationB = deckB->getDuration();
    bool positionMonotone = true;

    t_inRealTimeBlock = true;
    for (int i = 0; i < kBlocks; ++i) {
        engine.audioDeviceIOCallbackWithContext(nullptr, 0, outputChannels, 2, static_cast<int>(kBlock), ctx);

        // No planning behavior: while audio is still available, state stays Playing and
        // the loaded audio is never swapped (the executor must not seek/reload/stop).
        // (After the 24 s fixture ends the deck legitimately pauses — that is normal
        // end-of-audio, not a violation.)
        if (prevPosA < durationA - 0.05 && prevPosB < durationB - 0.05) {
            if (deckA->getPlaybackState() != pulse::audio::DeckPlaybackState::Playing ||
                deckB->getPlaybackState() != pulse::audio::DeckPlaybackState::Playing ||
                deckA->getLoadedAudio() != audioA || deckB->getLoadedAudio() != audioB) {
                positionMonotone = false;
                std::cout << "  VIOLATION at block " << i << ": state/reload changed" << std::endl;
            }
        }

        // No seek: playback position advances monotonically at block rate (+/- 1 sample).
        const double posA = deckA->getPlaybackPosition();
        const double posB = deckB->getPlaybackPosition();
        if (posA < prevPosA - oneSample || posB < prevPosB - oneSample) {
            positionMonotone = false;
            std::cout << "  VIOLATION at block " << i << ": position went backwards" << std::endl;
        }
        if (prevPosA < durationA - 0.05 && std::abs((posA - prevPosA) - expectedDelta) > oneSample) {
            positionMonotone = false;
            std::cout << "  VIOLATION at block " << i << ": unexpected delta A=" << (posA - prevPosA) << std::endl;
        }
        if (prevPosB < durationB - 0.05 && std::abs((posB - prevPosB) - expectedDelta) > oneSample) {
            positionMonotone = false;
            std::cout << "  VIOLATION at block " << i << ": unexpected delta B=" << (posB - prevPosB) << std::endl;
        }
        prevPosA = posA;
        prevPosB = posB;
    }
    t_inRealTimeBlock = false;

    uint64_t totalAllocations = g_allocationCount.load(std::memory_order_relaxed);
    g_trackAllocations.store(false, std::memory_order_seq_cst);

    std::cout << "Total real-time callback allocations detected: " << totalAllocations << " (audio-thread scoped)" << std::endl;
    assert(totalAllocations == 0);
    assert(positionMonotone);
    assert(!engine.getTransitionExecutor()->isTransitionActive()); // 20s transition completed inside 64s window
    assert(std::abs(engine.getMixer()->getCrossfader() - 1.0f) < 1e-5f); // parked at crossfader_end
    std::cout << "  Transition completed, crossfader parked at endpoint, no seek/reload/stop." << std::endl;
}

// ============================================================================
// Case f: Engine live advance (IO callback drives the transition)
// ============================================================================
static void testEngineLiveAdvance(AudioEngine& engine, const std::string& file0, const std::string& file1) {
    std::cout << "[f] Engine live advance via IO callback path..." << std::endl;

    constexpr uint32_t kRate = 48000;
    constexpr uint32_t kBlock = 2048;
    constexpr uint32_t kChannels = 2;

    const AudioEngineConfigC cfg{kRate, kBlock, kChannels};
    engine.initialize(cfg);
    resetEngineCanonical(engine, file0, file1);

    TransitionCommandC plan = makeClassicPlan(); // 4.0s + 2.0s ramp = 6.0s total
    assert(engine.executeTransition(plan) == 0);

    std::vector<float> chanL(kBlock, 0.0f);
    std::vector<float> chanR(kBlock, 0.0f);
    float* outputChannels[2] = {chanL.data(), chanR.data()};
    juce::AudioIODeviceCallbackContext ctx{};

    TransitionExecutor* exec = engine.getTransitionExecutor();

    // ~1s in: not active yet? The transition starts on the first block (pending consumed).
    engine.audioDeviceIOCallbackWithContext(nullptr, 0, outputChannels, 2, static_cast<int>(kBlock), ctx);
    assert(exec->isTransitionActive());

    // Mid-window: active.
    for (int i = 0; i < 20; ++i) {
        engine.audioDeviceIOCallbackWithContext(nullptr, 0, outputChannels, 2, static_cast<int>(kBlock), ctx);
    }
    assert(exec->isTransitionActive());
    const double midProgress = exec->getNormalizedProgress();
    assert(midProgress > 0.1 && midProgress < 0.8);
    std::cout << "  Mid-window progress: " << std::fixed << std::setprecision(3) << midProgress << std::endl;

    // Run past duration + ramp: 6.0s == 140.6 blocks; run a total of 200.
    for (int i = 0; i < 179; ++i) {
        engine.audioDeviceIOCallbackWithContext(nullptr, 0, outputChannels, 2, static_cast<int>(kBlock), ctx);
    }
    assert(!exec->isTransitionActive());
    assert(std::abs(engine.getMixer()->getCrossfader() - 1.0f) < 1e-5f);
    std::cout << "  Deactivated after duration + ramp; crossfader at endpoint." << std::endl;
}

// ============================================================================
// Case g: C FFI
// ============================================================================
static void testFfi() {
    std::cout << "[g] C FFI pulse_audio_execute_transition..." << std::endl;

    TransitionCommandC valid = makeClassicPlan();
    valid.duration_seconds = 2.0;
    assert(pulse_audio_execute_transition(valid) == 0);
    std::cout << "  valid v2 plan accepted (rc=0)" << std::endl;

    TransitionCommandC bad = makeClassicPlan();
    bad.source_deck = 0;
    bad.destination_deck = 0; // source == destination
    assert(pulse_audio_execute_transition(bad) == -1);
    std::cout << "  source==destination rejected (rc=-1)" << std::endl;
}

int main() {
    std::cout << "==================================================" << std::endl;
    std::cout << "Running Production TransitionExecutor Test Suite" << std::endl;
    std::cout << "==================================================" << std::endl;

    testSanitizationMatrix();
    testMalformedRejection();

    std::filesystem::create_directories("tests/audio/trans_exec_tmp");
    const std::string file0 = "tests/audio/trans_exec_tmp/tone_60hz.wav";
    const std::string file1 = "tests/audio/trans_exec_tmp/tone_90hz.wav";
    // 24 s fixtures: long enough to outlast the full 20 s transition in case (d).
    createToneWav(file0, 60.0, 24.0, 0.85f);
    createToneWav(file1, 90.0, 24.0, 0.85f);

    auto& engine = AudioEngine::getInstance();

    testDeterminism(engine, file0, file1);
    testRealtimeContract(engine, file0, file1);
    testEngineLiveAdvance(engine, file0, file1);
    testFfi();

    assert(engine.shutdown() == 0);

    std::cout << "==================================================" << std::endl;
    std::cout << "TransitionExecutor Test Suite PASSED" << std::endl;
    std::cout << "==================================================" << std::endl;
    return 0;
}
