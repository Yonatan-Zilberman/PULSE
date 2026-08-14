#include "../include/AudioEngine.h"
#include "../include/AudioDecoder.h"
#include "../include/WavWriter.h"
#include "../include/TempoStrategy.h"
#include "../include/TransitionPlanner.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

namespace {

void printUsage(const char* progName) {
    std::cout << "PULSE Phase 0 CLI Audio-Engine Prototype\n"
              << "Usage: " << progName << " [options]\n\n"
              << "Options:\n"
              << "  --deck-a <path>             Source track A audio file (WAV/AIFF/MP3/M4A)\n"
              << "  --deck-b <path>             Destination track B audio file (WAV/AIFF/MP3/M4A)\n"
              << "  --transition-sec <sec>      Transition duration in seconds (default: 4.0)\n"
              << "  --transition-time <sec>     Alias for --transition-sec\n"
              << "  --transition-start <sec>    Mix start timestamp in seconds (default: durationA - transition_sec)\n"
              << "  --phrase-aware              Enable musical phrase-aware window planning (default: enabled)\n"
              << "  --no-phrase-aware           Disable musical phrase alignment (fallback to bar/beat snap)\n"
              << "  --phrase-bars <N>           Preferred phrase length in bars: 2, 4, 8, 16, 32, auto (default: 8)\n"
              << "  --phrase-length-bars <N>    Alias for --phrase-bars\n"
              << "  --transition-strategy <name> Transition strategy name (default: phrase_crossfade)\n"
              << "  --min-phrase-confidence <f>  Minimum phrase confidence required [0.0 - 1.0] (default: 0.5)\n"
              << "  --align-beats               Enable musical beat/downbeat alignment (default: enabled)\n"
              << "  --no-align-beats            Disable musical beat alignment\n"
              << "  --snap-to-bar               Snap transition start to closest bar downbeat (default: enabled)\n"
              << "  --no-snap-to-bar            Disable downbeat bar snapping (snap to beat instead)\n"
              << "  --tempo-strategy <mode>     Tempo strategy: source (default), dest, master, none, auto\n"
              << "  --tempo-target <bpm>        Target BPM for master tempo strategy\n"
              << "  --target-bpm <bpm>          Alias for --tempo-target\n"
              << "  --max-stretch-pct <pct>     Max allowable tempo stretch percentage before rejection (default: 6.0)\n"
              << "  --force-stretch             Override extreme stretch rejection for testing\n"
              << "  --duration <sec>            Total rendered mix duration (default: auto)\n"
              << "  --out <path.wav>            Output rendered WAV path (default: output_mix.wav)\n"
              << "  --out-audio <path.wav>      Alias for --out\n"
              << "  --report <path.json>        Output JSON telemetry path (default: output_report.json)\n"
              << "  --out-json <path.json>      Alias for --report\n"
              << "  --help, -h                  Show this help message\n";
}

std::string formatJsonString(const std::string& input) {
    std::string out;
    for (char c : input) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else out += c;
    }
    return out;
}

std::string formatDoubleArrayJson(const std::vector<double>& arr) {
    std::ostringstream ss;
    ss << "[";
    for (size_t i = 0; i < arr.size(); ++i) {
        if (i > 0) ss << ", ";
        ss << std::fixed << std::setprecision(1) << arr[i];
    }
    ss << "]";
    return ss.str();
}

} // anonymous namespace

int main(int argc, char* argv[]) {
    std::string deckAPath;
    std::string deckBPath;
    std::string outAudioPath = "output_mix.wav";
    std::string outReportPath = "output_report.json";
    double transitionDuration = 4.0;
    bool customTransDurationGiven = false;
    double customTransitionStart = -1.0;
    double customTotalDuration = -1.0;
    bool alignBeats = true;
    bool snapToBar = true;

    // Phrase-Aware Options
    bool phraseAware = true;
    uint32_t phraseBars = 8;
    std::string transitionStrategy = "phrase_crossfade";
    double minPhraseConfidence = 0.5;

    // Tempo Strategy Options
    std::string tempoStrategyStr = "source";
    double targetMasterBpm = 0.0;
    double maxStretchPct = 6.0;
    bool forceStretch = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--deck-a" && i + 1 < argc) {
            deckAPath = argv[++i];
        } else if (arg == "--deck-b" && i + 1 < argc) {
            deckBPath = argv[++i];
        } else if ((arg == "--transition-sec" || arg == "--transition-time") && i + 1 < argc) {
            transitionDuration = std::stod(argv[++i]);
            customTransDurationGiven = true;
        } else if (arg == "--transition-start" && i + 1 < argc) {
            customTransitionStart = std::stod(argv[++i]);
        } else if (arg == "--phrase-aware") {
            phraseAware = true;
        } else if (arg == "--no-phrase-aware") {
            phraseAware = false;
        } else if ((arg == "--phrase-bars" || arg == "--phrase-length-bars") && i + 1 < argc) {
            std::string pBars = argv[++i];
            if (pBars == "auto") {
                phraseBars = 0;
            } else {
                phraseBars = static_cast<uint32_t>(std::stoul(pBars));
            }
        } else if (arg == "--transition-strategy" && i + 1 < argc) {
            transitionStrategy = argv[++i];
        } else if (arg == "--min-phrase-confidence" && i + 1 < argc) {
            minPhraseConfidence = std::stod(argv[++i]);
        } else if (arg == "--duration" && i + 1 < argc) {
            customTotalDuration = std::stod(argv[++i]);
        } else if (arg == "--align-beats" || arg == "--beat-align") {
            alignBeats = true;
        } else if (arg == "--no-align-beats") {
            alignBeats = false;
        } else if (arg == "--snap-to-bar") {
            snapToBar = true;
        } else if (arg == "--no-snap-to-bar") {
            snapToBar = false;
        } else if (arg == "--tempo-strategy" && i + 1 < argc) {
            tempoStrategyStr = argv[++i];
        } else if ((arg == "--tempo-target" || arg == "--target-bpm") && i + 1 < argc) {
            targetMasterBpm = std::stod(argv[++i]);
        } else if (arg == "--max-stretch-pct" && i + 1 < argc) {
            maxStretchPct = std::stod(argv[++i]);
        } else if (arg == "--force-stretch") {
            forceStretch = true;
        } else if ((arg == "--out" || arg == "--out-audio") && i + 1 < argc) {
            outAudioPath = argv[++i];
        } else if ((arg == "--report" || arg == "--out-json") && i + 1 < argc) {
            outReportPath = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "Error: Unrecognized option '" << arg << "'\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    if (deckAPath.empty() || deckBPath.empty()) {
        std::cerr << "Error: Both --deck-a and --deck-b paths are required.\n";
        printUsage(argv[0]);
        return 1;
    }

    std::cout << "================================================================\n"
              << "🎧 PULSE Phase 0 CLI Audio-Engine Prototype\n"
              << "================================================================\n"
              << "Deck A Input:        " << deckAPath << "\n"
              << "Deck B Input:        " << deckBPath << "\n"
              << "Phrase Planning:     " << (phraseAware ? "Phrase-Aware (Preferred: " + std::to_string(phraseBars) + " bars)" : "Disabled") << "\n"
              << "Transition Strategy: " << transitionStrategy << "\n"
              << "Beat Alignment:      " << (alignBeats ? (snapToBar ? "Downbeat Bar Snap" : "Beat Snap") : "Disabled") << "\n"
              << "Tempo Strategy:      " << tempoStrategyStr << " (Max Stretch: " << maxStretchPct << "%)\n"
              << "Output Audio:        " << outAudioPath << "\n"
              << "Output Report:       " << outReportPath << "\n"
              << "----------------------------------------------------------------\n";

    // 1. Initialize Audio Engine
    auto& engine = pulse::audio::AudioEngine::getInstance();
    AudioEngineConfigC config{48000, 512, 2};
    if (engine.initialize(config) != 0) {
        std::cerr << "Error: Failed to initialize PULSE Audio Engine.\n";
        return 1;
    }

    // 2. Ingest and decode Deck A
    std::cout << "Ingesting Deck A..." << std::endl;
    if (!engine.loadTrack(0, deckAPath)) {
        std::cerr << "Error: Failed to decode audio file for Deck A: " << deckAPath << "\n";
        return 1;
    }
    auto* deckA = engine.getDeck(0);
    const auto& metaA = deckA->getDecodedAudio();
    std::cout << "  Deck A: " << metaA.durationSeconds << "s | "
              << metaA.sampleRate << "Hz | " << metaA.channels << "ch | "
              << "Peak: " << metaA.peakAmplitude << " (" << metaA.maxPeakDb << " dB) | "
              << "BPM: " << metaA.detectedBpm << " (Conf: " << metaA.tempoProfile.confidence << ") | "
              << "Phrases: 4-bar=" << metaA.phraseProfile.boundaries4Bar.size()
              << ", 8-bar=" << metaA.phraseProfile.boundaries8Bar.size()
              << " (PhraseConf: " << metaA.phraseProfile.phraseConfidence << ")\n";

    // 3. Ingest and decode Deck B
    std::cout << "Ingesting Deck B..." << std::endl;
    if (!engine.loadTrack(1, deckBPath)) {
        std::cerr << "Error: Failed to decode audio file for Deck B: " << deckBPath << "\n";
        return 1;
    }
    auto* deckB = engine.getDeck(1);
    const auto& metaB = deckB->getDecodedAudio();
    std::cout << "  Deck B: " << metaB.durationSeconds << "s | "
              << metaB.sampleRate << "Hz | " << metaB.channels << "ch | "
              << "Peak: " << metaB.peakAmplitude << " (" << metaB.maxPeakDb << " dB) | "
              << "BPM: " << metaB.detectedBpm << " (Conf: " << metaB.tempoProfile.confidence << ") | "
              << "Phrases: 4-bar=" << metaB.phraseProfile.boundaries4Bar.size()
              << ", 8-bar=" << metaB.phraseProfile.boundaries8Bar.size()
              << " (PhraseConf: " << metaB.phraseProfile.phraseConfidence << ")\n";

    // 4. Evaluate Bounded Tempo Strategy
    auto strategyMode = pulse::audio::TempoStrategy::parseModeString(tempoStrategyStr);
    auto tempoDecision = pulse::audio::TempoStrategy::evaluate(
        metaA.detectedBpm,
        metaB.detectedBpm,
        metaA.tempoProfile.alternativeHypotheses,
        metaB.tempoProfile.alternativeHypotheses,
        strategyMode,
        targetMasterBpm,
        maxStretchPct,
        forceStretch
    );

    std::cout << "Tempo Strategy Evaluation:\n"
              << "  Strategy Decision:  " << tempoDecision.strategy << "\n"
              << "  Deck A Tempo Ratio: " << tempoDecision.effectiveDeckATempoRatio << "\n"
              << "  Deck B Tempo Ratio: " << tempoDecision.effectiveDeckBTempoRatio << "\n"
              << "  BPM Delta:          " << tempoDecision.bpmDeltaPercent << "%\n"
              << "  Octave Jump:        " << (tempoDecision.octaveJumpApplied ? "YES" : "NO") << "\n"
              << "  Artifact Risk:      " << tempoDecision.artifactRiskScore << "\n"
              << "  Exceeded Limit:     " << (tempoDecision.stretchExceededThreshold ? "YES (PENALIZED/REJECTED)" : "NO") << "\n"
              << "  Recommended Type:   " << tempoDecision.recommendedTransitionType << "\n";

    if (tempoDecision.stretchExceededThreshold && !forceStretch) {
        std::cout << "  ⚠️ WARNING: " << tempoDecision.rejectionReason << "\n"
                  << "  Switching transition type from eq_crossfade to " << tempoDecision.recommendedTransitionType << ".\n";
    }

    // Configure Deck Players with evaluated tempo ratios
    deckA->setTempoRatio(tempoDecision.effectiveDeckATempoRatio);
    deckB->setTempoRatio(tempoDecision.effectiveDeckBTempoRatio);

    // 5. Calculate Transition Planning, Phrase & Beat Alignment & Timing
    double effectiveDurA = metaA.durationSeconds / tempoDecision.effectiveDeckATempoRatio;
    double effectiveDurB = metaB.durationSeconds / tempoDecision.effectiveDeckBTempoRatio;

    pulse::audio::TransitionPlanResult planResult;
    bool fallbackApplied = false;
    std::string fallbackReasonStr = "";

    double transStartSec = 0.0;
    double actualTransDuration = transitionDuration;
    double alignedBeatASec = 0.0;
    double alignedCueBSec = 0.0;
    std::string alignmentMode = "phrase_aligned";
    double phraseScore = 0.0;
    uint32_t phraseBarsCount = phraseBars;

    if (phraseAware && customTransitionStart < 0.0 && !customTransDurationGiven) {
        planResult = pulse::audio::TransitionPlanner::planTransition(
            metaA,
            metaB,
            tempoDecision.effectiveDeckATempoRatio,
            tempoDecision.effectiveDeckBTempoRatio,
            phraseBars,
            minPhraseConfidence,
            phraseAware
        );

        transStartSec = planResult.selectedWindow.sourceExitSec;
        actualTransDuration = planResult.selectedWindow.durationSeconds;
        alignedCueBSec = planResult.selectedWindow.destEntrySec;
        alignedBeatASec = transStartSec;
        alignmentMode = planResult.selectedWindow.alignmentStrategy;
        phraseScore = planResult.selectedWindow.phraseScore;
        phraseBarsCount = planResult.selectedWindow.durationBars;
        fallbackApplied = !planResult.selectedWindow.isPhraseAligned;
        fallbackReasonStr = planResult.selectedWindow.fallbackReason;
    } else {
        // Fallback or explicit user custom transition parameters
        actualTransDuration = std::max(0.1, std::min(transitionDuration, std::min(effectiveDurA, effectiveDurB)));
        double rawTransStartSec = (customTransitionStart >= 0.0)
            ? customTransitionStart
            : std::max(0.0, effectiveDurA - actualTransDuration);

        alignmentMode = "none";
        alignedBeatASec = rawTransStartSec;
        alignedCueBSec = 0.0;

        if (alignBeats) {
            if (snapToBar && !metaA.tempoProfile.downbeatPositions.empty()) {
                alignmentMode = "downbeat_snap";
                double bestDownbeat = metaA.tempoProfile.downbeatPositions.front() / tempoDecision.effectiveDeckATempoRatio;
                for (double db : metaA.tempoProfile.downbeatPositions) {
                    double scaledDb = db / tempoDecision.effectiveDeckATempoRatio;
                    if (scaledDb <= rawTransStartSec) {
                        bestDownbeat = scaledDb;
                    } else {
                        break;
                    }
                }
                alignedBeatASec = bestDownbeat;
            } else if (!metaA.tempoProfile.beatPositions.empty()) {
                alignmentMode = "beat_snap";
                double bestBeat = metaA.tempoProfile.beatPositions.front() / tempoDecision.effectiveDeckATempoRatio;
                for (double bp : metaA.tempoProfile.beatPositions) {
                    double scaledBp = bp / tempoDecision.effectiveDeckATempoRatio;
                    if (scaledBp <= rawTransStartSec) {
                        bestBeat = scaledBp;
                    } else {
                        break;
                    }
                }
                alignedBeatASec = bestBeat;
            } else {
                alignmentMode = "time_fallback";
                alignedBeatASec = rawTransStartSec;
            }

            if (snapToBar && !metaB.tempoProfile.downbeatPositions.empty()) {
                alignedCueBSec = metaB.tempoProfile.downbeatPositions.front() / tempoDecision.effectiveDeckBTempoRatio;
            } else if (!metaB.tempoProfile.beatPositions.empty()) {
                alignedCueBSec = metaB.tempoProfile.beatPositions.front() / tempoDecision.effectiveDeckBTempoRatio;
            } else {
                alignedCueBSec = metaB.tempoProfile.gridOffsetSeconds / tempoDecision.effectiveDeckBTempoRatio;
            }
        }

        transStartSec = alignedBeatASec;
        fallbackApplied = true;
        fallbackReasonStr = !phraseAware ? "Phrase alignment disabled by user configuration" : "Custom transition parameters provided";
        phraseScore = 0.50;
        phraseBarsCount = static_cast<uint32_t>(std::max(1.0, std::round(actualTransDuration / (4.0 * (60.0 / (metaA.detectedBpm * tempoDecision.effectiveDeckATempoRatio))))));
    }

    double phaseOffsetSec = metaA.tempoProfile.gridOffsetSeconds - metaB.tempoProfile.gridOffsetSeconds;

    double totalRenderSec = (customTotalDuration > 0.0)
        ? customTotalDuration
        : (transStartSec + effectiveDurB - alignedCueBSec);

    std::cout << "Executing transition:\n"
              << "  Alignment Mode:      " << alignmentMode << "\n"
              << "  Deck A Aligned Pos:  " << alignedBeatASec << "s\n"
              << "  Deck B Cue Pos:      " << alignedCueBSec << "s\n"
              << "  Transition Start:    " << transStartSec << "s\n"
              << "  Transition Window:   " << actualTransDuration << "s (" << phraseBarsCount << " bars)\n"
              << "  Phrase Score:        " << phraseScore << "\n"
              << "  Fallback Applied:    " << (fallbackApplied ? "YES (" + fallbackReasonStr + ")" : "NO") << "\n"
              << "  Total Render Mix:    " << totalRenderSec << "s\n"
              << "----------------------------------------------------------------\n";

    // 6. Setup Engine Initial State
    auto* mixer = engine.getMixer();
    auto* transExec = engine.getTransitionExecutor();

    mixer->setCrossfader(-1.0f); // 100% Deck A
    mixer->setMasterVolume(1.0f);

    deckA->setPlaybackPosition(0.0);
    deckA->setPlaying(true);

    deckB->setPlaybackPosition(alignedCueBSec * tempoDecision.effectiveDeckBTempoRatio);
    deckB->setPlaying(false);

    // 7. Offline Block-by-Block Mix Rendering
    uint32_t sampleRate = config.sample_rate;
    uint32_t blockSize = config.buffer_size;
    uint32_t channels = config.channel_count;
    double dt = static_cast<double>(blockSize) / sampleRate;

    uint64_t totalFramesToRender = static_cast<uint64_t>(totalRenderSec * sampleRate);
    std::vector<float> renderedMaster;
    renderedMaster.reserve(totalFramesToRender * channels);

    std::vector<float> blockBuffer(blockSize * channels, 0.0f);
    double currentTime = 0.0;
    bool transitionTriggered = false;

    while (currentTime < totalRenderSec) {
        // Trigger Deck B playback and transition executor when reaching transition timestamp
        if (currentTime >= transStartSec && !transitionTriggered) {
            deckB->setPlaybackPosition(alignedCueBSec * tempoDecision.effectiveDeckBTempoRatio);
            deckB->setPlaying(true);
            TransitionCommandC cmd{0, 1, actualTransDuration, 0};
            transExec->startTransition(cmd);
            transitionTriggered = true;
        }

        // Update transition crossfader automation
        if (transitionTriggered && currentTime >= transStartSec) {
            double elapsedTrans = currentTime - transStartSec;
            transExec->updateAutomation(elapsedTrans, *mixer);
        }

        // Process audio block through real-time audio pipeline
        engine.processAudioBlock(blockBuffer.data(), blockSize, channels);

        // Append block to render accumulator
        renderedMaster.insert(renderedMaster.end(), blockBuffer.begin(), blockBuffer.end());

        currentTime += dt;
    }

    uint64_t renderedFrames = renderedMaster.size() / channels;
    double actualRenderedDuration = static_cast<double>(renderedFrames) / sampleRate;

    // 8. Analyze Rendered Output
    float maxPeakOut = 0.0f;
    for (float sample : renderedMaster) {
        float absVal = std::abs(sample);
        if (absVal > maxPeakOut) {
            maxPeakOut = absVal;
        }
    }
    bool clippingDetected = (maxPeakOut >= (1.0f - 1e-4f));
    float maxPeakDb = (maxPeakOut > 1e-5f) ? 20.0f * std::log10(maxPeakOut) : -100.0f;

    std::cout << "Rendering Complete:\n"
              << "  Rendered Frames:     " << renderedFrames << "\n"
              << "  Rendered Duration:   " << actualRenderedDuration << "s\n"
              << "  Max Peak Amplitude:  " << maxPeakOut << " (" << maxPeakDb << " dB)\n"
              << "  Clipping Detected:   " << (clippingDetected ? "YES (LIMITED)" : "NO") << "\n";

    // 9. Write Master WAV Output
    std::cout << "Writing master audio artifact: " << outAudioPath << "..." << std::endl;
    if (!pulse::audio::WavWriter::writeWav16(outAudioPath, renderedMaster.data(), renderedFrames, sampleRate, channels)) {
        std::cerr << "Error: Failed to write output WAV file: " << outAudioPath << "\n";
        return 1;
    }

    // 10. Write JSON Execution Report
    std::cout << "Emitting JSON telemetry report: " << outReportPath << "..." << std::endl;
    std::ofstream reportFile(outReportPath);
    if (!reportFile.is_open()) {
        std::cerr << "Error: Failed to write output JSON report: " << outReportPath << "\n";
        return 1;
    }

    std::string execTransType = (tempoDecision.stretchExceededThreshold && !forceStretch)
        ? tempoDecision.recommendedTransitionType
        : transitionStrategy;

    reportFile << "{\n"
               << "  \"deck_a\": {\n"
               << "    \"file_path\": \"" << formatJsonString(deckAPath) << "\",\n"
               << "    \"sample_rate\": " << metaA.sampleRate << ",\n"
               << "    \"channels\": " << metaA.channels << ",\n"
               << "    \"duration_seconds\": " << std::fixed << std::setprecision(2) << metaA.durationSeconds << ",\n"
               << "    \"detected_bpm\": " << std::fixed << std::setprecision(1) << metaA.detectedBpm << ",\n"
               << "    \"bpm_confidence\": " << std::fixed << std::setprecision(2) << metaA.tempoProfile.confidence << ",\n"
               << "    \"alternative_hypotheses\": " << formatDoubleArrayJson(metaA.tempoProfile.alternativeHypotheses) << ",\n"
               << "    \"grid_offset_seconds\": " << std::fixed << std::setprecision(3) << metaA.tempoProfile.gridOffsetSeconds << ",\n"
               << "    \"total_beats_detected\": " << metaA.tempoProfile.beatPositions.size() << ",\n"
               << "    \"is_variable_tempo\": " << (metaA.tempoProfile.isVariableTempo ? "true" : "false") << ",\n"
               << "    \"peak_amplitude\": " << std::fixed << std::setprecision(4) << metaA.peakAmplitude << "\n"
               << "  },\n"
               << "  \"deck_b\": {\n"
               << "    \"file_path\": \"" << formatJsonString(deckBPath) << "\",\n"
               << "    \"sample_rate\": " << metaB.sampleRate << ",\n"
               << "    \"channels\": " << metaB.channels << ",\n"
               << "    \"duration_seconds\": " << std::fixed << std::setprecision(2) << metaB.durationSeconds << ",\n"
               << "    \"detected_bpm\": " << std::fixed << std::setprecision(1) << metaB.detectedBpm << ",\n"
               << "    \"bpm_confidence\": " << std::fixed << std::setprecision(2) << metaB.tempoProfile.confidence << ",\n"
               << "    \"alternative_hypotheses\": " << formatDoubleArrayJson(metaB.tempoProfile.alternativeHypotheses) << ",\n"
               << "    \"grid_offset_seconds\": " << std::fixed << std::setprecision(3) << metaB.tempoProfile.gridOffsetSeconds << ",\n"
               << "    \"total_beats_detected\": " << metaB.tempoProfile.beatPositions.size() << ",\n"
               << "    \"is_variable_tempo\": " << (metaB.tempoProfile.isVariableTempo ? "true" : "false") << ",\n"
               << "    \"peak_amplitude\": " << std::fixed << std::setprecision(4) << metaB.peakAmplitude << "\n"
               << "  },\n"
               << "  \"tempo_matching\": {\n"
               << "    \"strategy\": \"" << tempoDecision.strategy << "\",\n"
               << "    \"source_native_bpm\": " << std::fixed << std::setprecision(1) << tempoDecision.sourceNativeBpm << ",\n"
               << "    \"destination_native_bpm\": " << std::fixed << std::setprecision(1) << tempoDecision.destNativeBpm << ",\n"
               << "    \"effective_tempo_ratio\": " << std::fixed << std::setprecision(4) << tempoDecision.effectiveTempoRatio << ",\n"
               << "    \"bpm_delta_percent\": " << std::fixed << std::setprecision(2) << tempoDecision.bpmDeltaPercent << ",\n"
               << "    \"pitch_semitone_shift\": " << std::fixed << std::setprecision(1) << tempoDecision.pitchSemitoneShift << ",\n"
               << "    \"octave_jump_applied\": " << (tempoDecision.octaveJumpApplied ? "true" : "false") << ",\n"
               << "    \"artifact_risk_score\": " << std::fixed << std::setprecision(2) << tempoDecision.artifactRiskScore << ",\n"
               << "    \"stretch_exceeded_threshold\": " << (tempoDecision.stretchExceededThreshold ? "true" : "false") << ",\n"
               << "    \"rejection_reason\": " << (tempoDecision.rejectionReason.empty() ? "null" : ("\"" + formatJsonString(tempoDecision.rejectionReason) + "\"")) << "\n"
               << "  },\n"
               << "  \"beat_alignment\": {\n"
               << "    \"alignment_mode\": \"" << alignmentMode << "\",\n"
               << "    \"deck_a_aligned_beat_sec\": " << std::fixed << std::setprecision(2) << alignedBeatASec << ",\n"
               << "    \"deck_b_aligned_cue_sec\": " << std::fixed << std::setprecision(2) << alignedCueBSec << ",\n"
               << "    \"phase_offset_sec\": " << std::fixed << std::setprecision(2) << phaseOffsetSec << ",\n"
               << "    \"tempo_ratio\": " << std::fixed << std::setprecision(2) << tempoDecision.effectiveTempoRatio << "\n"
               << "  },\n"
               << "  \"phrase_analysis\": {\n"
               << "    \"deck_a_phrases_4bar_count\": " << metaA.phraseProfile.boundaries4Bar.size() << ",\n"
               << "    \"deck_a_phrases_8bar_count\": " << metaA.phraseProfile.boundaries8Bar.size() << ",\n"
               << "    \"deck_a_confidence\": " << std::fixed << std::setprecision(2) << metaA.phraseProfile.phraseConfidence << ",\n"
               << "    \"deck_b_phrases_4bar_count\": " << metaB.phraseProfile.boundaries4Bar.size() << ",\n"
               << "    \"deck_b_phrases_8bar_count\": " << metaB.phraseProfile.boundaries8Bar.size() << ",\n"
               << "    \"deck_b_confidence\": " << std::fixed << std::setprecision(2) << metaB.phraseProfile.phraseConfidence << "\n"
               << "  },\n"
               << "  \"transition_window\": {\n"
               << "    \"strategy\": \"" << execTransType << "\",\n"
               << "    \"alignment_strategy\": \"" << alignmentMode << "\",\n"
               << "    \"phrase_length_bars\": " << phraseBarsCount << ",\n"
               << "    \"duration_seconds\": " << std::fixed << std::setprecision(2) << actualTransDuration << ",\n"
               << "    \"deck_a_exit_sec\": " << std::fixed << std::setprecision(2) << transStartSec << ",\n"
               << "    \"deck_b_entry_sec\": " << std::fixed << std::setprecision(2) << alignedCueBSec << ",\n"
               << "    \"phrase_score\": " << std::fixed << std::setprecision(2) << phraseScore << ",\n"
               << "    \"fallback_applied\": " << (fallbackApplied ? "true" : "false") << ",\n"
               << "    \"fallback_reason\": " << (fallbackReasonStr.empty() ? "null" : ("\"" + formatJsonString(fallbackReasonStr) + "\"")) << "\n"
               << "  },\n"
               << "  \"transition\": {\n"
               << "    \"type\": \"" << execTransType << "\",\n"
               << "    \"duration_seconds\": " << std::fixed << std::setprecision(2) << actualTransDuration << ",\n"
               << "    \"start_position_seconds\": " << std::fixed << std::setprecision(2) << transStartSec << "\n"
               << "  },\n"
               << "  \"output\": {\n"
               << "    \"file_path\": \"" << formatJsonString(outAudioPath) << "\",\n"
               << "    \"rendered_duration_seconds\": " << std::fixed << std::setprecision(2) << actualRenderedDuration << ",\n"
               << "    \"clipping_detected\": " << (clippingDetected ? "true" : "false") << ",\n"
               << "    \"max_peak_db\": " << std::fixed << std::setprecision(2) << maxPeakDb << "\n"
               << "  }\n"
               << "}\n";
    reportFile.close();

    std::cout << "✨ PULSE Phase 0 CLI Feasibility Run SUCCESSFUL!\n";
    return 0;
}
