#include "../include/TempoStrategy.h"
#include <cmath>
#include <algorithm>

namespace pulse::audio {

TempoStrategyDecision TempoStrategy::evaluate(
    double bpmA,
    double bpmB,
    const std::vector<double>& altA,
    const std::vector<double>& altB,
    TempoStrategyMode requestedMode,
    double masterTargetBpm,
    double maxStretchPct,
    bool forceStretch
) {
    TempoStrategyDecision decision;
    decision.sourceNativeBpm = (bpmA > 0.0) ? bpmA : 120.0;
    decision.destNativeBpm = (bpmB > 0.0) ? bpmB : 120.0;
    decision.pitchSemitoneShift = 0.0f;

    // 1. Octave Compatibility Evaluation (Half-Time / Double-Time)
    // E.g. 70 BPM vs 140 BPM (ratio ~2.0 or ~0.5)
    bool isOctaveCompatible = false;
    double doubleBpmB = decision.destNativeBpm * 2.0;
    double halfBpmB = decision.destNativeBpm * 0.5;

    if (std::abs(doubleBpmB - decision.sourceNativeBpm) / decision.sourceNativeBpm <= 0.03 ||
        std::abs(halfBpmB - decision.sourceNativeBpm) / decision.sourceNativeBpm <= 0.03) {
        isOctaveCompatible = true;
    }

    // Also check alternative hypotheses
    for (double alt : altB) {
        if (std::abs(alt - decision.sourceNativeBpm) / decision.sourceNativeBpm <= 0.03) {
            isOctaveCompatible = true;
            break;
        }
    }
    for (double alt : altA) {
        if (std::abs(alt - decision.destNativeBpm) / decision.destNativeBpm <= 0.03) {
            isOctaveCompatible = true;
            break;
        }
    }

    if (isOctaveCompatible && (requestedMode == TempoStrategyMode::Auto || requestedMode == TempoStrategyMode::Source)) {
        decision.strategy = "octave_match";
        decision.octaveJumpApplied = true;
        decision.effectiveDeckATempoRatio = 1.0;
        decision.effectiveDeckBTempoRatio = 1.0;
        decision.effectiveTempoRatio = 1.0;
        decision.bpmDeltaPercent = 0.0;
        decision.artifactRiskScore = 0.05f;
        decision.stretchExceededThreshold = false;
        decision.rejectionReason = "";
        decision.recommendedTransitionType = "breakdown_blend";
        return decision;
    }

    // 2. Mode Configuration
    TempoStrategyMode mode = (requestedMode == TempoStrategyMode::Auto) ? TempoStrategyMode::Source : requestedMode;

    switch (mode) {
        case TempoStrategyMode::None:
            decision.strategy = "none";
            decision.effectiveDeckATempoRatio = 1.0;
            decision.effectiveDeckBTempoRatio = 1.0;
            decision.effectiveTempoRatio = 1.0;
            break;

        case TempoStrategyMode::Dest:
            decision.strategy = "match_dest";
            decision.effectiveDeckATempoRatio = decision.destNativeBpm / decision.sourceNativeBpm;
            decision.effectiveDeckBTempoRatio = 1.0;
            decision.effectiveTempoRatio = decision.effectiveDeckATempoRatio;
            break;

        case TempoStrategyMode::Master: {
            decision.strategy = "match_master";
            double target = (masterTargetBpm > 0.0) ? masterTargetBpm : decision.sourceNativeBpm;
            decision.effectiveDeckATempoRatio = target / decision.sourceNativeBpm;
            decision.effectiveDeckBTempoRatio = target / decision.destNativeBpm;
            decision.effectiveTempoRatio = decision.effectiveDeckBTempoRatio;
            break;
        }

        case TempoStrategyMode::Source:
        case TempoStrategyMode::Auto:
        default:
            decision.strategy = "match_source";
            decision.effectiveDeckATempoRatio = 1.0;
            decision.effectiveDeckBTempoRatio = decision.sourceNativeBpm / decision.destNativeBpm;
            decision.effectiveTempoRatio = decision.effectiveDeckBTempoRatio;
            break;
    }

    // 3. Delta Calculation
    double primaryRatio = decision.effectiveTempoRatio;
    double deltaNorm = std::abs(primaryRatio - 1.0);
    decision.bpmDeltaPercent = deltaNorm * 100.0;

    double maxStretchThreshold = std::max(0.01, maxStretchPct / 100.0);

    // 4. Four-Tier Policy Evaluation
    if (deltaNorm <= 0.03) {
        // Tier 1: Small Delta (<= 3.0%)
        decision.artifactRiskScore = static_cast<float>(std::min(0.05, (deltaNorm / 0.03) * 0.05));
        decision.stretchExceededThreshold = false;
        decision.rejectionReason = "";
        decision.recommendedTransitionType = "eq_crossfade";
    } else if (deltaNorm <= 0.06) {
        // Tier 2: Moderate Delta (3.0% to 6.0%)
        double excess = (deltaNorm - 0.03) / 0.03; // [0.0, 1.0]
        decision.artifactRiskScore = static_cast<float>(0.15 + excess * excess * 0.25); // [0.15, 0.40]
        decision.stretchExceededThreshold = false;
        decision.rejectionReason = "";
        decision.recommendedTransitionType = "eq_crossfade";
    } else {
        // Tier 3: Excessive Delta (> 6.0%)
        double excess = deltaNorm - 0.06;
        decision.artifactRiskScore = static_cast<float>(std::min(1.0, 0.70 + excess * 2.0));

        if (deltaNorm > maxStretchThreshold && !forceStretch) {
            decision.stretchExceededThreshold = true;
            decision.rejectionReason = "BPM delta exceeds maximum allowable time-stretch limit (6.0%). Direct tempo stretching rejected to prevent audio degradation.";
            decision.recommendedTransitionType = "energy_cut";
        } else {
            decision.stretchExceededThreshold = false;
            decision.rejectionReason = "";
            decision.recommendedTransitionType = "eq_crossfade";
        }
    }

    return decision;
}

TempoStrategyMode TempoStrategy::parseModeString(const std::string& modeStr) {
    if (modeStr == "source" || modeStr == "match_source") return TempoStrategyMode::Source;
    if (modeStr == "dest" || modeStr == "match_dest") return TempoStrategyMode::Dest;
    if (modeStr == "master" || modeStr == "match_master") return TempoStrategyMode::Master;
    if (modeStr == "none" || modeStr == "disable") return TempoStrategyMode::None;
    return TempoStrategyMode::Auto;
}

std::string TempoStrategy::modeToString(TempoStrategyMode mode) {
    switch (mode) {
        case TempoStrategyMode::Source: return "match_source";
        case TempoStrategyMode::Dest: return "match_dest";
        case TempoStrategyMode::Master: return "match_master";
        case TempoStrategyMode::None: return "none";
        case TempoStrategyMode::Auto: return "auto";
    }
    return "match_source";
}

} // namespace pulse::audio
