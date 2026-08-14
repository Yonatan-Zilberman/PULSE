#include "../include/TransitionPlanner.h"
#include <cmath>
#include <algorithm>

namespace pulse::audio {

TransitionPlanResult TransitionPlanner::planTransition(
    const DecodedAudio& trackA,
    const DecodedAudio& trackB,
    double effectiveRatioA,
    double effectiveRatioB,
    uint32_t preferredPhraseBars,
    double minConfidenceThreshold,
    bool phraseAware
) {
    TransitionPlanResult result;
    result.transitionType = "phrase_crossfade";

    // 1. Guard against empty or invalid audio buffers
    if (trackA.durationSeconds <= 0.0 || trackB.durationSeconds <= 0.0) {
        result.overallConfidence = 0.0f;
        result.selectedWindow.sourceExitSec = 0.0;
        result.selectedWindow.destEntrySec = 0.0;
        result.selectedWindow.durationBars = 1;
        result.selectedWindow.durationSeconds = 4.0;
        result.selectedWindow.phraseScore = 0.0;
        result.selectedWindow.isPhraseAligned = false;
        result.selectedWindow.alignmentStrategy = "time_fallback";
        result.selectedWindow.fallbackReason = "Empty or invalid audio input";
        result.candidateWindows.push_back(result.selectedWindow);
        return result;
    }

    // 2. Compute effective playback metrics
    double ratioA = (effectiveRatioA > 0.01) ? effectiveRatioA : 1.0;
    double ratioB = (effectiveRatioB > 0.01) ? effectiveRatioB : 1.0;

    double effDurA = trackA.durationSeconds / ratioA;
    double effDurB = trackB.durationSeconds / ratioB;

    double bpmA = (trackA.detectedBpm > 0.0) ? trackA.detectedBpm : 120.0;
    double targetBpm = bpmA * ratioA;
    if (targetBpm <= 1.0) targetBpm = 120.0;

    double secPerBeat = 60.0 / targetBpm;
    double secPerBar = 4.0 * secPerBeat;

    float confA = trackA.tempoProfile.phraseProfile.phraseConfidence;
    float confB = trackB.tempoProfile.phraseProfile.phraseConfidence;
    float overallConfidence = std::min(trackA.tempoProfile.confidence, trackB.tempoProfile.confidence);
    float combinedPhraseConf = std::min(confA, confB);
    result.overallConfidence = overallConfidence;

    // 3. Determine phrase search sequence
    std::vector<uint32_t> barHierarchy;
    if (preferredPhraseBars == 0) {
        barHierarchy = {16, 8, 4, 2};
    } else if (preferredPhraseBars >= 32) {
        barHierarchy = {32, 16, 8, 4, 2};
    } else if (preferredPhraseBars >= 16) {
        barHierarchy = {16, 8, 4, 2};
    } else if (preferredPhraseBars >= 8) {
        barHierarchy = {8, 4, 2};
    } else if (preferredPhraseBars >= 4) {
        barHierarchy = {4, 2};
    } else {
        barHierarchy = {2};
    }

    bool phraseFound = false;
    double destEntrySec = 0.0;
    if (!trackB.tempoProfile.downbeatPositions.empty()) {
        destEntrySec = trackB.tempoProfile.downbeatPositions.front() / ratioB;
    }

    // 4. Phrase-Aware Window Search
    if (phraseAware && combinedPhraseConf >= static_cast<float>(minConfidenceThreshold)) {
        for (uint32_t bars : barHierarchy) {
            double transDurSec = static_cast<double>(bars) * secPerBar;

            // Ensure window fits within both track durations
            if (transDurSec > effDurA || transDurSec > effDurB) {
                continue;
            }

            // Retrieve candidate phrase boundary timestamps for Track A
            std::vector<double> boundaries;
            if (bars == 32 && !trackA.tempoProfile.phraseProfile.boundaries32Bar.empty()) {
                boundaries = trackA.tempoProfile.phraseProfile.boundaries32Bar;
            } else if (bars == 16 && !trackA.tempoProfile.phraseProfile.boundaries16Bar.empty()) {
                boundaries = trackA.tempoProfile.phraseProfile.boundaries16Bar;
            } else if (bars == 8 && !trackA.tempoProfile.phraseProfile.boundaries8Bar.empty()) {
                boundaries = trackA.tempoProfile.phraseProfile.boundaries8Bar;
            } else if (bars == 4 && !trackA.tempoProfile.phraseProfile.boundaries4Bar.empty()) {
                boundaries = trackA.tempoProfile.phraseProfile.boundaries4Bar;
            } else {
                // Generate from barPositions if exact profile vector is empty
                for (size_t b = 0; b < trackA.tempoProfile.barPositions.size(); ++b) {
                    if (b % bars == 0) {
                        boundaries.push_back(trackA.tempoProfile.barPositions[b]);
                    }
                }
            }

            // Find valid exit points leaving transDurSec before end of track A
            std::vector<double> validExitPoints;
            for (double bSec : boundaries) {
                double scaledExit = bSec / ratioA;
                if (scaledExit + transDurSec <= effDurA + 0.05) {
                    validExitPoints.push_back(scaledExit);
                }
            }

            if (!validExitPoints.empty()) {
                // Sort and pick latest exit boundary for primary window (outro mix)
                std::sort(validExitPoints.begin(), validExitPoints.end());
                double bestExitSec = validExitPoints.back();

                double phraseScore = static_cast<double>(combinedPhraseConf);
                if (preferredPhraseBars > 0 && bars < preferredPhraseBars) {
                    double penalty = 0.05 * (static_cast<double>(preferredPhraseBars) / static_cast<double>(bars));
                    phraseScore = std::max(0.60, phraseScore - penalty);
                }

                std::string fallbackReason = "";
                if (preferredPhraseBars > 0 && bars < preferredPhraseBars) {
                    fallbackReason = "Track length insufficient for requested " +
                        std::to_string(preferredPhraseBars) + "-bar phrase; stepped down to " +
                        std::to_string(bars) + " bars";
                }

                TransitionWindowCandidate cand;
                cand.sourceExitSec = bestExitSec;
                cand.destEntrySec = destEntrySec;
                cand.durationBars = bars;
                cand.durationSeconds = transDurSec;
                cand.phraseScore = std::clamp(phraseScore, 0.0, 1.0);
                cand.isPhraseAligned = true;
                cand.alignmentStrategy = "phrase_aligned";
                cand.fallbackReason = fallbackReason;

                if (!phraseFound) {
                    result.selectedWindow = cand;
                    phraseFound = true;
                }
                result.candidateWindows.push_back(cand);

                // Add alternative earlier exit points as secondary candidates
                for (int e = static_cast<int>(validExitPoints.size()) - 2; e >= 0 && result.candidateWindows.size() < 4; --e) {
                    TransitionWindowCandidate altCand = cand;
                    altCand.sourceExitSec = validExitPoints[e];
                    result.candidateWindows.push_back(altCand);
                }

                // If primary requested bar length matched, break early
                if (preferredPhraseBars == 0 || bars == preferredPhraseBars) {
                    break;
                }
            }
        }
    }

    // 5. Fallback Policy: Downbeat Bar Snapping or Time Fallback
    if (!phraseFound) {
        std::string fallbackReason;
        if (!phraseAware) {
            fallbackReason = "Phrase alignment disabled by user configuration";
        } else if (combinedPhraseConf < static_cast<float>(minConfidenceThreshold)) {
            fallbackReason = "Phrase confidence below minimum threshold (" + std::to_string(minConfidenceThreshold) + ")";
        } else {
            fallbackReason = "Track length insufficient for phrase-aligned transition";
        }

        // Fallback duration: 4.0 seconds (or 2 bars / half track duration)
        double fallbackDurSec = std::min(4.0, effDurA * 0.5);
        if (fallbackDurSec < 0.1) fallbackDurSec = 0.1;

        double exitSec = std::max(0.0, effDurA - fallbackDurSec);
        std::string alignStrat = "time_fallback";

        if (!trackA.tempoProfile.downbeatPositions.empty()) {
            alignStrat = "bar_fallback";
            double bestDownbeat = trackA.tempoProfile.downbeatPositions.front() / ratioA;
            for (double db : trackA.tempoProfile.downbeatPositions) {
                double scaledDb = db / ratioA;
                if (scaledDb <= exitSec) {
                    bestDownbeat = scaledDb;
                } else {
                    break;
                }
            }
            exitSec = bestDownbeat;
        }

        uint32_t fallbackBars = std::max(1u, static_cast<uint32_t>(std::round(fallbackDurSec / secPerBar)));

        TransitionWindowCandidate fallbackCand;
        fallbackCand.sourceExitSec = exitSec;
        fallbackCand.destEntrySec = destEntrySec;
        fallbackCand.durationBars = fallbackBars;
        fallbackCand.durationSeconds = fallbackDurSec;
        fallbackCand.phraseScore = (alignStrat == "bar_fallback") ? 0.50 : 0.20;
        fallbackCand.isPhraseAligned = false;
        fallbackCand.alignmentStrategy = alignStrat;
        fallbackCand.fallbackReason = fallbackReason;

        result.selectedWindow = fallbackCand;
        result.candidateWindows = {fallbackCand};
    }

    return result;
}

} // namespace pulse::audio
