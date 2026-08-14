#pragma once

#include "AudioDecoder.h"
#include <string>
#include <vector>
#include <cstdint>

namespace pulse::audio {

/**
 * @brief Candidate transition window between outgoing Track A and incoming Track B.
 */
struct TransitionWindowCandidate {
    double sourceExitSec{0.0};            // Deck A exit timestamp (seconds)
    double destEntrySec{0.0};             // Deck B cue/entry timestamp (seconds)
    uint32_t durationBars{8};             // Transition duration in musical bars
    double durationSeconds{16.0};         // Effective transition duration (seconds)
    double phraseScore{0.0};              // [0.0, 1.0] phrase compatibility score
    bool isPhraseAligned{true};
    std::string alignmentStrategy{"phrase_aligned"}; // "phrase_aligned", "bar_fallback", "time_fallback"
    std::string fallbackReason{};
};

/**
 * @brief Aggregate transition plan result containing the selected window and alternative candidates.
 */
struct TransitionPlanResult {
    TransitionWindowCandidate selectedWindow;
    std::vector<TransitionWindowCandidate> candidateWindows;
    std::string transitionType{"phrase_crossfade"};
    float overallConfidence{0.0f};
};

/**
 * @brief Deterministic Transition Window Planner.
 */
class TransitionPlanner {
public:
    /**
     * @brief Plans a musically aligned transition window between Track A and Track B.
     * @param trackA Decoded metadata and beatgrid of outgoing track A.
     * @param trackB Decoded metadata and beatgrid of incoming track B.
     * @param effectiveRatioA Playback tempo ratio for Deck A (default: 1.0).
     * @param effectiveRatioB Playback tempo ratio for Deck B (default: 1.0).
     * @param preferredPhraseBars Preferred phrase length in bars (e.g. 8, 16, 4, 2, or 0 for auto).
     * @param minConfidenceThreshold Minimum phrase confidence required to apply phrase alignment (default: 0.5).
     * @param phraseAware Whether phrase-aware planning is enabled (if false, falls back to bar/time alignment).
     * @return TransitionPlanResult with selected window and candidate evaluations.
     */
    static TransitionPlanResult planTransition(
        const DecodedAudio& trackA,
        const DecodedAudio& trackB,
        double effectiveRatioA = 1.0,
        double effectiveRatioB = 1.0,
        uint32_t preferredPhraseBars = 8,
        double minConfidenceThreshold = 0.5,
        bool phraseAware = true
    );
};

} // namespace pulse::audio
