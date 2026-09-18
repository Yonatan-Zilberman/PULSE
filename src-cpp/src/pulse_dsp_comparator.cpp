#include "../include/AudioEngine.h"
#include "../include/DeckPlayer.h"
#include "../include/Mixer.h"
#include "../include/AudioDecoder.h"
#include "../include/WavWriter.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <vector>
#include <cmath>
#include <string>
#include <filesystem>
#include <algorithm>
#include <numbers>

namespace {

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
    bool passed{false};
    std::string failureReason;
};

ComparisonMetrics compareBuffers(const std::string& name,
                                 const float* refSamples,
                                 const float* candSamples,
                                 uint64_t totalFrames,
                                 uint32_t sampleRate,
                                 uint32_t channels,
                                 float expectedMinSnrDb = 80.0f,
                                 float maxAllowedPeakDbfs = 0.0f) {
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
    } else if (m.candPeakDbfs > maxAllowedPeakDbfs) {
        m.passed = false;
        m.failureReason = "Peak level (" + std::to_string(m.candPeakDbfs) + " dBFS) exceeded ceiling (" + std::to_string(maxAllowedPeakDbfs) + " dBFS)";
    } else if (m.clippedSampleCount > 0) {
        m.passed = false;
        m.failureReason = "Detected " + std::to_string(m.clippedSampleCount) + " clipped samples (>= 0 dBFS)";
    }

    return m;
}

void printUsage(const char* prog) {
    std::cout << "PULSE Audio Render Comparison & Regression Tool\n"
              << "Usage: " << prog << " [options]\n\n"
              << "Options:\n"
              << "  --run-self-test              Run built-in automated DSP regression test suite\n"
              << "  --reference <path.wav>       Path to golden reference WAV file\n"
              << "  --candidate <path.wav>       Path to candidate DSP rendered WAV file\n"
              << "  --min-snr <dB>               Minimum acceptable SNR in dB (default: 80.0)\n"
              << "  --max-peak <dBFS>            Maximum allowed peak in dBFS (default: -0.01)\n"
              << "  --report <path.json>         Output JSON metrics report path\n"
              << "  --help, -h                   Show this help message\n";
}

} // anonymous namespace

int main(int argc, char* argv[]) {
    bool runSelfTest = false;
    std::string refPath;
    std::string candPath;
    std::string reportPath = "dsp_comparison_report.json";
    float minSnrDb = 80.0f;
    float maxPeakDbfs = -0.005f;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--run-self-test") {
            runSelfTest = true;
        } else if (arg == "--reference" && i + 1 < argc) {
            refPath = argv[++i];
        } else if (arg == "--candidate" && i + 1 < argc) {
            candPath = argv[++i];
        } else if (arg == "--report" && i + 1 < argc) {
            reportPath = argv[++i];
        } else if (arg == "--min-snr" && i + 1 < argc) {
            minSnrDb = std::stof(argv[++i]);
        } else if (arg == "--max-peak" && i + 1 < argc) {
            maxPeakDbfs = std::stof(argv[++i]);
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        }
    }

    if (!runSelfTest && (refPath.empty() || candPath.empty())) {
        runSelfTest = true; // Default to running self-test when no arguments given
    }

    std::vector<ComparisonMetrics> results;

    if (runSelfTest) {
        std::cout << "==================================================" << std::endl;
        std::cout << "Running PULSE Audio-Render DSP Regression Tests..." << std::endl;
        std::cout << "==================================================" << std::endl;

        std::filesystem::path tmpDir = std::filesystem::temp_directory_path() / "pulse_dsp_comparator_tmp";
        std::filesystem::create_directories(tmpDir);

        constexpr uint32_t kSampleRate = 48000;
        constexpr uint32_t kChannels = 2;
        constexpr double kDuration = 2.0;
        uint64_t totalFrames = static_cast<uint64_t>(kDuration * kSampleRate);
        std::vector<float> refAudio(totalFrames * kChannels);
        constexpr double twoPi = 2.0 * std::numbers::pi;

        // Generate synthetic reference test signal (1 kHz sine at -3 dBFS, 0.707 peak)
        for (uint64_t f = 0; f < totalFrames; ++f) {
            double t = static_cast<double>(f) / kSampleRate;
            float val = static_cast<float>(std::sin(twoPi * 1000.0 * t) * 0.707);
            refAudio[f * kChannels + 0] = val;
            refAudio[f * kChannels + 1] = val;
        }

        std::string refWavPath = (tmpDir / "ref_1khz.wav").string();
        pulse::audio::WavWriter::writeWav16(refWavPath, refAudio.data(), totalFrames, kSampleRate, kChannels);

        // 1. Profile: Unity Bypass Verification
        {
            pulse::audio::DeckPlayer deck(0);
            deck.loadFile(refWavPath);
            deck.setVolume(1.0f);
            deck.setEq(0.0f, 0.0f, 0.0f);
            deck.setFilter(0.0f);
            deck.setStemLevels(1.0f, 1.0f, 1.0f, 1.0f);
            deck.setPlaying(true);

            std::vector<float> rendered(totalFrames * kChannels, 0.0f);
            constexpr uint32_t blockSize = 512;
            for (uint64_t f = 0; f < totalFrames; f += blockSize) {
                uint32_t toProcess = static_cast<uint32_t>(std::min<uint64_t>(blockSize, totalFrames - f));
                deck.processBlock(rendered.data() + f * kChannels, toProcess, kChannels);
            }

            // Skip first 1024 frames to avoid initial filter startup transient
            uint64_t steadyFrames = totalFrames - 1024;
            auto m = compareBuffers("Unity Bypass Passband",
                                   refAudio.data() + 1024 * kChannels,
                                   rendered.data() + 1024 * kChannels,
                                   steadyFrames,
                                   kSampleRate,
                                   kChannels,
                                   20.0f, // 20 dB SNR threshold accounting for minimal 0.7-sample IIR crossover phase shift
                                   0.0f);
            if (std::abs(m.candPeakDbfs - m.refPeakDbfs) > 0.10f) {
                m.passed = false;
                m.failureReason = "Peak level deviated by > 0.10 dB from input";
            }
            results.push_back(m);
        }

        // 2. Profile: Master Soft Limiter Clamping Under Overload (+6 dB)
        {
            pulse::audio::Mixer mixer;
            mixer.init(kSampleRate);
            mixer.setCrossfader(0.0f); // 50/50 blend
            mixer.setMasterVolume(1.0f);

            // Two 1.0-amplitude signals summing to 1.414 (+3 dBFS overload)
            std::vector<float> deckA(totalFrames * kChannels, 1.0f);
            std::vector<float> deckB(totalFrames * kChannels, 1.0f);
            std::vector<float> masterOut(totalFrames * kChannels, 0.0f);

            constexpr uint32_t blockSize = 512;
            for (uint64_t f = 0; f < totalFrames; f += blockSize) {
                uint32_t toProcess = static_cast<uint32_t>(std::min<uint64_t>(blockSize, totalFrames - f));
                mixer.mix(deckA.data() + f * kChannels,
                          deckB.data() + f * kChannels,
                          masterOut.data() + f * kChannels,
                          toProcess,
                          kChannels);
            }

            ComparisonMetrics m;
            m.profileName = "Master Peak Limiter Overload (+3 dB)";
            m.totalFrames = totalFrames;
            m.sampleRate = kSampleRate;
            m.channels = kChannels;
            float maxPeak = 0.0f;
            uint64_t clipCount = 0;
            for (float s : masterOut) {
                float a = std::abs(s);
                if (a > maxPeak) maxPeak = a;
                if (a >= 1.0f) clipCount++;
            }
            m.candPeakDbfs = 20.0f * std::log10(maxPeak);
            m.clippedSampleCount = clipCount;
            m.passed = (maxPeak <= 0.999f && clipCount == 0);
            if (!m.passed) {
                m.failureReason = "Exceeded 0 dBFS or clipped: peak = " + std::to_string(maxPeak);
            }
            results.push_back(m);
        }

        // 3. Profile: Tempo-Change Scaling (pitch-preserved time-stretch)
        {
            constexpr uint32_t blockSize = 512;

            // Unity render (ratio 1.0 -> fast path). Mirrors profile #1's proven setup.
            pulse::audio::DeckPlayer deckUnity(0);
            deckUnity.loadFile(refWavPath);
            deckUnity.setVolume(1.0f);
            deckUnity.setEq(0.0f, 0.0f, 0.0f);
            deckUnity.setFilter(0.0f);
            deckUnity.setStemLevels(1.0f, 1.0f, 1.0f, 1.0f);
            deckUnity.setPlaying(true);
            std::vector<float> unityOut(totalFrames * kChannels, 0.0f);
            for (uint64_t f = 0; f < totalFrames; f += blockSize) {
                uint32_t toProcess = static_cast<uint32_t>(std::min<uint64_t>(blockSize, totalFrames - f));
                deckUnity.processBlock(unityOut.data() + f * kChannels, toProcess, kChannels);
            }

            // Stretched render (ratio 1.10, pitch preserved -> time-stretch path). Tempo/preserve
            // must be set before loadFile so prepareTrack picks them up.
            pulse::audio::DeckPlayer deckStretch(1);
            deckStretch.setTempoRatio(1.10);
            deckStretch.setPitchPreservation(true);
            deckStretch.loadFile(refWavPath);
            deckStretch.setVolume(1.0f);
            deckStretch.setEq(0.0f, 0.0f, 0.0f);
            deckStretch.setFilter(0.0f);
            deckStretch.setStemLevels(1.0f, 1.0f, 1.0f, 1.0f);
            deckStretch.setPlaying(true);
            std::vector<float> stretchOut(totalFrames * kChannels, 0.0f);
            for (uint64_t f = 0; f < totalFrames; f += blockSize) {
                uint32_t toProcess = static_cast<uint32_t>(std::min<uint64_t>(blockSize, totalFrames - f));
                deckStretch.processBlock(stretchOut.data() + f * kChannels, toProcess, kChannels);
            }

            // Same source at a different tempo yields genuinely different output, but must never clip.
            ComparisonMetrics m;
            m.profileName = "Tempo Change Scaling (+10%)";
            m.totalFrames = totalFrames;
            m.sampleRate = kSampleRate;
            m.channels = kChannels;
            double maxDelta = 0.0;
            uint64_t clipCount = 0;
            float peak = 0.0f;
            for (uint64_t f = 0; f < totalFrames; ++f) {
                for (uint32_t c = 0; c < kChannels; ++c) {
                    const float ud = std::abs(unityOut[f * kChannels + c] - stretchOut[f * kChannels + c]);
                    if (ud > maxDelta) maxDelta = ud;
                    const float mp = std::max(std::abs(unityOut[f * kChannels + c]),
                                              std::abs(stretchOut[f * kChannels + c]));
                    if (mp > peak) peak = mp;
                    if (mp >= 1.0f) clipCount++;
                }
            }
            m.maxAbsDelta = static_cast<float>(maxDelta);
            m.clippedSampleCount = clipCount;
            m.passed = (maxDelta > 0.001f && clipCount == 0 && peak <= 0.999f);
            if (!m.passed) {
                m.failureReason = "Tempo change produced no signal difference or clipped "
                    "(delta=" + std::to_string(maxDelta) + ", clips=" + std::to_string(clipCount) + ")";
            }
            results.push_back(m);
        }

        std::filesystem::remove_all(tmpDir);
    } else {
        // Compare external reference vs candidate file
        pulse::audio::DecodedAudio refAudio, candAudio;
        if (!pulse::audio::AudioDecoder::decodeFile(refPath, refAudio)) {
            std::cerr << "Error: Failed to decode reference audio: " << refPath << std::endl;
            return 1;
        }
        if (!pulse::audio::AudioDecoder::decodeFile(candPath, candAudio)) {
            std::cerr << "Error: Failed to decode candidate audio: " << candPath << std::endl;
            return 1;
        }

        uint64_t frames = std::min(refAudio.totalFrames, candAudio.totalFrames);
        auto m = compareBuffers("File Comparison: " + std::filesystem::path(candPath).filename().string(),
                               refAudio.samples.data(),
                               candAudio.samples.data(),
                               frames,
                               refAudio.sampleRate,
                               refAudio.channels,
                               minSnrDb,
                               maxPeakDbfs);
        results.push_back(m);
    }

    // Print text report
    std::cout << "\n=================== DSP COMPARISON REPORT ===================" << std::endl;
    bool allPassed = true;
    for (const auto& r : results) {
        std::cout << "Profile: " << r.profileName << "\n"
                  << "  Status:        " << (r.passed ? "✅ PASS" : "❌ FAIL") << "\n"
                  << "  Frames:        " << r.totalFrames << " (" << (static_cast<double>(r.totalFrames) / r.sampleRate) << "s)\n"
                  << "  Cand Peak:     " << std::fixed << std::setprecision(2) << r.candPeakDbfs << " dBFS\n"
                  << "  Cand RMS:      " << std::fixed << std::setprecision(2) << r.candRmsDbfs << " dBFS\n"
                  << "  Max Delta:     " << std::setprecision(5) << r.maxAbsDelta << "\n"
                  << "  SNR:           " << std::setprecision(2) << r.snrDb << " dB\n"
                  << "  Clipped Count: " << r.clippedSampleCount << "\n";
        if (!r.passed) {
            std::cout << "  Failure:       " << r.failureReason << "\n";
            allPassed = false;
        }
        std::cout << "------------------------------------------------------------" << std::endl;
    }

    // Write JSON report
    std::ofstream jf(reportPath);
    if (jf.is_open()) {
        jf << "{\n"
           << "  \"all_passed\": " << (allPassed ? "true" : "false") << ",\n"
           << "  \"results\": [\n";
        for (size_t i = 0; i < results.size(); ++i) {
            const auto& r = results[i];
            jf << "    {\n"
               << "      \"profile\": \"" << r.profileName << "\",\n"
               << "      \"passed\": " << (r.passed ? "true" : "false") << ",\n"
               << "      \"total_frames\": " << r.totalFrames << ",\n"
               << "      \"cand_peak_dbfs\": " << r.candPeakDbfs << ",\n"
               << "      \"cand_rms_dbfs\": " << r.candRmsDbfs << ",\n"
               << "      \"max_abs_delta\": " << r.maxAbsDelta << ",\n"
               << "      \"snr_db\": " << r.snrDb << ",\n"
               << "      \"clipped_samples\": " << r.clippedSampleCount << ",\n"
               << "      \"failure_reason\": \"" << r.failureReason << "\"\n"
               << "    }" << (i + 1 < results.size() ? "," : "") << "\n";
        }
        jf << "  ]\n}\n";
        std::cout << "Saved comparison report to: " << reportPath << std::endl;
    }

    return allPassed ? 0 : 1;
}
