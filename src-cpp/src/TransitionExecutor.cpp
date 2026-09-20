#include "../include/TransitionExecutor.h"
#include "../include/DeckPlayer.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pulse::audio {

void PhraseCrossfadeStrategy::update(double normalizedProgress, const TransitionCommandC& command, Mixer& mixer, DeckPlayer* deckA, DeckPlayer* deckB) noexcept {
    float startX = (command.source_deck == 0) ? -1.0f : 1.0f;
    float endX = (command.destination_deck == 1) ? 1.0f : -1.0f;

    double p = std::clamp(normalizedProgress, 0.0, 1.0);
    float currentX = static_cast<float>(startX + (endX - startX) * p);
    mixer.setCrossfader(currentX);

    if (deckA) deckA->setEq(0.0f, 0.0f, 0.0f);
    if (deckB) deckB->setEq(0.0f, 0.0f, 0.0f);
}

void EqCrossfadeStrategy::update(double normalizedProgress, const TransitionCommandC& command, Mixer& mixer, DeckPlayer* deckA, DeckPlayer* deckB) noexcept {
    float startX = (command.source_deck == 0) ? -1.0f : 1.0f;
    float endX = (command.destination_deck == 1) ? 1.0f : -1.0f;

    double p = std::clamp(normalizedProgress, 0.0, 1.0);
    float currentX = static_cast<float>(startX + (endX - startX) * p);
    mixer.setCrossfader(currentX);

    // Monotonic low-frequency crossover ramp
    // Source deck low: 0.0 (0 dB) -> -1.0 (kill)
    // Dest deck low: -1.0 (kill) -> 0.0 (0 dB)
    float lowSrc = -static_cast<float>(p);
    float lowDst = static_cast<float>(p - 1.0);

    DeckPlayer* src = (command.source_deck == 0) ? deckA : deckB;
    DeckPlayer* dst = (command.destination_deck == 1) ? deckB : deckA;

    if (src) src->setEq(lowSrc, 0.0f, 0.0f);
    if (dst) dst->setEq(lowDst, 0.0f, 0.0f);
}

void BassSwapStrategy::update(double normalizedProgress, const TransitionCommandC& command, Mixer& mixer, DeckPlayer* deckA, DeckPlayer* deckB) noexcept {
    float startX = (command.source_deck == 0) ? -1.0f : 1.0f;
    float endX = (command.destination_deck == 1) ? 1.0f : -1.0f;

    double p = std::clamp(normalizedProgress, 0.0, 1.0);
    float currentX = static_cast<float>(startX + (endX - startX) * p);
    mixer.setCrossfader(currentX);

    double halfWindow = swapWindow_ * 0.5;
    double swapStart = swapPoint_ - halfWindow;
    double swapEnd = swapPoint_ + halfWindow;

    float gainSrcLow = 1.0f;
    float gainDstLow = 0.0f;

    if (p <= swapStart) {
        gainSrcLow = 1.0f;
        gainDstLow = 0.0f;
    } else if (p >= swapEnd) {
        gainSrcLow = 0.0f;
        gainDstLow = 1.0f;
    } else {
        // Smooth Hermite smoothstep swap curve
        double u = (p - swapStart) / swapWindow_;
        float s = static_cast<float>(u * u * (3.0 - 2.0 * u));
        gainSrcLow = 1.0f - s;
        gainDstLow = s;
    }

    // Map linear gains [0.0, 1.0] to EQ parameters [-1.0, 0.0]
    // where gain = 1.0 + eqVal  =>  eqVal = gain - 1.0
    float lowEqSrc = std::clamp(gainSrcLow - 1.0f, -1.0f, 0.0f);
    float lowEqDst = std::clamp(gainDstLow - 1.0f, -1.0f, 0.0f);

    DeckPlayer* src = (command.source_deck == 0) ? deckA : deckB;
    DeckPlayer* dst = (command.destination_deck == 1) ? deckB : deckA;

    if (src) src->setEq(lowEqSrc, 0.0f, 0.0f);
    if (dst) dst->setEq(lowEqDst, 0.0f, 0.0f);
}

namespace detail {

constexpr double kPi = 3.14159265358979323846;

// Hermite smoothstep of x clamped to [0,1]
inline float smoothstep01(double x) noexcept {
    double c = std::clamp(x, 0.0, 1.0);
    return static_cast<float>(c * c * (3.0 - 2.0 * c));
}

// Hermite smoothstep over an arbitrary window [a, b]; degenerate windows snap to a step.
inline float smoothstepAB(double x, double a, double b) noexcept {
    if (b <= a) {
        return (x >= b) ? 1.0f : 0.0f;
    }
    return smoothstep01((x - a) / (b - a));
}

// Clamp a possibly non-finite value to [lo, hi]; non-finite input maps to `def`.
inline float sanitizeClamp(float v, double lo, double hi, float def) noexcept {
    if (!std::isfinite(v)) {
        return def;
    }
    return static_cast<float>(std::clamp(static_cast<double>(v), lo, hi));
}

/**
 * Sanitization contract for the v2 full-plan command.
 *
 * - Every non-finite (NaN / +/-Inf) field is replaced by its documented safe
 *   default, then clamped to its documented bound.
 * - Phases are forced monotonic: 0 <= sync <= eq <= vocal <= 1.
 * - Structural rejection (return false): source_deck > 1, destination_deck > 1,
 *   or source_deck == destination_deck.
 * - `version != 1` is NOT a rejection: every field is sanitized to safe
 *   defaults and the transition still executes (fail-safe).
 */
inline bool sanitizePlan(const TransitionCommandC& in, TransitionCommandC& out) noexcept {
    const bool versionOk = (in.version == 1);

    // Structural validation runs on the raw deck ids (8-bit, cannot be NaN).
    if (in.source_deck > 1 || in.destination_deck > 1 || in.source_deck == in.destination_deck) {
        return false;
    }

    out = in;
    if (!versionOk) {
        // Unknown layout: fail safe with all documented defaults.
        out.version = 1u;
        out.source_deck = 0;
        out.destination_deck = 1;
        out.crossfader_curve = 0;
        out.flags = 0;
        out._pad0 = 0.0f;
        out.duration_seconds = 16.0;
        out.src_tempo_ratio = 1.0f;
        out.dst_tempo_ratio = 1.0f;
        out.dst_tempo_ramp_seconds = 0.0f;
        out.src_gain = 1.0f;
        out.dst_gain = 1.0f;
        out.src_low_eq = 0.0f;
        out.src_mid_eq = 0.0f;
        out.src_high_eq = 0.0f;
        out.dst_low_eq = 0.0f;
        out.dst_mid_eq = 0.0f;
        out.dst_high_eq = 0.0f;
        out.src_filter = 0.0f;
        out.dst_filter = 0.0f;
        out.src_vocal_stem = 1.0f;
        out.dst_vocal_stem = 1.0f;
        out.crossfader_start = -1.0f;
        out.crossfader_end = 1.0f;
        out.phase_sync_end = 0.5f;
        out.phase_eq_end = 0.875f;
        out.phase_vocal_end = 1.0f;
        out.bass_swap_point = 0.5f;
        out.bass_swap_window = 0.1f;
        out.transition_type = static_cast<uint32_t>(TransitionStrategyType::PhraseCrossfade);
        out._pad1 = 0u;
        return true;
    }

    out._pad0 = 0.0f;
    out._pad1 = 0u;

    out.crossfader_curve = std::isfinite(static_cast<double>(in.crossfader_curve))
        ? static_cast<uint8_t>(std::clamp(static_cast<int>(in.crossfader_curve), 0, 2))
        : 0u;
    out.flags = std::isfinite(static_cast<double>(in.flags)) ? in.flags : 0u;

    out.duration_seconds = std::isfinite(in.duration_seconds)
        ? std::clamp(in.duration_seconds, 0.1, 600.0)
        : 16.0;

    out.src_tempo_ratio = sanitizeClamp(in.src_tempo_ratio, 0.5, 2.0, 1.0f);
    out.dst_tempo_ratio = sanitizeClamp(in.dst_tempo_ratio, 0.5, 2.0, 1.0f);
    out.dst_tempo_ramp_seconds = sanitizeClamp(in.dst_tempo_ramp_seconds, 0.0, 300.0, 0.0f);
    out.src_gain = sanitizeClamp(in.src_gain, 0.0, 1.0, 1.0f);
    out.dst_gain = sanitizeClamp(in.dst_gain, 0.0, 1.0, 1.0f);

    out.src_low_eq = sanitizeClamp(in.src_low_eq, -1.0, 1.0, 0.0f);
    out.src_mid_eq = sanitizeClamp(in.src_mid_eq, -1.0, 1.0, 0.0f);
    out.src_high_eq = sanitizeClamp(in.src_high_eq, -1.0, 1.0, 0.0f);
    out.dst_low_eq = sanitizeClamp(in.dst_low_eq, -1.0, 1.0, 0.0f);
    out.dst_mid_eq = sanitizeClamp(in.dst_mid_eq, -1.0, 1.0, 0.0f);
    out.dst_high_eq = sanitizeClamp(in.dst_high_eq, -1.0, 1.0, 0.0f);
    out.src_filter = sanitizeClamp(in.src_filter, -1.0, 1.0, 0.0f);
    out.dst_filter = sanitizeClamp(in.dst_filter, -1.0, 1.0, 0.0f);

    out.src_vocal_stem = sanitizeClamp(in.src_vocal_stem, 0.0, 1.0, 1.0f);
    out.dst_vocal_stem = sanitizeClamp(in.dst_vocal_stem, 0.0, 1.0, 1.0f);

    out.crossfader_start = sanitizeClamp(in.crossfader_start, -1.0, 1.0, -1.0f);
    out.crossfader_end = sanitizeClamp(in.crossfader_end, -1.0, 1.0, 1.0f);

    out.phase_sync_end = sanitizeClamp(in.phase_sync_end, 0.0, 1.0, 0.5f);
    out.phase_eq_end = sanitizeClamp(in.phase_eq_end, 0.0, 1.0, 0.875f);
    out.phase_vocal_end = sanitizeClamp(in.phase_vocal_end, 0.0, 1.0, 1.0f);

    // Force monotonic phase boundaries: 0 <= sync <= eq <= vocal <= 1.
    double p1 = static_cast<double>(out.phase_sync_end);
    double p2 = std::max(p1, static_cast<double>(out.phase_eq_end));
    double p3 = std::max(p2, static_cast<double>(out.phase_vocal_end));
    p2 = std::min(p2, p3);
    out.phase_sync_end = static_cast<float>(std::clamp(p1, 0.0, 1.0));
    out.phase_eq_end = static_cast<float>(std::clamp(p2, 0.0, 1.0));
    out.phase_vocal_end = static_cast<float>(std::clamp(p3, 0.0, 1.0));

    out.bass_swap_point = sanitizeClamp(in.bass_swap_point, 0.1, 0.9, 0.5f);
    out.bass_swap_window = sanitizeClamp(in.bass_swap_window, 0.02, 0.5, 0.1f);
    // Unknown transition types keep their raw value; strategy selection maps them to PhraseCrossfade.
    return true;
}

} // namespace detail

void ClassicEqBlendStrategy::update(double normalizedProgress, const TransitionCommandC& cmd, Mixer& mixer, DeckPlayer* deckA, DeckPlayer* deckB) noexcept {
    DeckPlayer* srcP = (cmd.source_deck == 0) ? deckA : deckB;
    DeckPlayer* dstP = (cmd.destination_deck == 0) ? deckA : deckB;

    const double p = normalizedProgress; // may exceed 1.0 during the phase-4 ramp
    const double p1 = static_cast<double>(cmd.phase_sync_end);
    const double p2 = static_cast<double>(cmd.phase_eq_end);
    const double p3 = static_cast<double>(cmd.phase_vocal_end);
    const bool srcIsA = (cmd.source_deck == 0);

    // Crossfader endpoints (custom when flagged, else derived from deck ids).
    const float startX = (cmd.flags & 1u) ? cmd.crossfader_start : (srcIsA ? -1.0f : 1.0f);
    const float endX = (cmd.flags & 1u) ? cmd.crossfader_end : (srcIsA ? 1.0f : -1.0f);

    // Bass handoff window (stateless smoothstep).
    const double halfWindow = static_cast<double>(cmd.bass_swap_window) * 0.5;
    const double swapStart = static_cast<double>(cmd.bass_swap_point) - halfWindow;
    const double swapEnd = static_cast<double>(cmd.bass_swap_point) + halfWindow;
    const float sLow = detail::smoothstepAB(p, swapStart, swapEnd);

    // Fader trajectory u in [0,1] after the silent sync phase, shaped by the plan curve.
    const float u = detail::smoothstep01((p - p1) / ((1.0 - p1) > 1e-6 ? (1.0 - p1) : 1.0));
    // Source deck weight per the plan's curve (equal-power: cos/sin; linear: 1-u; s-curve: 1-u^2(3-2u)).
    double srcW = 1.0;
    switch (static_cast<CrossfaderCurveType>(cmd.crossfader_curve)) {
        case CrossfaderCurveType::Linear:
            srcW = 1.0 - static_cast<double>(u);
            break;
        case CrossfaderCurveType::SCurve: {
            const double us = static_cast<double>(u);
            srcW = 1.0 - us * us * (3.0 - 2.0 * us);
            break;
        }
        case CrossfaderCurveType::EqualPower:
        default:
            srcW = std::cos(static_cast<double>(u) * 0.5 * detail::kPi);
            break;
    }
    const float v = static_cast<float>(1.0 - srcW); // 0 at start endpoint, 1 at end endpoint
    const float xfadePos = startX + (endX - startX) * v;

    if (p < p1) {
        // ---- Phase 1: Sync & Phase Align — destination silent, tempos matched.
        mixer.setCrossfader(startX);
        if (srcP) srcP->setTempoRatio(static_cast<double>(cmd.src_tempo_ratio));
        if (dstP) dstP->setTempoRatio(static_cast<double>(cmd.dst_tempo_ratio));
        if (srcP) srcP->setVolume(cmd.src_gain);
        if (dstP) {
            const double denom = (p1 > 1e-6) ? p1 : 1.0;
            dstP->setVolume(cmd.dst_gain * detail::smoothstepAB(p, 0.0, denom));
        }
    } else if (p < 1.0) {
        // ---- Phase 2: EQ Blend & Swap — fader move, HPF'd bass handoff, gain staging.
        mixer.setCrossfader(xfadePos);
        if (srcP) srcP->setVolume(cmd.src_gain);
        if (dstP) dstP->setVolume(cmd.dst_gain * detail::smoothstepAB(p, 0.0, (p1 > 1e-6 ? p1 : 1.0)));

        // Sequenced low-frequency handoff: src low 0 -> -1, dst low -1 -> 0 (HPF'd pre-swap).
        const float lowSrc = -sLow;
        const float lowDst = sLow - 1.0f;

        // Mid/high EQ + filter automation: neutral at u=0, plan targets at u=1.
        const float srcMid = static_cast<float>(static_cast<double>(cmd.src_mid_eq) * u);
        const float srcHigh = static_cast<float>(static_cast<double>(cmd.src_high_eq) * u);
        const float dstMid = static_cast<float>(static_cast<double>(cmd.dst_mid_eq) * u);
        const float dstHigh = static_cast<float>(static_cast<double>(cmd.dst_high_eq) * u);
        const float srcFilter = static_cast<float>(static_cast<double>(cmd.src_filter) * u);
        const float dstFilter = static_cast<float>(static_cast<double>(cmd.dst_filter) * u);

        if (srcP) {
            srcP->setEq(lowSrc, srcMid, srcHigh);
            srcP->setFilter(srcFilter);
        }
        if (dstP) {
            dstP->setEq(lowDst, dstMid, dstHigh);
            dstP->setFilter(dstFilter);
        }

        // ---- Phase 3: Vocal/Stem Isolate (tail of phase 2 window: [p2, p3]).
        if (p >= p2) {
            const double vEnd = std::max(p3, p2 + 0.001);
            const float vVal = detail::smoothstepAB(p, p2, vEnd);
            const float srcVocal = static_cast<float>(1.0 + (static_cast<double>(cmd.src_vocal_stem) - 1.0) * vVal);
            const float dstVocal = static_cast<float>(1.0 + (static_cast<double>(cmd.dst_vocal_stem) - 1.0) * vVal);
            if (srcP) srcP->setStemLevels(srcVocal, 1.0f, 1.0f, 1.0f);
            if (dstP) dstP->setStemLevels(dstVocal, 1.0f, 1.0f, 1.0f);
        }
    } else {
        // ---- Phase 4: Cut & Tempo Ramp — source cut, destination returns to native tempo.
        mixer.setCrossfader(endX);
        if (srcP) srcP->setVolume(0.0f);
        if (dstP) {
            dstP->setVolume(cmd.dst_gain);
            const double rampSec = static_cast<double>(cmd.dst_tempo_ramp_seconds);
            if (rampSec > 0.0 && cmd.duration_seconds > 0.0) {
                const double w = std::clamp((p - 1.0) * (cmd.duration_seconds / rampSec), 0.0, 1.0);
                const double t = static_cast<double>(cmd.dst_tempo_ratio) + (1.0 - static_cast<double>(cmd.dst_tempo_ratio)) * w;
                dstP->setTempoRatio(t);
            }
        }
    }
}

TransitionExecutor::TransitionExecutor() = default;

int TransitionExecutor::startTransition(const TransitionCommandC& command) {
    TransitionCommandC safe{};
    if (!detail::sanitizePlan(command, safe)) {
        // Structurally malformed: reject, apply nothing, stay as-is.
        return -1;
    }

    // Seqlock write: odd = in-progress, even = valid.
    pendingSeq_.store(1u, std::memory_order_relaxed);
    pendingCmd_ = safe;
    pendingSeq_.store(2u, std::memory_order_seq_cst);
    return 0;
}

void TransitionExecutor::captureBaselines(DeckPlayer* deckA, DeckPlayer* deckB) noexcept {
    const DeckPlayer* decks[2] = {deckA, deckB};
    for (int i = 0; i < 2; ++i) {
        DeckBaseline& b = baselines_[i];
        const DeckPlayer* d = decks[i];
        if (!d) {
            b = DeckBaseline{};
            continue;
        }
        b.volume = d->getVolume();
        b.low = d->getEqLow();
        b.mid = d->getEqMid();
        b.high = d->getEqHigh();
        b.filter = d->getFilter();
        b.vocal = d->getVocalStem();
        b.drum = d->getDrumStem();
        b.bass = d->getBassStem();
        b.other = d->getOtherStem();
    }
}

void TransitionExecutor::finishTransition(Mixer& mixer, DeckPlayer* deckA, DeckPlayer* deckB) noexcept {
    const TransitionCommandC& c = activeCommand_;

    // Crossfader parks at the plan's final endpoint (custom when flagged).
    const bool srcIsA = (c.source_deck == 0);
    const float endX = (c.flags & 1u) ? c.crossfader_end : (srcIsA ? 1.0f : -1.0f);
    mixer.setCrossfader(endX);

    DeckPlayer* decks[2] = {deckA, deckB};
    for (int i = 0; i < 2; ++i) {
        DeckPlayer* d = decks[i];
        if (!d) {
            continue;
        }
        const DeckBaseline& b = baselines_[i];
        d->setVolume(b.volume);
        d->setEq(b.low, b.mid, b.high);
        d->setFilter(b.filter);
        d->setStemLevels(b.vocal, b.drum, b.bass, b.other);
    }

    // Ramped destination tempo returns to native (attenuation-safe, idempotent).
    DeckPlayer* dstP = (c.destination_deck == 0) ? deckA : deckB;
    if (dstP && c.dst_tempo_ramp_seconds > 0.0f) {
        dstP->setTempoRatio(1.0);
    }

    deactivate();
}

void TransitionExecutor::deactivate() noexcept {
    isActive_.store(false, std::memory_order_relaxed);
    elapsedFrames_ = 0;
}

void TransitionExecutor::consumePending(Mixer& mixer, DeckPlayer* deckA, DeckPlayer* deckB) noexcept {
    (void)mixer;

    constexpr int kMaxRetries = 32;
    for (int retry = 0;; ++retry) {
        const uint32_t seq = pendingSeq_.load(std::memory_order_acquire);
        if (seq == 0u) {
            return; // nothing pending
        }

        TransitionCommandC c = pendingCmd_; // 120-byte plain copy
        const uint32_t seq2 = pendingSeq_.load(std::memory_order_acquire);

        const bool stable = (seq == seq2) && ((seq & 1u) == 0u);
        const bool exhausted = (retry >= kMaxRetries);
        if (!stable && !exhausted) {
            continue; // writer in flight: bounded retry
        }

        pendingSeq_.store(0u, std::memory_order_release);

        // Re-validate structurally: a worst-case torn read (retry exhaustion) must
        // still never activate an invalid plan.
        if (c.source_deck > 1 || c.destination_deck > 1 || c.source_deck == c.destination_deck) {
            return;
        }

        activeCommand_ = c;
        captureBaselines(deckA, deckB);
        elapsedFrames_ = 0;

        switch (static_cast<TransitionStrategyType>(c.transition_type)) {
            case TransitionStrategyType::EqCrossfade:
                currentStrategy_ = &eqCrossfadeStrategy_;
                break;
            case TransitionStrategyType::BassSwap:
                currentStrategy_ = &bassSwapStrategy_;
                break;
            case TransitionStrategyType::ClassicEqBlend:
                currentStrategy_ = &classicEqBlendStrategy_;
                break;
            case TransitionStrategyType::PhraseCrossfade:
            default:
                currentStrategy_ = &phraseCrossfadeStrategy_;
                break;
        }
        currentStrategy_->reset();
        isActive_.store(true, std::memory_order_relaxed);
        return;
    }
}

void TransitionExecutor::processBlock(uint32_t numSamples, uint32_t sampleRate, Mixer& mixer, DeckPlayer* deckA, DeckPlayer* deckB) noexcept {
    if (numSamples == 0 || sampleRate == 0) {
        return;
    }

    consumePending(mixer, deckA, deckB);
    if (!isActive_.load(std::memory_order_relaxed)) {
        return;
    }

    elapsedFrames_ += numSamples;

    const double durationFrames = activeCommand_.duration_seconds * static_cast<double>(sampleRate);
    const double p = (durationFrames > 0.0) ? (static_cast<double>(elapsedFrames_) / durationFrames) : 1.0;
    progress_.store(std::clamp(p, 0.0, 1.0), std::memory_order_relaxed);

    applyStrategy(p, mixer, deckA, deckB);

    // Completion: p >= 1.0, plus the ClassicEqBlend post-cut tempo ramp if planned.
    const double rampSec = activeCommand_.dst_tempo_ramp_seconds;
    const double totalFrames = durationFrames + ((currentStrategy_ == &classicEqBlendStrategy_ && rampSec > 0.0) ? (rampSec * static_cast<double>(sampleRate)) : 0.0);
    if (static_cast<double>(elapsedFrames_) >= totalFrames) {
        finishTransition(mixer, deckA, deckB);
    }
}

void TransitionExecutor::updateAutomation(double elapsedTimeSeconds, Mixer& mixer, DeckPlayer* deckA, DeckPlayer* deckB) noexcept {
    consumePending(mixer, deckA, deckB);
    if (!isActive_.load(std::memory_order_relaxed) || activeCommand_.duration_seconds <= 0.0) {
        return;
    }

    const double elapsed = std::max(0.0, std::isfinite(elapsedTimeSeconds) ? elapsedTimeSeconds : 0.0);
    const double p = elapsed / activeCommand_.duration_seconds;
    progress_.store(std::clamp(p, 0.0, 1.0), std::memory_order_relaxed);

    applyStrategy(p, mixer, deckA, deckB);

    const double rampSec = activeCommand_.dst_tempo_ramp_seconds;
    const bool hasRamp = (currentStrategy_ == &classicEqBlendStrategy_ && rampSec > 0.0);
    if (p >= 1.0 && (!hasRamp || elapsed >= activeCommand_.duration_seconds + rampSec)) {
        finishTransition(mixer, deckA, deckB);
    }
}

void TransitionExecutor::applyStrategy(double p, Mixer& mixer, DeckPlayer* deckA, DeckPlayer* deckB) noexcept {
    if (currentStrategy_) {
        currentStrategy_->update(p, activeCommand_, mixer, deckA, deckB);
    }
}

bool TransitionExecutor::isTransitionActive() const noexcept {
    return isActive_.load();
}

} // namespace pulse::audio
