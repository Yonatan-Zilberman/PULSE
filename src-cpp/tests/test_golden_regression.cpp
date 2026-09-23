#include "../include/AudioEngine.h"
#include "../include/AudioDecoder.h"
#include "../include/WavWriter.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <cstdint>

namespace {

// ---------------------------------------------------------------------------
// Comparison metrics. PROVENANCE: copied from src/pulse_dsp_comparator.cpp
// (~lines 19-122: ComparisonMetrics + compareBuffers) for the self-contained
// single-file test style. Deliberate differences from the canonical copy:
// the peak-dBFS ceiling parameter is dropped (clip-count bar replaces it) and
// a max-abs-delta bar is added. If the canonical comparator's metric math
// changes, re-sync this copy (and regenerate references only if the math —
// not a build regression — is what changed).
// ---------------------------------------------------------------------------
struct ComparisonMetrics {
    std::string profileName;
    uint64_t totalFrames{0};
    uint32_t sampleRate{48000};
    uint32_t channels{2};
    float refPeakDbfs{-100.0f};
    float candPeakDbfs{-100.0f};
    float refRmsDbfs{-100.0f};
    float candRmsDbfs{-100.0f};
    float maxAbsDelta{0.0f};
    float rmsDeltaDbfs{-100.0f};
    float snrDb{0.0f};
    uint64_t clippedSampleCount{0};
    uint64_t refClippedSampleCount{0};
    bool passed{false};
    std::string failureReason;
};

ComparisonMetrics compareBuffers(const std::string& name,
                                 const float* refSamples,
                                 const float* candSamples,
                                 uint64_t totalFrames,
                                 uint32_t sampleRate,
                                 uint32_t channels,
                                 float expectedMinSnrDb = 80.0f) {
    ComparisonMetrics m;
    m.profileName = name;
    m.totalFrames = totalFrames;
    m.sampleRate = sampleRate;
    m.channels = channels;

    size_t totalSamples = totalFrames * channels;
    if (totalSamples == 0) {
        m.passed = false;
        m.failureReason = "Empty audio buffer";
        return m;
    }

    double refSumSq = 0.0;
    double candSumSq = 0.0;
    double errorSumSq = 0.0;
    float maxRefPeak = 0.0f;
    float maxCandPeak = 0.0f;
    float maxDelta = 0.0f;
    uint64_t clipCount = 0;
    uint64_t refClipCount = 0;

    for (size_t i = 0; i < totalSamples; ++i) {
        float r = refSamples[i];
        float c = candSamples[i];

        float absR = std::abs(r);
        float absC = std::abs(c);
        if (absR > maxRefPeak) maxRefPeak = absR;
        if (absC > maxCandPeak) maxCandPeak = absC;

        float delta = std::abs(c - r);
        if (delta > maxDelta) maxDelta = delta;

        refSumSq += r * r;
        candSumSq += c * c;
        errorSumSq += delta * delta;

        if (absC >= 1.0f - 1e-5f) {
            clipCount++;
        }
        if (absR >= 1.0f - 1e-5f) {
            refClipCount++;
        }
    }

    m.refPeakDbfs = (maxRefPeak > 1e-7f) ? 20.0f * std::log10(maxRefPeak) : -100.0f;
    m.candPeakDbfs = (maxCandPeak > 1e-7f) ? 20.0f * std::log10(maxCandPeak) : -100.0f;

    double refRms = std::sqrt(refSumSq / totalSamples);
    double candRms = std::sqrt(candSumSq / totalSamples);
    double errorRms = std::sqrt(errorSumSq / totalSamples);

    m.refRmsDbfs = (refRms > 1e-7) ? static_cast<float>(20.0 * std::log10(refRms)) : -100.0f;
    m.candRmsDbfs = (candRms > 1e-7) ? static_cast<float>(20.0 * std::log10(candRms)) : -100.0f;
    m.rmsDeltaDbfs = (errorRms > 1e-7) ? static_cast<float>(20.0 * std::log10(errorRms)) : -100.0f;
    m.maxAbsDelta = maxDelta;
    m.clippedSampleCount = clipCount;
    m.refClippedSampleCount = refClipCount;

    if (errorSumSq < 1e-12) {
        m.snrDb = 120.0f; // Effectively infinite SNR
    } else if (refSumSq > 1e-12) {
        m.snrDb = static_cast<float>(10.0 * std::log10(refSumSq / errorSumSq));
    } else {
        m.snrDb = 0.0f;
    }

    m.passed = true;
    if (m.snrDb < expectedMinSnrDb) {
        m.passed = false;
        m.failureReason = "SNR (" + std::to_string(m.snrDb) + " dB) below threshold (" + std::to_string(expectedMinSnrDb) + " dB)";
    } else if (m.clippedSampleCount > 0) {
        m.passed = false;
        m.failureReason = "Detected " + std::to_string(m.clippedSampleCount) + " clipped samples (>= 0 dBFS)";
    } else if (m.maxAbsDelta > 5e-3f) {
        m.passed = false;
        m.failureReason = "Max abs delta (" + std::to_string(m.maxAbsDelta) + ") above bound (0.005)";
    }

    return m;
}

// ---------------------------------------------------------------------------
// Golden scenario definitions (all 48 kHz / 2 ch / engine config {48000, 512, 2};
// deterministic control timelines: fixed block counts, no wall clock, no RNG).
// ---------------------------------------------------------------------------
constexpr uint32_t kSampleRate = 48000;
constexpr uint32_t kBlockFrames = 512;
constexpr uint32_t kChannels = 2;
constexpr double kSecondsPerBlock = static_cast<double>(kBlockFrames) / kSampleRate;

struct Scenario {
    const char* name;
    const char* refFile;
};

const Scenario kScenarios[] = {
    {"ref_eq_blend_classic", "ref_eq_blend_classic.wav"},
    {"ref_eq_blend_scurve", "ref_eq_blend_scurve.wav"},
    {"ref_bass_swap", "ref_bass_swap.wav"},
    {"ref_tempo_matched", "ref_tempo_matched.wav"},
    {"ref_full_set", "ref_full_set.wav"},
};

std::string fixtureDir();

uint32_t blocksForSeconds(double seconds) {
    return static_cast<uint32_t>(std::llround(seconds / kSecondsPerBlock));
}

// Renders `blocks` blocks (512 frames each) into outBuf (appended), draining
// events into a stack buffer each block.
void pump(pulse::audio::AudioEngine& engine, uint32_t blocks, std::vector<float>& outBuf) {
    static_assert(sizeof(AudioEventC) == 32);
    alignas(32) AudioEventC evBuf[256];
    std::vector<float> blockBuf(kBlockFrames * kChannels, 0.0f);
    for (uint32_t i = 0; i < blocks; ++i) {
        engine.processAudioBlock(blockBuf.data(), kBlockFrames, kChannels);
        engine.drainEvents(evBuf, 256, nullptr);
        outBuf.insert(outBuf.end(), blockBuf.begin(), blockBuf.end());
    }
}

std::string fixturePath(const std::string& fileName) {
    return (std::filesystem::path(fixtureDir()) / fileName).string();
}

pulse::audio::AudioEngine& engine() {
    return pulse::audio::AudioEngine::getInstance();
}

void initEngine() {
    AudioEngineConfigC cfg{kSampleRate, kBlockFrames, kChannels};
    if (engine().initialize(cfg) != 0) {
        std::fprintf(stderr, "FATAL: engine initialize failed\n");
        std::exit(1);
    }
}

bool loadOrDie(uint8_t deck, const std::string& path) {
    if (!engine().loadTrack(deck, path)) {
        std::fprintf(stderr, "FATAL: loadTrack(%u, %s) failed\n", (unsigned)deck, path.c_str());
        std::exit(1);
    }
    return true;
}

TransitionCommandC makeCmd(uint8_t src, uint8_t dst, double durationSec,
                           uint32_t transitionType) {
    TransitionCommandC cmd{};
    cmd.version = 1;
    cmd.source_deck = src;
    cmd.destination_deck = dst;
    cmd.duration_seconds = durationSec;
    cmd.transition_type = transitionType;
    cmd.src_tempo_ratio = 1.0f;
    cmd.dst_tempo_ratio = 1.0f;
    return cmd;
}

// --- Scenario 1: ref_eq_blend_classic — 8 s, 128->120 BPM, ClassicEqBlend (9) ---
uint64_t renderEqBlendClassic(std::vector<float>& out) {
    initEngine();
    loadOrDie(0, fixturePath("golden_synth_a.wav"));
    loadOrDie(1, fixturePath("golden_synth_b.wav"));
    engine().playDeck(0);
    engine().playDeck(1);
    pump(engine(), blocksForSeconds(2.0), out);
    engine().executeTransition(makeCmd(0, 1, 6.0,
                                       static_cast<uint32_t>(pulse::audio::TransitionStrategyType::ClassicEqBlend)));
    pump(engine(), blocksForSeconds(8.0) - blocksForSeconds(2.0), out);
    return out.size() / kChannels;
}

// --- Scenario 2: ref_eq_blend_scurve — 8 s, same pair, SCurve + preset deck EQs ---
uint64_t renderEqBlendSCurve(std::vector<float>& out) {
    initEngine();
    loadOrDie(0, fixturePath("golden_synth_a.wav"));
    loadOrDie(1, fixturePath("golden_synth_b.wav"));
    engine().playDeck(0);
    engine().playDeck(1);
    pump(engine(), blocksForSeconds(2.0), out);
    auto cmd = makeCmd(0, 1, 6.0,
                       static_cast<uint32_t>(pulse::audio::TransitionStrategyType::ClassicEqBlend));
    cmd.crossfader_curve = static_cast<uint8_t>(pulse::audio::CrossfaderCurveType::SCurve);
    cmd.src_mid_eq = -0.3f;
    cmd.dst_high_eq = 0.3f;
    engine().executeTransition(cmd);
    pump(engine(), blocksForSeconds(8.0) - blocksForSeconds(2.0), out);
    return out.size() / kChannels;
}

// --- Scenario 3: ref_bass_swap — 8 s, bass-heavy fixtures, BassSwap (2) ---
uint64_t renderBassSwap(std::vector<float>& out) {
    initEngine();
    loadOrDie(0, fixturePath("golden_bass_a.wav"));
    loadOrDie(1, fixturePath("golden_bass_b.wav"));
    engine().playDeck(0);
    engine().playDeck(1);
    pump(engine(), blocksForSeconds(2.0), out);
    engine().executeTransition(makeCmd(0, 1, 8.0,
                                       static_cast<uint32_t>(pulse::audio::TransitionStrategyType::BassSwap)));
    pump(engine(), blocksForSeconds(8.0) - blocksForSeconds(2.0), out);
    return out.size() / kChannels;
}

// --- Scenario 4: ref_tempo_matched — 10 s, 120 vs 126 BPM, matchTempo + BassSwap ---
uint64_t renderTempoMatched(std::vector<float>& out) {
    initEngine();
    loadOrDie(0, fixturePath("golden_tm_a.wav"));
    loadOrDie(1, fixturePath("golden_tm_b.wav"));
    engine().playDeck(0);
    engine().playDeck(1);
    pump(engine(), blocksForSeconds(1.0), out);
    engine().matchTempo(0, 1);
    engine().executeTransition(makeCmd(0, 1, 8.0,
                                       static_cast<uint32_t>(pulse::audio::TransitionStrategyType::BassSwap)));
    pump(engine(), blocksForSeconds(10.0) - blocksForSeconds(1.0), out);
    return out.size() / kChannels;
}

// --- Scenario 5: ref_full_set — 24 s, 3 tracks, ClassicEqBlend then BassSwap ---
uint64_t renderFullSet(std::vector<float>& out) {
    initEngine();
    loadOrDie(0, fixturePath("golden_fs_a.wav"));
    loadOrDie(1, fixturePath("golden_fs_b.wav"));
    engine().playDeck(0);
    engine().playDeck(1);
    // Slot 1 (0-8 s): ClassicEqBlend 0 -> 1
    engine().executeTransition(makeCmd(0, 1, 8.0,
                                       static_cast<uint32_t>(pulse::audio::TransitionStrategyType::ClassicEqBlend)));
    pump(engine(), blocksForSeconds(8.0), out);
    // Slot 2 (8-16 s): finish src 0, load next track, BassSwap 1 -> 0
    engine().stopDeck(0);
    loadOrDie(0, fixturePath("golden_fs_c.wav"));
    engine().playDeck(0);
    engine().executeTransition(makeCmd(1, 0, 8.0,
                                       static_cast<uint32_t>(pulse::audio::TransitionStrategyType::BassSwap)));
    pump(engine(), blocksForSeconds(8.0), out);
    // Slot 3 (16-24 s): steady state on deck 0
    engine().stopDeck(1);
    pump(engine(), blocksForSeconds(8.0), out);
    return out.size() / kChannels;
}

struct ScenarioRunner {
    const Scenario meta;
    uint64_t (*renderFn)(std::vector<float>&);
};

const ScenarioRunner kRunners[] = {
    {kScenarios[0], renderEqBlendClassic},
    {kScenarios[1], renderEqBlendSCurve},
    {kScenarios[2], renderBassSwap},
    {kScenarios[3], renderTempoMatched},
    {kScenarios[4], renderFullSet},
};

std::string fixtureDir() {
    std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "pulse_golden_test";
    std::filesystem::create_directories(dir);
    return dir.string();
}

void generateFixtures() {
    using pulse::audio::WavWriter;
    std::string dir = fixtureDir();
    if (!WavWriter::createSyntheticFixture((dir + "/golden_synth_a.wav"), 440.0, 20.0, 128.0, 0.7f, kSampleRate) ||
        !WavWriter::createSyntheticFixture((dir + "/golden_synth_b.wav"), 523.25, 20.0, 120.0, 0.7f, kSampleRate) ||
        !WavWriter::createBassHeavyFixture((dir + "/golden_bass_a.wav"), 60.0, 440.0, 20.0, 120.0, 0.8f, kSampleRate) ||
        !WavWriter::createBassHeavyFixture((dir + "/golden_bass_b.wav"), 55.0, 523.25, 20.0, 122.0, 0.8f, kSampleRate) ||
        !WavWriter::createSyntheticFixture((dir + "/golden_tm_a.wav"), 440.0, 20.0, 120.0, 0.7f, kSampleRate) ||
        !WavWriter::createSyntheticFixture((dir + "/golden_tm_b.wav"), 523.25, 20.0, 126.0, 0.7f, kSampleRate) ||
        !WavWriter::createSyntheticFixture((dir + "/golden_fs_a.wav"), 440.0, 20.0, 120.0, 0.7f, kSampleRate) ||
        !WavWriter::createSyntheticFixture((dir + "/golden_fs_b.wav"), 523.25, 20.0, 128.0, 0.7f, kSampleRate) ||
        !WavWriter::createSyntheticFixture((dir + "/golden_fs_c.wav"), 587.33, 20.0, 132.0, 0.7f, kSampleRate)) {
        std::fprintf(stderr, "FATAL: fixture generation failed\n");
        std::exit(1);
    }
}

// Three-tier reference directory resolution (mirrors test_realtime_safety pattern).
std::string resolveReferenceDir() {
    const char* tiers[] = {
        "tests/golden-set/references",
        "../tests/golden-set/references",
        "../../tests/golden-set/references",
    };
    for (const char* t : tiers) {
        std::filesystem::path p(t);
        if (std::filesystem::is_directory(p)) {
            return p.string();
        }
    }
    return {};
}

void printUsage(const char* prog) {
    std::printf("Pulse Golden-Set Regression Harness\n"
                "Usage:\n"
                "  %s                       Run regression comparison vs committed references\n"
                "  %s --generate [dir]      (Re)generate float32 references (default: tests/golden-set/references)\n",
                prog, prog);
}

int runGenerate(const std::string& dir) {
    using pulse::audio::WavWriter;
    generateFixtures();
    std::filesystem::create_directories(dir);
    bool ok = true;
    for (const auto& runner : kRunners) {
        std::vector<float> out;
        uint64_t frames = runner.renderFn(out);

        // Refuse to commit a clipping reference: writeWav32 clamps, so it would
        // otherwise 'successfully' write a clipped file that the regression
        // clip-count bar can never question.
        uint64_t clips = 0;
        for (float s : out) {
            if (std::abs(s) >= 1.0f - 1e-5f) clips++;
        }
        if (clips > 0) {
            std::fprintf(stderr, "ERROR: [%s] render contains %llu clipped samples; "
                         "refusing to write reference (regenerate from a non-clipping build)\n",
                         runner.meta.name, (unsigned long long)clips);
            ok = false;
            continue;
        }

        std::string path = (std::filesystem::path(dir) / runner.meta.refFile).string();
        if (!WavWriter::writeWav32(path, out.data(), frames, kSampleRate, kChannels)) {
            std::fprintf(stderr, "ERROR: failed to write reference %s\n", path.c_str());
            ok = false;
            continue;
        }
        uint64_t size = std::filesystem::file_size(path);
        std::printf("Generated %-28s %llu frames (%.2f s) -> %s (%llu bytes)\n",
                    runner.meta.name, (unsigned long long)frames,
                    static_cast<double>(frames) / kSampleRate, path.c_str(),
                    (unsigned long long)size);
    }
    if (!ok) return 1;
    std::printf("Golden reference generation complete.\n");
    return 0;
}

int runRegression() {
    std::string refDir = resolveReferenceDir();
    if (refDir.empty()) {
        std::fprintf(stderr,
                     "ERROR: no golden reference directory found. Expected one of:\n"
                     "  tests/golden-set/references\n"
                     "  ../tests/golden-set/references\n"
                     "  ../../tests/golden-set/references\n"
                     "Generate with: ./test_golden_regression --generate tests/golden-set/references\n");
        return 2;
    }

    generateFixtures();
    bool allPassed = true;

    for (const auto& runner : kRunners) {
        const std::string refPath = (std::filesystem::path(refDir) / runner.meta.refFile).string();
        if (!std::filesystem::exists(refPath)) {
            std::fprintf(stderr, "ERROR: [%s] reference file missing: %s\n",
                         runner.meta.name, refPath.c_str());
            return 2;
        }

        std::vector<float> render;
        uint64_t renderFrames = runner.renderFn(render);

        pulse::audio::DecodedAudio ref;
        if (!pulse::audio::AudioDecoder::decodeFile(refPath, ref, kSampleRate, kChannels)) {
            std::fprintf(stderr, "FAIL: [%s] reference decode failed: %s\n",
                         runner.meta.name, refPath.c_str());
            allPassed = false;
            continue;
        }
        if (ref.totalFrames == 0 || renderFrames == 0) {
            std::fprintf(stderr, "FAIL: [%s] zero-length audio (ref=%llu render=%llu)\n",
                         runner.meta.name,
                         (unsigned long long)ref.totalFrames, (unsigned long long)renderFrames);
            allPassed = false;
            continue;
        }
        if (ref.totalFrames != renderFrames) {
            std::fprintf(stderr, "FAIL: [%s] frame count mismatch: ref=%llu render=%llu\n",
                         runner.meta.name,
                         (unsigned long long)ref.totalFrames, (unsigned long long)renderFrames);
            allPassed = false;
            continue;
        }

        ComparisonMetrics m = compareBuffers(runner.meta.name, ref.samples.data(), render.data(),
                                             ref.totalFrames, kSampleRate, kChannels, 80.0f);
        // The reference is also clipped-checked: a future --generate run on a
        // clipping build would otherwise commit a clipping reference that no bar
        // ever questions (writeWav32 clamps, so it writes 'successfully').
        bool pass = (m.snrDb >= 80.0f) && (m.maxAbsDelta <= 5e-3f)
            && (m.clippedSampleCount == 0) && (m.refClippedSampleCount == 0);

        std::printf("Profile: %s\n", m.profileName.c_str());
        std::printf("  Frames:          %llu\n", (unsigned long long)m.totalFrames);
        std::printf("  SNR:             %.3f dB (bar: >= 80 dB)\n", m.snrDb);
        std::printf("  Max Delta:       %.6f (bar: <= 0.005)\n", m.maxAbsDelta);
        std::printf("  Clipped Count:   %llu / ref %llu (bars: == 0)\n",
                    (unsigned long long)m.clippedSampleCount,
                    (unsigned long long)m.refClippedSampleCount);
        std::printf("  Ref Peak:        %.3f dBFS | Cand Peak: %.3f dBFS\n", m.refPeakDbfs, m.candPeakDbfs);
        std::printf("  Result: %s%s\n", pass ? "PASS" : "FAIL",
                    (pass || m.failureReason.empty()) ? "" : (" — " + m.failureReason).c_str());
        std::printf("\n");
        if (!pass) allPassed = false;
    }

    engine().shutdown();
    std::printf(allPassed ? "Golden regression: ALL PROFILES PASS\n"
                          : "Golden regression: FAILURES DETECTED\n");
    return allPassed ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc >= 2 && std::string(argv[1]) == "--generate") {
        std::string dir = (argc >= 3) ? argv[2] : "tests/golden-set/references";
        int rc = runGenerate(dir);
        std::filesystem::remove_all(fixtureDir());
        return rc;
    }
    if (argc >= 2 && std::string(argv[1]) == "--help") {
        printUsage(argv[0]);
        return 0;
    }
    if (argc != 1) {
        printUsage(argv[0]);
        return 2;
    }
    int rc = runRegression();
    std::filesystem::remove_all(fixtureDir());
    return rc;
}
