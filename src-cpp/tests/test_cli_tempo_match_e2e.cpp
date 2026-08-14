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
    std::cout << "Running PULSE Phase 0 CLI Tempo-Matching E2E Integration Test..." << std::endl;

    std::filesystem::path tempDir = std::filesystem::temp_directory_path() / "pulse_cli_tempo_e2e";
    std::filesystem::create_directories(tempDir);

    std::string deckAPath = (tempDir / "track_a_120.wav").string();
    std::string deckBPath = (tempDir / "track_b_125.wav").string();
    std::string outWavPath = (tempDir / "tempo_match_out.wav").string();
    std::string outReportPath = (tempDir / "tempo_match_report.json").string();

    constexpr uint32_t kSampleRate = 48000;
    constexpr uint32_t kChannels = 2;
    constexpr uint32_t kBlockSize = 512;

    // 1. Synthesize Track A (120 BPM, 440 Hz, 8.0s) and Track B (125 BPM, 880 Hz, 8.0s)
    bool createdA = pulse::audio::WavWriter::createTempoTestFixture(deckAPath, 440.0, 8.0, 120.0, 0.80f, kSampleRate);
    bool createdB = pulse::audio::WavWriter::createTempoTestFixture(deckBPath, 880.0, 8.0, 125.0, 0.80f, kSampleRate);
    assert(createdA && createdB);

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
    assert(std::abs(metaA.detectedBpm - 120.0) <= 1.5);
    assert(std::abs(metaB.detectedBpm - 125.0) <= 1.5);

    // 3. Evaluate Bounded Tempo Strategy
    auto tempoDecision = pulse::audio::TempoStrategy::evaluate(
        metaA.detectedBpm,
        metaB.detectedBpm,
        metaA.tempoProfile.alternativeHypotheses,
        metaB.tempoProfile.alternativeHypotheses,
        pulse::audio::TempoStrategyMode::Source
    );

    std::cout << "  Evaluated Strategy: " << tempoDecision.strategy
              << ", Deck B Tempo Ratio = " << tempoDecision.effectiveDeckBTempoRatio
              << ", Delta = " << tempoDecision.bpmDeltaPercent << "%\n";

    assert(tempoDecision.strategy == "match_source");
    assert(tempoDecision.effectiveDeckATempoRatio == 1.0);
    assert(std::abs(tempoDecision.effectiveDeckBTempoRatio - (120.0 / 125.0)) < 0.02);
    assert(tempoDecision.pitchSemitoneShift == 0.0f);
    assert(!tempoDecision.stretchExceededThreshold);

    deckA->setTempoRatio(tempoDecision.effectiveDeckATempoRatio);
    deckB->setTempoRatio(tempoDecision.effectiveDeckBTempoRatio);

    // 4. Configure Beat Alignment & Transition Start
    double transDuration = 4.0;
    double effectiveDurA = metaA.durationSeconds / tempoDecision.effectiveDeckATempoRatio;
    double effectiveDurB = metaB.durationSeconds / tempoDecision.effectiveDeckBTempoRatio;
    double rawTransStart = effectiveDurA - transDuration;

    double transStartSec = rawTransStart;
    if (!metaA.tempoProfile.downbeatPositions.empty()) {
        for (double db : metaA.tempoProfile.downbeatPositions) {
            double scaledDb = db / tempoDecision.effectiveDeckATempoRatio;
            if (scaledDb <= rawTransStart) {
                transStartSec = scaledDb;
            }
        }
    }

    double cueBSec = 0.0;
    if (!metaB.tempoProfile.downbeatPositions.empty()) {
        cueBSec = metaB.tempoProfile.downbeatPositions.front() / tempoDecision.effectiveDeckBTempoRatio;
    }

    double totalRenderSec = transStartSec + effectiveDurB - cueBSec;

    mixer->setCrossfader(-1.0f);
    deckA->setPlaybackPosition(0.0);
    deckA->setPlaying(true);

    deckB->setPlaybackPosition(cueBSec * tempoDecision.effectiveDeckBTempoRatio);
    deckB->setPlaying(false);

    // 5. Render Offline Transition Mix
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
            TransitionCommandC cmd{0, 1, transDuration, 1};
            transExec->startTransition(cmd);
            transitionTriggered = true;
        }

        if (transitionTriggered && currentTime >= transStartSec) {
            transExec->updateAutomation(currentTime - transStartSec, *mixer);
        }

        engine.processAudioBlock(block.data(), kBlockSize, kChannels);
        renderedMix.insert(renderedMix.end(), block.begin(), block.end());
        currentTime += dt;
    }

    uint64_t renderedFrames = renderedMix.size() / kChannels;
    assert(renderedFrames >= totalFramesToRender);

    // 6. Verify Non-clipping Audio
    float maxPeak = 0.0f;
    for (float s : renderedMix) {
        maxPeak = std::max(maxPeak, std::abs(s));
    }
    std::cout << "  Rendered Frames: " << renderedFrames << ", Max Peak: " << maxPeak << std::endl;
    assert(maxPeak > 0.4f && maxPeak <= 1.0f);

    // 7. Write Master WAV Output
    assert(pulse::audio::WavWriter::writeWav16(outWavPath, renderedMix.data(), renderedFrames, kSampleRate, kChannels));
    assert(std::filesystem::exists(outWavPath));
    assert(std::filesystem::file_size(outWavPath) > 1000);

    // 8. Decode Rendered Output and Verify Beat Tracking Stability
    pulse::audio::DecodedAudio verifyDecoded;
    assert(pulse::audio::AudioDecoder::decodeFile(outWavPath, verifyDecoded, kSampleRate, kChannels));
    assert(verifyDecoded.sampleRate == 48000);
    assert(verifyDecoded.channels == 2);
    // Tempo across the whole rendered transition must cleanly match 120 BPM
    std::cout << "  Decoded Rendered Mix BPM: " << verifyDecoded.detectedBpm << " (Expected ~120.0 BPM)\n";
    assert(std::abs(verifyDecoded.detectedBpm - 120.0) <= 2.0);

    // 9. Write JSON Telemetry
    std::ofstream reportFile(outReportPath);
    assert(reportFile.is_open());
    reportFile << "{\n"
               << "  \"tempo_matching\": {\n"
               << "    \"strategy\": \"" << tempoDecision.strategy << "\",\n"
               << "    \"source_native_bpm\": " << tempoDecision.sourceNativeBpm << ",\n"
               << "    \"destination_native_bpm\": " << tempoDecision.destNativeBpm << ",\n"
               << "    \"effective_tempo_ratio\": " << tempoDecision.effectiveTempoRatio << ",\n"
               << "    \"bpm_delta_percent\": " << tempoDecision.bpmDeltaPercent << ",\n"
               << "    \"pitch_semitone_shift\": 0.0,\n"
               << "    \"octave_jump_applied\": " << (tempoDecision.octaveJumpApplied ? "true" : "false") << ",\n"
               << "    \"artifact_risk_score\": " << tempoDecision.artifactRiskScore << ",\n"
               << "    \"stretch_exceeded_threshold\": " << (tempoDecision.stretchExceededThreshold ? "true" : "false") << "\n"
               << "  }\n"
               << "}\n";
    reportFile.close();

    // Verify report written
    assert(std::filesystem::exists(outReportPath));
    std::ifstream checkReport(outReportPath);
    std::stringstream buffer;
    buffer << checkReport.rdbuf();
    std::string reportContent = buffer.str();
    assert(reportContent.find("\"strategy\": \"match_source\"") != std::string::npos);
    assert(reportContent.find("\"pitch_semitone_shift\": 0.0") != std::string::npos);

    // Cleanup
    std::filesystem::remove_all(tempDir);

    std::cout << "CLI Tempo-Matching E2E Integration Test PASSED!" << std::endl;
    return 0;
}
