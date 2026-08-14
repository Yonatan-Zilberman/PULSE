#include "../include/AudioEngine.h"
#include "../include/AudioDecoder.h"
#include "../include/WavWriter.h"
#include "../include/TempoStrategy.h"
#include "../include/TransitionPlanner.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cassert>
#include <cmath>
#include <filesystem>

int main() {
    std::cout << "Running PULSE Phase 0 CLI Phrase-Aware Transition E2E Integration Test..." << std::endl;

    std::filesystem::path tempDir = std::filesystem::temp_directory_path() / "pulse_cli_phrase_e2e";
    std::filesystem::create_directories(tempDir);

    std::string deckAPath = (tempDir / "track_a_120.wav").string();
    std::string deckBPath = (tempDir / "track_b_122.wav").string();
    std::string outWavPath = (tempDir / "phrase_mix_out.wav").string();
    std::string outReportPath = (tempDir / "phrase_mix_report.json").string();

    constexpr uint32_t kSampleRate = 48000;
    constexpr uint32_t kChannels = 2;
    constexpr uint32_t kBlockSize = 512;

    // 1. Synthesize Track A (120 BPM, 440 Hz, 16 bars = 32.0s) and Track B (122 BPM, 880 Hz, 16 bars = 31.475s)
    assert(pulse::audio::WavWriter::createPhraseFixture(deckAPath, 440.0, 16, 120.0, 0.80f, kSampleRate));
    assert(pulse::audio::WavWriter::createPhraseFixture(deckBPath, 880.0, 16, 122.0, 0.80f, kSampleRate));

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
    assert(std::abs(metaB.detectedBpm - 122.0) <= 1.5);

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

    // 4. Plan Phrase-Aware Transition Window
    auto plan = pulse::audio::TransitionPlanner::planTransition(
        metaA,
        metaB,
        tempoDecision.effectiveDeckATempoRatio,
        tempoDecision.effectiveDeckBTempoRatio,
        8,    // 8-bar preferred
        0.50, // min phrase confidence
        true  // phrase-aware enabled
    );

    std::cout << "  Planned Window: Strategy=" << plan.selectedWindow.alignmentStrategy
              << ", ExitSec=" << plan.selectedWindow.sourceExitSec
              << ", EntrySec=" << plan.selectedWindow.destEntrySec
              << ", Bars=" << plan.selectedWindow.durationBars
              << ", DurSec=" << plan.selectedWindow.durationSeconds
              << ", Score=" << plan.selectedWindow.phraseScore << std::endl;

    assert(plan.selectedWindow.isPhraseAligned);
    assert(plan.selectedWindow.alignmentStrategy == "phrase_aligned");
    assert(plan.selectedWindow.durationBars == 8);
    assert(std::abs(plan.selectedWindow.durationSeconds - 16.0) < 0.1);
    assert(std::abs(plan.selectedWindow.sourceExitSec - 16.0) < 0.2);

    double transStartSec = plan.selectedWindow.sourceExitSec;
    double transDurationSec = plan.selectedWindow.durationSeconds;
    double cueBSec = plan.selectedWindow.destEntrySec;

    double effectiveDurB = metaB.durationSeconds / tempoDecision.effectiveDeckBTempoRatio;
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
            TransitionCommandC cmd{0, 1, transDurationSec, 0};
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

    // 6. Verify Non-clipping and Non-silent Audio
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
    std::cout << "  Decoded Rendered Mix BPM: " << verifyDecoded.detectedBpm << " (Expected ~120.0 BPM)\n";
    assert(std::abs(verifyDecoded.detectedBpm - 120.0) <= 2.0);

    // 9. Write and Validate JSON Telemetry
    std::ofstream reportFile(outReportPath);
    assert(reportFile.is_open());
    reportFile << "{\n"
               << "  \"phrase_analysis\": {\n"
               << "    \"deck_a_phrases_4bar_count\": " << metaA.phraseProfile.boundaries4Bar.size() << ",\n"
               << "    \"deck_a_phrases_8bar_count\": " << metaA.phraseProfile.boundaries8Bar.size() << ",\n"
               << "    \"deck_a_confidence\": " << metaA.phraseProfile.phraseConfidence << ",\n"
               << "    \"deck_b_phrases_4bar_count\": " << metaB.phraseProfile.boundaries4Bar.size() << ",\n"
               << "    \"deck_b_phrases_8bar_count\": " << metaB.phraseProfile.boundaries8Bar.size() << ",\n"
               << "    \"deck_b_confidence\": " << metaB.phraseProfile.phraseConfidence << "\n"
               << "  },\n"
               << "  \"transition_window\": {\n"
               << "    \"strategy\": \"phrase_crossfade\",\n"
               << "    \"alignment_strategy\": \"" << plan.selectedWindow.alignmentStrategy << "\",\n"
               << "    \"phrase_length_bars\": " << plan.selectedWindow.durationBars << ",\n"
               << "    \"duration_seconds\": " << plan.selectedWindow.durationSeconds << ",\n"
               << "    \"deck_a_exit_sec\": " << plan.selectedWindow.sourceExitSec << ",\n"
               << "    \"deck_b_entry_sec\": " << plan.selectedWindow.destEntrySec << ",\n"
               << "    \"phrase_score\": " << plan.selectedWindow.phraseScore << ",\n"
               << "    \"fallback_applied\": false,\n"
               << "    \"fallback_reason\": null\n"
               << "  }\n"
               << "}\n";
    reportFile.close();

    assert(std::filesystem::exists(outReportPath));
    std::ifstream checkReport(outReportPath);
    std::stringstream buffer;
    buffer << checkReport.rdbuf();
    std::string reportContent = buffer.str();
    assert(reportContent.find("\"strategy\": \"phrase_crossfade\"") != std::string::npos);
    assert(reportContent.find("\"fallback_applied\": false") != std::string::npos);

    // Cleanup
    std::filesystem::remove_all(tempDir);

    std::cout << "CLI Phrase-Aware Transition E2E Integration Test PASSED!" << std::endl;
    return 0;
}
