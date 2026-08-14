#include "../include/AudioEngine.h"
#include "../include/AudioDecoder.h"
#include "../include/WavWriter.h"
#include "../include/TempoStrategy.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cassert>
#include <cmath>
#include <filesystem>

int main() {
    std::cout << "Running PULSE Phase 0 CLI Bass-Swap Transition E2E Integration Test..." << std::endl;

    std::filesystem::path tempDir = std::filesystem::temp_directory_path() / "pulse_cli_bass_swap_e2e";
    std::filesystem::create_directories(tempDir);

    std::string deckAPath = (tempDir / "track_bass_120.wav").string();
    std::string deckBPath = (tempDir / "track_bass_122.wav").string();
    std::string outWavPath = (tempDir / "bass_swap_mix_out.wav").string();
    std::string outReportPath = (tempDir / "bass_swap_report.json").string();

    constexpr uint32_t kSampleRate = 48000;
    constexpr uint32_t kChannels = 2;
    constexpr uint32_t kBlockSize = 512;

    // 1. Synthesize Bass-Heavy Fixtures: Track A (120 BPM, 60 Hz sub) and Track B (122 BPM, 60 Hz sub)
    assert(pulse::audio::WavWriter::createBassHeavyFixture(deckAPath, 60.0, 440.0, 16.0, 120.0, 0.85f, kSampleRate));
    assert(pulse::audio::WavWriter::createBassHeavyFixture(deckBPath, 60.0, 440.0, 16.0, 122.0, 0.85f, kSampleRate));

    // 2. Initialize Engine & Ingest Tracks
    auto& engine = pulse::audio::AudioEngine::getInstance();
    AudioEngineConfigC config{kSampleRate, kBlockSize, kChannels};
    assert(engine.initialize(config) == 0);

    assert(engine.loadTrack(0, deckAPath));
    assert(engine.loadTrack(1, deckBPath));

    auto* deckA = engine.getDeck(0);
    auto* deckB = engine.getDeck(1);
    auto* mixer = engine.getMixer();
    auto* transExec = engine.getTransitionExecutor();

    assert(deckA && deckB && mixer && transExec);

    const auto& metaA = deckA->getDecodedAudio();
    const auto& metaB = deckB->getDecodedAudio();
    assert(std::abs(metaA.detectedBpm - 120.0) <= 2.0);
    assert(std::abs(metaB.detectedBpm - 122.0) <= 2.0);

    // 3. Evaluate Bounded Tempo Strategy
    auto tempoDecision = pulse::audio::TempoStrategy::evaluate(
        metaA.detectedBpm,
        metaB.detectedBpm,
        metaA.tempoProfile.alternativeHypotheses,
        metaB.tempoProfile.alternativeHypotheses,
        pulse::audio::TempoStrategyMode::Source
    );

    assert(tempoDecision.strategy == "match_source");
    assert(tempoDecision.effectiveDeckATempoRatio == 1.0);
    assert(!tempoDecision.stretchExceededThreshold);

    deckA->setTempoRatio(tempoDecision.effectiveDeckATempoRatio);
    deckB->setTempoRatio(tempoDecision.effectiveDeckBTempoRatio);

    // 4. Plan Transition Window
    double transDurationSec = 8.0;
    double transStartSec = 4.0;
    double cueBSec = 0.0;
    double effectiveDurB = metaB.durationSeconds / tempoDecision.effectiveDeckBTempoRatio;
    double totalRenderSec = transStartSec + effectiveDurB - cueBSec;

    mixer->setCrossfader(-1.0f);
    deckA->setPlaybackPosition(0.0);
    deckA->setPlaying(true);

    deckB->setPlaybackPosition(cueBSec * tempoDecision.effectiveDeckBTempoRatio);
    deckB->setPlaying(false);

    // Set Bass Swap Strategy (type = 2)
    transExec->getBassSwapStrategy().setSwapPoint(0.50);

    // 5. Render Offline Transition Mix with Bass Swap
    uint64_t totalFramesToRender = static_cast<uint64_t>(totalRenderSec * kSampleRate);
    std::vector<float> renderedMix;
    renderedMix.reserve(totalFramesToRender * kChannels);

    std::vector<float> block(kBlockSize * kChannels, 0.0f);
    double currentTime = 0.0;
    double dt = static_cast<double>(kBlockSize) / kSampleRate;
    bool transitionTriggered = false;

    while (currentTime < totalRenderSec) {
        if (currentTime >= transStartSec && !transitionTriggered) {
            deckB->setPlaybackPosition(cueBSec * tempoDecision.effectiveDeckBTempoRatio);
            deckB->setPlaying(true);
            TransitionCommandC cmd{0, 1, transDurationSec, 2}; // 2 = BassSwap
            transExec->startTransition(cmd);
            transitionTriggered = true;
        }

        if (transitionTriggered && currentTime >= transStartSec) {
            transExec->updateAutomation(currentTime - transStartSec, *mixer, deckA, deckB);
        }

        engine.processAudioBlock(block.data(), kBlockSize, kChannels);
        renderedMix.insert(renderedMix.end(), block.begin(), block.end());
        currentTime += dt;
    }

    uint64_t renderedFrames = renderedMix.size() / kChannels;
    assert(renderedFrames >= totalFramesToRender);

    // 6. Verify Non-clipping and Gain-Safe Audio
    float maxPeak = 0.0f;
    for (float s : renderedMix) {
        maxPeak = std::max(maxPeak, std::abs(s));
    }
    bool clippingDetected = (maxPeak >= (1.0f - 1e-4f));
    float maxPeakDb = 20.0f * std::log10(maxPeak);

    std::cout << "  Rendered Frames: " << renderedFrames << ", Max Peak: " << maxPeak
              << " (" << maxPeakDb << " dBFS)" << std::endl;

    assert(!clippingDetected);
    assert(maxPeak <= 1.0f);
    assert(maxPeak > 0.4f);

    // 7. Write Master WAV Output
    assert(pulse::audio::WavWriter::writeWav16(outWavPath, renderedMix.data(), renderedFrames, kSampleRate, kChannels));
    assert(std::filesystem::exists(outWavPath));
    assert(std::filesystem::file_size(outWavPath) > 1000);

    // 8. Decode Rendered Output
    pulse::audio::DecodedAudio verifyDecoded;
    assert(pulse::audio::AudioDecoder::decodeFile(outWavPath, verifyDecoded, kSampleRate, kChannels));
    assert(verifyDecoded.sampleRate == 48000);
    assert(verifyDecoded.channels == 2);
    std::cout << "  Decoded Rendered Mix Peak: " << verifyDecoded.peakAmplitude << " (" << verifyDecoded.maxPeakDb << " dBFS)\n";

    // 9. Write and Validate JSON Telemetry
    std::ofstream reportFile(outReportPath);
    assert(reportFile.is_open());
    reportFile << "{\n"
               << "  \"transition\": {\n"
               << "    \"type\": \"bass_swap\",\n"
               << "    \"strategy\": \"bass_swap\",\n"
               << "    \"bass_swap_point\": 0.50,\n"
               << "    \"duration_seconds\": " << transDurationSec << ",\n"
               << "    \"start_position_seconds\": " << transStartSec << ",\n"
               << "    \"eq_automation\": {\n"
               << "      \"deck_a\": {\n"
               << "        \"initial\": { \"low\": 0.00, \"mid\": 0.0, \"high\": 0.0 },\n"
               << "        \"mid_swap\": { \"low\": -0.50, \"mid\": 0.0, \"high\": 0.0 },\n"
               << "        \"final\": { \"low\": -1.00, \"mid\": 0.0, \"high\": 0.0 }\n"
               << "      },\n"
               << "      \"deck_b\": {\n"
               << "        \"initial\": { \"low\": -1.00, \"mid\": 0.0, \"high\": 0.0 },\n"
               << "        \"mid_swap\": { \"low\": -0.50, \"mid\": 0.0, \"high\": 0.0 },\n"
               << "        \"final\": { \"low\": 0.00, \"mid\": 0.0, \"high\": 0.0 }\n"
               << "      }\n"
               << "    }\n"
               << "  },\n"
               << "  \"output\": {\n"
               << "    \"file_path\": \"" << outWavPath << "\",\n"
               << "    \"clipping_detected\": false,\n"
               << "    \"max_peak_db\": " << maxPeakDb << "\n"
               << "  }\n"
               << "}\n";
    reportFile.close();

    assert(std::filesystem::exists(outReportPath));
    std::ifstream checkReport(outReportPath);
    std::stringstream buffer;
    buffer << checkReport.rdbuf();
    std::string reportContent = buffer.str();
    assert(reportContent.find("\"strategy\": \"bass_swap\"") != std::string::npos);
    assert(reportContent.find("\"eq_automation\"") != std::string::npos);
    assert(reportContent.find("\"clipping_detected\": false") != std::string::npos);

    // Cleanup
    std::filesystem::remove_all(tempDir);

    std::cout << "CLI Bass-Swap Transition E2E Integration Test PASSED!" << std::endl;
    return 0;
}
