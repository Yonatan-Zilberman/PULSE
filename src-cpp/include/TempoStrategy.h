#pragma once

#include <string>
#include <vector>

namespace pulse::audio {

/**
 * @brief Tempo matching strategy modes.
 */
enum class TempoStrategyMode {
    Source,   // Stretch Deck B to match Deck A's native tempo
    Dest,     // Stretch Deck A to match Deck B's native tempo
    Master,   // Stretch both decks to target master BPM
    None,     // 1.0x native rate on both decks
    Auto      // Automatically select strategy based on delta and octave compatibility
};

/**
 * @brief Structured decision emitted by the bounded tempo strategy engine.
 */
struct TempoStrategyDecision {
    std::string strategy{"match_source"};
    double sourceNativeBpm{120.0};
    double destNativeBpm{120.0};
    double effectiveDeckATempoRatio{1.0};
    double effectiveDeckBTempoRatio{1.0};
    double effectiveTempoRatio{1.0};
    double bpmDeltaPercent{0.0};
    float pitchSemitoneShift{0.0f}; // Strictly 0.0f for pitch preservation
    bool octaveJumpApplied{false};
    float artifactRiskScore{0.0f};  // [0.0, 1.0]
    bool stretchExceededThreshold{false};
    std::string rejectionReason{};
    std::string recommendedTransitionType{"eq_crossfade"};
};

/**
 * @brief Bounded tempo strategy decision engine.
 */
class TempoStrategy {
public:
    /**
     * @brief Evaluates tempo compatibility between source (Deck A) and destination (Deck B).
     * @param bpmA Native BPM of source track A.
     * @param bpmB Native BPM of destination track B.
     * @param altA Alternative tempo hypotheses for track A.
     * @param altB Alternative tempo hypotheses for track B.
     * @param requestedMode Desired tempo strategy mode.
     * @param masterTargetBpm Target BPM when Master mode is selected.
     * @param maxStretchPct Maximum allowable tempo stretch percentage before rejection (default: 6.0%).
     * @param forceStretch If true, overrides extreme stretch rejection for testing.
     * @return Evaluated TempoStrategyDecision struct.
     */
    static TempoStrategyDecision evaluate(
        double bpmA,
        double bpmB,
        const std::vector<double>& altA = {},
        const std::vector<double>& altB = {},
        TempoStrategyMode requestedMode = TempoStrategyMode::Source,
        double masterTargetBpm = 0.0,
        double maxStretchPct = 6.0,
        bool forceStretch = false
    );

    /**
     * @brief Parses tempo strategy mode string into enum.
     */
    static TempoStrategyMode parseModeString(const std::string& modeStr);

    /**
     * @brief Serializes tempo strategy mode enum into canonical string.
     */
    static std::string modeToString(TempoStrategyMode mode);
};

} // namespace pulse::audio
