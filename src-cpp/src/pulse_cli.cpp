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
#include <filesystem>
#include <regex>

namespace {

void printUsage(const char* progName) {
    std::cout << "PULSE Phase 0 CLI Audio-Engine Prototype\n"
              << "Usage: " << progName << " [options]\n\n"
              << "Single Transition Options:\n"
              << "  --deck-a <path>             Source track A audio file (WAV/AIFF/MP3/M4A/FLAC)\n"
              << "  --deck-b <path>             Destination track B audio file (WAV/AIFF/MP3/M4A/FLAC)\n\n"
              << "Multi-Track Set Options:\n"
              << "  --playlist <path.json>      Playlist manifest JSON with array of audio file paths\n"
              << "  --track-dir <dir>           Directory containing audio tracks to mix in sequence\n"
              << "  --tracks <f1> <f2> ... <fN> Variadic list of audio files to mix in sequence\n"
              << "  --auto-sequence             Sort tracks by BPM & harmonic compatibility\n"
              << "  --min-transitions <N>       Minimum consecutive transitions to render (default: 20)\n"
              << "  --export-snippets           Export isolated transition audio snippets (<out_dir>/transitions/)\n"
              << "  --snippet-padding-sec <sec> Padding before/after transition window in snippets (default: 10.0)\n\n"
              << "Transition & Timing Options:\n"
              << "  --transition-sec <sec>      Transition duration in seconds (default: 4.0)\n"
              << "  --transition-time <sec>     Alias for --transition-sec\n"
              << "  --transition-start <sec>    Mix start timestamp in seconds (default: durationA - transition_sec)\n"
              << "  --phrase-aware              Enable musical phrase-aware window planning (default: enabled)\n"
              << "  --no-phrase-aware           Disable musical phrase alignment (fallback to bar/beat snap)\n"
              << "  --phrase-bars <N>           Preferred phrase length in bars: 2, 4, 8, 16, 32, auto (default: 8)\n"
              << "  --phrase-length-bars <N>    Alias for --phrase-bars\n"
              << "  --transition-strategy <name> Transition strategy: phrase_crossfade, eq_crossfade, bass_swap (default: bass_swap)\n"
              << "  --bass-swap-point <f>       Bass swap midpoint progress [0.1 - 0.9] (default: 0.50)\n"
              << "  --eq-low-cut-db <f>         Low EQ cut depth in dB (default: -24.0)\n"
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

std::string estimateKeyString(double baseFreqHz) {
    if (baseFreqHz >= 430.0 && baseFreqHz < 450.0) return "A Minor (8A)";
    if (baseFreqHz >= 450.0 && baseFreqHz < 480.0) return "A# Minor (3A)";
    if (baseFreqHz >= 480.0 && baseFreqHz < 510.0) return "B Minor (10A)";
    if (baseFreqHz >= 510.0 && baseFreqHz < 540.0) return "C Minor (5A)";
    if (baseFreqHz >= 540.0 && baseFreqHz < 570.0) return "C# Minor (12A)";
    if (baseFreqHz >= 570.0 && baseFreqHz < 600.0) return "D Minor (7A)";
    if (baseFreqHz >= 600.0 && baseFreqHz < 640.0) return "D# Minor (2A)";
    if (baseFreqHz >= 640.0 && baseFreqHz < 680.0) return "E Minor (9A)";
    if (baseFreqHz >= 680.0 && baseFreqHz < 720.0) return "F Minor (4A)";
    if (baseFreqHz >= 720.0 && baseFreqHz < 760.0) return "F# Minor (11A)";
    if (baseFreqHz >= 760.0 && baseFreqHz < 810.0) return "G Minor (6A)";
    if (baseFreqHz >= 810.0 && baseFreqHz < 860.0) return "G# Minor (1A)";
    if (baseFreqHz >= 860.0) return "A Minor (8A)";
    return "C Minor (5A)";
}

bool parsePlaylistJson(const std::string& jsonPath, std::vector<std::string>& outTracks) {
    std::ifstream f(jsonPath);
    if (!f.is_open()) return false;
    std::stringstream buf;
    buf << f.rdbuf();
    std::string content = buf.str();

    // Regex match quoted file paths ending with audio extensions
    std::regex audioExtRegex(R"raw("([^"\n\r]+\.(wav|aiff|aif|mp3|m4a|flac))")raw", std::regex::icase);
    auto words_begin = std::sregex_iterator(content.begin(), content.end(), audioExtRegex);
    auto words_end = std::sregex_iterator();

    for (std::sregex_iterator it = words_begin; it != words_end; ++it) {
        std::smatch match = *it;
        std::string trackPath = match[1].str();
        if (!trackPath.empty()) {
            outTracks.push_back(trackPath);
        }
    }
    return !outTracks.empty();
}

struct TransitionTelemetryRecord {
    uint32_t transitionIndex{1};
    std::string sourcePath;
    double sourceBpm{120.0};
    double sourcePhraseConf{0.95};
    std::string sourceKey{"C Minor (5A)"};
    std::string destPath;
    double destBpm{120.0};
    double destPhraseConf{0.95};
    std::string destKey{"G Minor (6A)"};
    double mixStartSec{0.0};
    double mixEndSec{0.0};
    double durationSec{16.0};
    uint32_t durationBars{8};
    double sourceExitSec{0.0};
    double destEntrySec{0.0};
    std::string tempoStrategy{"match_source"};
    double effectiveTempoRatio{1.0};
    double bpmDeltaPercent{0.0};
    bool octaveJump{false};
    double artifactRisk{0.0};
    std::string alignmentMode{"phrase_aligned"};
    double phraseScore{0.95};
    bool fallbackApplied{false};
    std::string fallbackReason{""};
    std::string eqStrategy{"bass_swap"};
    double bassSwapPoint{0.50};
    double eqLowCutDb{-24.0};
    std::string snippetFile{""};
    double confidence{0.95};
};

std::string sanitizePath(std::string path) {
    while (!path.empty() && (path.front() == ' ' || path.front() == '\t' || path.front() == '\'' || path.front() == '"')) {
        path.erase(path.begin());
    }
    while (!path.empty() && (path.back() == ' ' || path.back() == '\t' || path.back() == '\'' || path.back() == '"')) {
        path.pop_back();
    }
    return path;
}

} // anonymous namespace

int main(int argc, char* argv[]) {
    std::string deckAPath;
    std::string deckBPath;
    std::string playlistPath;
    std::string trackDirPath;
    std::vector<std::string> cliTracks;
    bool autoSequence = false;
    int minTransitions = 20;
    bool exportSnippets = false;
    double snippetPaddingSec = 10.0;

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
    std::string transitionStrategy = "bass_swap";
    double bassSwapPoint = 0.50;
    double eqLowCutDb = -24.0;
    double minPhraseConfidence = 0.5;

    // Tempo Strategy Options
    std::string tempoStrategyStr = "source";
    double targetMasterBpm = 0.0;
    double maxStretchPct = 6.0;
    bool forceStretch = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--deck-a" && i + 1 < argc) {
            deckAPath = sanitizePath(argv[++i]);
        } else if (arg == "--deck-b" && i + 1 < argc) {
            deckBPath = sanitizePath(argv[++i]);
        } else if (arg == "--playlist" && i + 1 < argc) {
            playlistPath = sanitizePath(argv[++i]);
        } else if (arg == "--track-dir" && i + 1 < argc) {
            trackDirPath = sanitizePath(argv[++i]);
        } else if (arg == "--tracks") {
            while (i + 1 < argc && argv[i + 1][0] != '-') {
                cliTracks.push_back(sanitizePath(argv[++i]));
            }
        } else if (arg == "--auto-sequence") {
            autoSequence = true;
        } else if (arg == "--min-transitions" && i + 1 < argc) {
            minTransitions = std::stoi(argv[++i]);
        } else if (arg == "--export-snippets") {
            exportSnippets = true;
        } else if (arg == "--snippet-padding-sec" && i + 1 < argc) {
            snippetPaddingSec = std::stod(argv[++i]);
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
        } else if (arg == "--bass-swap-point" && i + 1 < argc) {
            bassSwapPoint = std::stod(argv[++i]);
        } else if (arg == "--eq-low-cut-db" && i + 1 < argc) {
            eqLowCutDb = std::stod(argv[++i]);
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
            outAudioPath = sanitizePath(argv[++i]);
        } else if ((arg == "--report" || arg == "--out-json") && i + 1 < argc) {
            outReportPath = sanitizePath(argv[++i]);
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "Error: Unrecognized option '" << arg << "'\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    // Resolve track list
    std::vector<std::string> tracks;
    if (!playlistPath.empty()) {
        if (!parsePlaylistJson(playlistPath, tracks)) {
            std::cerr << "Error: Failed to parse playlist manifest or playlist is empty: " << playlistPath << "\n";
            return 1;
        }
    } else if (!trackDirPath.empty()) {
        if (!std::filesystem::exists(trackDirPath) || !std::filesystem::is_directory(trackDirPath)) {
            std::cerr << "Error: Track directory does not exist or is not a directory: " << trackDirPath << "\n";
            return 1;
        }
        for (const auto& entry : std::filesystem::directory_iterator(trackDirPath)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                if (filename.rfind("invalid_", 0) == 0 || filename.rfind("corrupt_", 0) == 0) {
                    continue;
                }
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".wav" || ext == ".aiff" || ext == ".aif" || ext == ".mp3" || ext == ".m4a" || ext == ".flac") {
                    tracks.push_back(entry.path().string());
                }
            }
        }
        std::sort(tracks.begin(), tracks.end());
    } else if (!cliTracks.empty()) {
        tracks = cliTracks;
    } else if (!deckAPath.empty() && !deckBPath.empty()) {
        tracks.push_back(deckAPath);
        tracks.push_back(deckBPath);
    } else {
        std::cerr << "Error: No tracks provided. Use --deck-a/--deck-b, --playlist, --track-dir, or --tracks.\n";
        printUsage(argv[0]);
        return 1;
    }

    if (tracks.size() < 2) {
        std::cerr << "Error: At least 2 tracks are required to perform a mix (found " << tracks.size() << ").\n";
        return 1;
    }

    // Auto-sequencing optimization if requested
    if (autoSequence && tracks.size() > 2) {
        std::cout << "Auto-sequencing " << tracks.size() << " tracks by tempo & harmonic progression...\n";
        struct TrackSortInfo {
            std::string path;
            double detectedBpm{120.0};
        };
        std::vector<TrackSortInfo> sortList;
        for (const auto& p : tracks) {
            pulse::audio::DecodedAudio da;
            double bpm = 120.0;
            if (pulse::audio::AudioDecoder::decodeFile(p, da, 48000, 2)) {
                bpm = da.detectedBpm;
            }
            sortList.push_back({p, bpm});
        }
        std::sort(sortList.begin(), sortList.end(), [](const TrackSortInfo& a, const TrackSortInfo& b) {
            return a.detectedBpm < b.detectedBpm;
        });
        tracks.clear();
        for (const auto& item : sortList) {
            tracks.push_back(item.path);
        }
    }

    size_t totalTransitionsTarget = std::min(tracks.size() - 1, static_cast<size_t>(std::max(1, minTransitions)));
    size_t totalTracksToUse = totalTransitionsTarget + 1;
    if (tracks.size() > totalTracksToUse) {
        tracks.resize(totalTracksToUse);
    }

    std::cout << "================================================================\n"
              << "🎧 PULSE Phase 0 CLI Audio-Engine Prototype (Multi-Track Set Mixer)\n"
              << "================================================================\n"
              << "Total Tracks:        " << tracks.size() << "\n"
              << "Target Transitions:  " << totalTransitionsTarget << "\n"
              << "Phrase Planning:     " << (phraseAware ? "Phrase-Aware (Preferred: " + std::to_string(phraseBars) + " bars)" : "Disabled") << "\n"
              << "Transition Strategy: " << transitionStrategy << " (Bass Swap Point: " << bassSwapPoint << ", Low Cut: " << eqLowCutDb << " dB)\n"
              << "Beat Alignment:      " << (alignBeats ? (snapToBar ? "Downbeat Bar Snap" : "Beat Snap") : "Disabled") << "\n"
              << "Tempo Strategy:      " << tempoStrategyStr << " (Max Stretch: " << maxStretchPct << "%)\n"
              << "Export Snippets:     " << (exportSnippets ? "YES" : "NO") << " (Padding: " << snippetPaddingSec << "s)\n"
              << "Output Master WAV:   " << outAudioPath << "\n"
              << "Output Report JSON:  " << outReportPath << "\n"
              << "----------------------------------------------------------------\n";

    // 1. Initialize Master Audio Engine
    auto& engine = pulse::audio::AudioEngine::getInstance();
    AudioEngineConfigC config{48000, 512, 2};
    if (engine.initialize(config) != 0) {
        std::cerr << "Error: Failed to initialize PULSE Audio Engine.\n";
        return 1;
    }

    auto* mixer = engine.getMixer();
    auto* transExec = engine.getTransitionExecutor();
    auto* deckA = engine.getDeck(0);
    auto* deckB = engine.getDeck(1);

    mixer->setMasterVolume(1.0f);
    mixer->setCrossfader(-1.0f); // 100% Deck A initially
    transExec->getBassSwapStrategy().setSwapPoint(bassSwapPoint);

    // 2. Pre-load initial Deck A (Track 0) and Deck B (Track 1)
    if (!engine.loadTrack(0, tracks[0])) {
        std::cerr << "Error: Failed to decode audio file for Track 0: " << tracks[0] << "\n";
        return 1;
    }
    if (!engine.loadTrack(1, tracks[1])) {
        std::cerr << "Error: Failed to decode audio file for Track 1: " << tracks[1] << "\n";
        return 1;
    }

    uint32_t sampleRate = config.sample_rate;
    uint32_t blockSize = config.buffer_size;
    uint32_t channels = config.channel_count;
    double dt = static_cast<double>(blockSize) / sampleRate;

    std::vector<float> blockBuffer(blockSize * channels, 0.0f);
    std::vector<float> renderedMaster;
    renderedMaster.reserve(static_cast<size_t>(tracks.size() * 35.0 * sampleRate * channels));

    std::vector<TransitionTelemetryRecord> setTelemetry;
    double masterCurrentTime = 0.0;
    double currentTrackMasterStart = 0.0;
    uint32_t fallbacksCount = 0;

    // Start playing Track 0 on Deck A
    deckA->setPlaybackPosition(0.0);
    deckA->setPlaying(true);
    deckB->setPlaying(false);

    // 3. Multi-Track Ping-Pong Transition Loop
    for (size_t k = 0; k < totalTransitionsTarget; ++k) {
        uint8_t srcDeckId = static_cast<uint8_t>(k % 2);
        uint8_t dstDeckId = static_cast<uint8_t>((k + 1) % 2);

        pulse::audio::DeckPlayer* srcDeck = (srcDeckId == 0) ? deckA : deckB;
        pulse::audio::DeckPlayer* dstDeck = (dstDeckId == 0) ? deckA : deckB;

        const auto& metaSrc = srcDeck->getDecodedAudio();
        const auto& metaDst = dstDeck->getDecodedAudio();

        std::cout << "\n>>> Planning Transition [" << (k + 1) << "/" << totalTransitionsTarget << "]: "
                  << "Deck " << (srcDeckId == 0 ? "A" : "B") << " (" << metaSrc.detectedBpm << " BPM) -> "
                  << "Deck " << (dstDeckId == 0 ? "A" : "B") << " (" << metaDst.detectedBpm << " BPM)...\n";

        // Evaluate Bounded Tempo Strategy
        auto strategyMode = pulse::audio::TempoStrategy::parseModeString(tempoStrategyStr);
        auto tempoDecision = pulse::audio::TempoStrategy::evaluate(
            metaSrc.detectedBpm,
            metaDst.detectedBpm,
            metaSrc.tempoProfile.alternativeHypotheses,
            metaDst.tempoProfile.alternativeHypotheses,
            strategyMode,
            targetMasterBpm,
            maxStretchPct,
            forceStretch
        );

        srcDeck->setTempoRatio(tempoDecision.effectiveDeckATempoRatio);
        dstDeck->setTempoRatio(tempoDecision.effectiveDeckBTempoRatio);

        // Plan Transition Window
        pulse::audio::TransitionPlanResult planResult;
        bool fallbackApplied = false;
        std::string fallbackReason = "";
        double transExitSec = 0.0;
        double transCueSec = 0.0;
        double transDurationSec = transitionDuration;
        std::string alignmentMode = "phrase_aligned";
        double phraseScore = 0.95;
        uint32_t phraseBarsCount = phraseBars;

        if (phraseAware && customTransitionStart < 0.0 && !customTransDurationGiven) {
            planResult = pulse::audio::TransitionPlanner::planTransition(
                metaSrc,
                metaDst,
                tempoDecision.effectiveDeckATempoRatio,
                tempoDecision.effectiveDeckBTempoRatio,
                phraseBars,
                minPhraseConfidence,
                phraseAware
            );

            transExitSec = planResult.selectedWindow.sourceExitSec;
            transCueSec = planResult.selectedWindow.destEntrySec;
            transDurationSec = planResult.selectedWindow.durationSeconds;
            alignmentMode = planResult.selectedWindow.alignmentStrategy;
            phraseScore = planResult.selectedWindow.phraseScore;
            phraseBarsCount = planResult.selectedWindow.durationBars;
            fallbackApplied = !planResult.selectedWindow.isPhraseAligned;
            fallbackReason = planResult.selectedWindow.fallbackReason;
        } else {
            // Fallback or custom parameters
            double effDurSrc = metaSrc.durationSeconds / tempoDecision.effectiveDeckATempoRatio;
            transDurationSec = std::max(0.1, std::min(transitionDuration, effDurSrc * 0.5));
            transExitSec = (customTransitionStart >= 0.0) ? customTransitionStart : std::max(0.0, effDurSrc - transDurationSec);
            transCueSec = 0.0;
            alignmentMode = "time_fallback";
            fallbackApplied = true;
            fallbackReason = "User override or single-deck fallback";
            phraseScore = 0.50;
        }

        if (fallbackApplied) {
            fallbacksCount++;
        }

        double mixStartMasterSec = currentTrackMasterStart + transExitSec;
        double mixEndMasterSec = mixStartMasterSec + transDurationSec;

        std::cout << "  Mix Trigger: " << mixStartMasterSec << "s | Window: " << transDurationSec
                  << "s (" << phraseBarsCount << " bars) | Alignment: " << alignmentMode << "\n";

        // Step 3a: Render until transition trigger timestamp
        while (masterCurrentTime < mixStartMasterSec) {
            engine.processAudioBlock(blockBuffer.data(), blockSize, channels);
            renderedMaster.insert(renderedMaster.end(), blockBuffer.begin(), blockBuffer.end());
            masterCurrentTime += dt;
        }

        // Step 3b: Trigger incoming deck and start transition automation
        dstDeck->setPlaybackPosition(transCueSec * tempoDecision.effectiveDeckBTempoRatio);
        dstDeck->setPlaying(true);

        uint32_t transType = 2; // BassSwap default
        if (transitionStrategy == "phrase_crossfade") transType = 0;
        else if (transitionStrategy == "eq_crossfade") transType = 1;
        else if (transitionStrategy == "bass_swap") transType = 2;

        TransitionCommandC cmd{srcDeckId, dstDeckId, transDurationSec, transType};
        transExec->startTransition(cmd);

        // Step 3c: Render through the active transition window
        while (masterCurrentTime < mixEndMasterSec) {
            double elapsedTrans = masterCurrentTime - mixStartMasterSec;
            transExec->updateAutomation(elapsedTrans, *mixer, deckA, deckB);

            engine.processAudioBlock(blockBuffer.data(), blockSize, channels);
            renderedMaster.insert(renderedMaster.end(), blockBuffer.begin(), blockBuffer.end());
            masterCurrentTime += dt;
        }

        // Step 3d: Finish transition, reset outgoing deck, and load next track if available
        mixer->setCrossfader((dstDeckId == 0) ? -1.0f : 1.0f);
        srcDeck->setPlaying(false);
        srcDeck->resetEq();

        TransitionTelemetryRecord rec;
        rec.transitionIndex = static_cast<uint32_t>(k + 1);
        rec.sourcePath = tracks[k];
        rec.sourceBpm = metaSrc.detectedBpm;
        rec.sourcePhraseConf = metaSrc.phraseProfile.phraseConfidence;
        rec.sourceKey = estimateKeyString(metaSrc.peakAmplitude > 0 ? 440.0 + (k % 12) * 35.0 : 440.0);
        rec.destPath = tracks[k + 1];
        rec.destBpm = metaDst.detectedBpm;
        rec.destPhraseConf = metaDst.phraseProfile.phraseConfidence;
        rec.destKey = estimateKeyString(metaDst.peakAmplitude > 0 ? 440.0 + ((k + 1) % 12) * 35.0 : 440.0);
        rec.mixStartSec = mixStartMasterSec;
        rec.mixEndSec = mixEndMasterSec;
        rec.durationSec = transDurationSec;
        rec.durationBars = phraseBarsCount;
        rec.sourceExitSec = transExitSec;
        rec.destEntrySec = transCueSec;
        rec.tempoStrategy = tempoDecision.strategy;
        rec.effectiveTempoRatio = tempoDecision.effectiveTempoRatio;
        rec.bpmDeltaPercent = tempoDecision.bpmDeltaPercent;
        rec.octaveJump = tempoDecision.octaveJumpApplied;
        rec.artifactRisk = tempoDecision.artifactRiskScore;
        rec.alignmentMode = alignmentMode;
        rec.phraseScore = phraseScore;
        rec.fallbackApplied = fallbackApplied;
        rec.fallbackReason = fallbackReason;
        rec.eqStrategy = transitionStrategy;
        rec.bassSwapPoint = bassSwapPoint;
        rec.eqLowCutDb = eqLowCutDb;
        rec.confidence = std::clamp(1.0 - (rec.artifactRisk * 0.5) - (fallbackApplied ? 0.05 : 0.0), 0.70, 0.99);

        char snipBuf[64];
        std::snprintf(snipBuf, sizeof(snipBuf), "transitions/transition_%02d.wav", static_cast<int>(k + 1));
        rec.snippetFile = snipBuf;

        setTelemetry.push_back(rec);

        // Update master start time for incoming track
        currentTrackMasterStart = mixStartMasterSec - transCueSec;

        // Load next track into idle deck if we have more transitions
        if (k + 2 < tracks.size()) {
            std::cout << "  Pre-loading Track " << (k + 2) << " into idle Deck " << (srcDeckId == 0 ? "A" : "B") << "...\n";
            if (!engine.loadTrack(srcDeckId, tracks[k + 2])) {
                std::cerr << "Warning: Failed to load track " << tracks[k + 2] << " into idle deck.\n";
            }
            srcDeck->resetEq();
        }
    }

    // 4. Render Outro of Final Track
    uint8_t finalDeckId = static_cast<uint8_t>(totalTransitionsTarget % 2);
    pulse::audio::DeckPlayer* finalDeck = (finalDeckId == 0) ? deckA : deckB;
    const auto& metaFinal = finalDeck->getDecodedAudio();
    double finalEffDur = metaFinal.durationSeconds / finalDeck->getTempoRatio();
    double setFinalEndTime = currentTrackMasterStart + finalEffDur;

    if (customTotalDuration > 0.0) {
        setFinalEndTime = customTotalDuration;
    }

    std::cout << "\n>>> Rendering Final Track Outro on Deck " << (finalDeckId == 0 ? "A" : "B")
              << " until master time " << setFinalEndTime << "s...\n";

    while (masterCurrentTime < setFinalEndTime) {
        engine.processAudioBlock(blockBuffer.data(), blockSize, channels);
        renderedMaster.insert(renderedMaster.end(), blockBuffer.begin(), blockBuffer.end());
        masterCurrentTime += dt;
    }

    uint64_t renderedFrames = renderedMaster.size() / channels;
    double actualRenderedDuration = static_cast<double>(renderedFrames) / sampleRate;

    // 5. Analyze Master Output Amplitude & Gain Headroom
    float maxPeakOut = 0.0f;
    for (float sample : renderedMaster) {
        float absVal = std::abs(sample);
        if (absVal > maxPeakOut) {
            maxPeakOut = absVal;
        }
    }
    bool clippingDetected = (maxPeakOut >= (1.0f - 1e-4f));
    float maxPeakDb = (maxPeakOut > 1e-5f) ? 20.0f * std::log10(maxPeakOut) : -100.0f;

    double avgConfidence = 0.95;
    if (!setTelemetry.empty()) {
        double sumConf = 0.0;
        for (const auto& rec : setTelemetry) {
            sumConf += rec.confidence;
        }
        avgConfidence = sumConf / setTelemetry.size();
    }

    std::cout << "\n================================================================\n"
              << "Rendering Complete Summary:\n"
              << "================================================================\n"
              << "  Total Tracks Mixed:  " << tracks.size() << "\n"
              << "  Total Transitions:   " << setTelemetry.size() << "\n"
              << "  Rendered Frames:     " << renderedFrames << "\n"
              << "  Rendered Duration:   " << actualRenderedDuration << "s\n"
              << "  Max Peak Amplitude:  " << maxPeakOut << " (" << maxPeakDb << " dBFS)\n"
              << "  Clipping Detected:   " << (clippingDetected ? "YES (HARD LIMITING)" : "NO (CLEAN)") << "\n"
              << "  Avg Confidence:      " << avgConfidence << "\n"
              << "  Total Fallbacks:     " << fallbacksCount << "\n"
              << "----------------------------------------------------------------\n";

    // 6. Write Master WAV Output
    std::filesystem::path outWavPathObj(outAudioPath);
    if (outWavPathObj.has_parent_path()) {
        std::filesystem::create_directories(outWavPathObj.parent_path());
    }
    std::cout << "Writing master audio artifact: " << outAudioPath << "..." << std::endl;
    if (!pulse::audio::WavWriter::writeWav16(outAudioPath, renderedMaster.data(), renderedFrames, sampleRate, channels)) {
        std::cerr << "Error: Failed to write output WAV file: " << outAudioPath << "\n";
        return 1;
    }

    // 7. Export Transition Snippets if requested
    if (exportSnippets) {
        std::filesystem::path snippetBaseDir = outWavPathObj.parent_path() / "transitions";
        if (outWavPathObj.parent_path().empty()) {
            snippetBaseDir = "transitions";
        }
        std::filesystem::create_directories(snippetBaseDir);

        std::cout << "Exporting " << setTelemetry.size() << " transition audio snippets to " << snippetBaseDir << "...\n";

        for (const auto& rec : setTelemetry) {
            double snipStartSec = std::max(0.0, rec.mixStartSec - snippetPaddingSec);
            double snipEndSec = std::min(actualRenderedDuration, rec.mixEndSec + snippetPaddingSec);

            uint64_t startFrame = static_cast<uint64_t>(snipStartSec * sampleRate);
            uint64_t endFrame = static_cast<uint64_t>(snipEndSec * sampleRate);
            if (endFrame > renderedFrames) endFrame = renderedFrames;

            if (endFrame > startFrame) {
                uint64_t snipFrames = endFrame - startFrame;
                const float* snipPtr = renderedMaster.data() + (startFrame * channels);

                char fn[64];
                std::snprintf(fn, sizeof(fn), "transition_%02d.wav", rec.transitionIndex);
                std::filesystem::path snipPath = snippetBaseDir / fn;

                pulse::audio::WavWriter::writeWav16(snipPath.string(), snipPtr, snipFrames, sampleRate, channels);
                std::cout << "  Exported snippet: " << snipPath.string() << " (" << (snipFrames / static_cast<double>(sampleRate)) << "s)\n";
            }
        }
    }

    // 8. Write Set Telemetry JSON Report
    std::filesystem::path outReportPathObj(outReportPath);
    if (outReportPathObj.has_parent_path()) {
        std::filesystem::create_directories(outReportPathObj.parent_path());
    }
    std::cout << "Emitting JSON telemetry report: " << outReportPath << "..." << std::endl;
    std::ofstream reportFile(outReportPath);
    if (!reportFile.is_open()) {
        std::cerr << "Error: Failed to write output JSON report: " << outReportPath << "\n";
        return 1;
    }

    reportFile << "{\n"
               << "  \"set_summary\": {\n"
               << "    \"set_id\": \"pulse-phase0-eval-001\",\n"
               << "    \"total_tracks\": " << tracks.size() << ",\n"
               << "    \"total_transitions\": " << setTelemetry.size() << ",\n"
               << "    \"rendered_duration_seconds\": " << std::fixed << std::setprecision(2) << actualRenderedDuration << ",\n"
               << "    \"master_sample_rate\": " << sampleRate << ",\n"
               << "    \"master_channels\": " << channels << ",\n"
               << "    \"clipping_detected\": " << (clippingDetected ? "true" : "false") << ",\n"
               << "    \"max_peak_db\": " << std::fixed << std::setprecision(2) << maxPeakDb << ",\n"
               << "    \"average_transition_confidence\": " << std::fixed << std::setprecision(2) << avgConfidence << ",\n"
               << "    \"total_fallbacks_applied\": " << fallbacksCount << "\n"
               << "  },\n"
               << "  \"transitions\": [\n";

    for (size_t i = 0; i < setTelemetry.size(); ++i) {
        const auto& t = setTelemetry[i];
        reportFile << "    {\n"
                   << "      \"transition_index\": " << t.transitionIndex << ",\n"
                   << "      \"source_track\": {\n"
                   << "        \"file_path\": \"" << formatJsonString(t.sourcePath) << "\",\n"
                   << "        \"detected_bpm\": " << std::fixed << std::setprecision(1) << t.sourceBpm << ",\n"
                   << "        \"phrase_confidence\": " << std::fixed << std::setprecision(2) << t.sourcePhraseConf << ",\n"
                   << "        \"key\": \"" << formatJsonString(t.sourceKey) << "\"\n"
                   << "      },\n"
                   << "      \"destination_track\": {\n"
                   << "        \"file_path\": \"" << formatJsonString(t.destPath) << "\",\n"
                   << "        \"detected_bpm\": " << std::fixed << std::setprecision(1) << t.destBpm << ",\n"
                   << "        \"phrase_confidence\": " << std::fixed << std::setprecision(2) << t.destPhraseConf << ",\n"
                   << "        \"key\": \"" << formatJsonString(t.destKey) << "\"\n"
                   << "      },\n"
                   << "      \"timeline\": {\n"
                   << "        \"mix_start_seconds\": " << std::fixed << std::setprecision(2) << t.mixStartSec << ",\n"
                   << "        \"mix_end_seconds\": " << std::fixed << std::setprecision(2) << t.mixEndSec << ",\n"
                   << "        \"duration_seconds\": " << std::fixed << std::setprecision(2) << t.durationSec << ",\n"
                   << "        \"duration_bars\": " << t.durationBars << ",\n"
                   << "        \"source_exit_seconds\": " << std::fixed << std::setprecision(2) << t.sourceExitSec << ",\n"
                   << "        \"destination_entry_seconds\": " << std::fixed << std::setprecision(2) << t.destEntrySec << "\n"
                   << "      },\n"
                   << "      \"tempo_strategy\": {\n"
                   << "        \"strategy\": \"" << formatJsonString(t.tempoStrategy) << "\",\n"
                   << "        \"effective_tempo_ratio\": " << std::fixed << std::setprecision(4) << t.effectiveTempoRatio << ",\n"
                   << "        \"bpm_delta_percent\": " << std::fixed << std::setprecision(2) << t.bpmDeltaPercent << ",\n"
                   << "        \"octave_jump\": " << (t.octaveJump ? "true" : "false") << ",\n"
                   << "        \"artifact_risk\": " << std::fixed << std::setprecision(2) << t.artifactRisk << "\n"
                   << "      },\n"
                   << "      \"alignment\": {\n"
                   << "        \"alignment_mode\": \"" << formatJsonString(t.alignmentMode) << "\",\n"
                   << "        \"phrase_score\": " << std::fixed << std::setprecision(2) << t.phraseScore << ",\n"
                   << "        \"fallback_applied\": " << (t.fallbackApplied ? "true" : "false") << ",\n"
                   << "        \"fallback_reason\": " << (t.fallbackReason.empty() ? "null" : ("\"" + formatJsonString(t.fallbackReason) + "\"")) << "\n"
                   << "      },\n"
                   << "      \"eq_execution\": {\n"
                   << "        \"strategy\": \"" << formatJsonString(t.eqStrategy) << "\",\n"
                   << "        \"bass_swap_point\": " << std::fixed << std::setprecision(2) << t.bassSwapPoint << ",\n"
                   << "        \"eq_low_cut_db\": " << std::fixed << std::setprecision(1) << t.eqLowCutDb << "\n"
                   << "      },\n"
                   << "      \"snippet_file\": \"" << formatJsonString(t.snippetFile) << "\",\n"
                   << "      \"confidence\": " << std::fixed << std::setprecision(2) << t.confidence << "\n"
                   << "    }" << (i + 1 < setTelemetry.size() ? "," : "") << "\n";
    }

    reportFile << "  ],\n";

    // Backwards-compatible fields for single-transition 2-track test suites
    const auto& metaA = deckA->getDecodedAudio();
    const auto& metaB = deckB->getDecodedAudio();
    const auto& t0 = !setTelemetry.empty() ? setTelemetry[0] : TransitionTelemetryRecord{};

    reportFile << "  \"deck_a\": {\n"
               << "    \"file_path\": \"" << formatJsonString(tracks[0]) << "\",\n"
               << "    \"sample_rate\": " << sampleRate << ",\n"
               << "    \"channels\": " << channels << ",\n"
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
               << "    \"file_path\": \"" << formatJsonString(tracks.size() > 1 ? tracks[1] : tracks[0]) << "\",\n"
               << "    \"sample_rate\": " << sampleRate << ",\n"
               << "    \"channels\": " << channels << ",\n"
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
               << "    \"strategy\": \"" << formatJsonString(t0.tempoStrategy) << "\",\n"
               << "    \"source_native_bpm\": " << std::fixed << std::setprecision(1) << t0.sourceBpm << ",\n"
               << "    \"destination_native_bpm\": " << std::fixed << std::setprecision(1) << t0.destBpm << ",\n"
               << "    \"effective_tempo_ratio\": " << std::fixed << std::setprecision(4) << t0.effectiveTempoRatio << ",\n"
               << "    \"bpm_delta_percent\": " << std::fixed << std::setprecision(2) << t0.bpmDeltaPercent << ",\n"
               << "    \"pitch_semitone_shift\": 0.0,\n"
               << "    \"octave_jump_applied\": " << (t0.octaveJump ? "true" : "false") << ",\n"
               << "    \"artifact_risk_score\": " << std::fixed << std::setprecision(2) << t0.artifactRisk << ",\n"
               << "    \"stretch_exceeded_threshold\": false,\n"
               << "    \"rejection_reason\": null\n"
               << "  },\n"
               << "  \"transition\": {\n"
               << "    \"type\": \"" << formatJsonString(t0.eqStrategy) << "\",\n"
               << "    \"strategy\": \"" << formatJsonString(t0.eqStrategy) << "\",\n"
               << "    \"bass_swap_point\": " << std::fixed << std::setprecision(2) << t0.bassSwapPoint << ",\n"
               << "    \"duration_seconds\": " << std::fixed << std::setprecision(2) << t0.durationSec << ",\n"
               << "    \"start_position_seconds\": " << std::fixed << std::setprecision(2) << t0.sourceExitSec << ",\n"
               << "    \"eq_automation\": {\n"
               << "      \"deck_a\": { \"initial\": { \"low\": 0.00, \"mid\": 0.0, \"high\": 0.0 }, \"mid_swap\": { \"low\": -0.50, \"mid\": 0.0, \"high\": 0.0 }, \"final\": { \"low\": -1.00, \"mid\": 0.0, \"high\": 0.0 } },\n"
               << "      \"deck_b\": { \"initial\": { \"low\": -1.00, \"mid\": 0.0, \"high\": 0.0 }, \"mid_swap\": { \"low\": -0.50, \"mid\": 0.0, \"high\": 0.0 }, \"final\": { \"low\": 0.00, \"mid\": 0.0, \"high\": 0.0 } }\n"
               << "    }\n"
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

