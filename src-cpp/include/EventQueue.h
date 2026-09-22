#pragma once

#include "AudioBridgeTypes.h"

#include <atomic>
#include <cstdint>

namespace pulse::audio {

/**
 * @brief Fixed-capacity lock-free MPMC event queue for engine -> application delivery.
 *
 * REAL-TIME SAFETY CONTRACT:
 * - Zero dynamic heap allocation (fixed `kCapacity` slot array, constructed once).
 * - Zero blocking synchronization (per-slot seqlock + atomic write cursor only).
 * - `push` is real-time safe: a single CAS with a BOUNDED retry (<= 8); when the
 *   ring is full or the cursor contended past the retry budget, the event is
 *   DROPPED and counted in `dropped()` — it never spins indefinitely.
 *
 * Seqlock protocol (per slot, seq = 2 * logicalPosition + stateBit):
 * - Writer: CAS the write cursor to claim `pos`, then seq = 2pos+1 -> payload -> seq = 2pos+2.
 * - Reader (single consumer, drains in order): accepts a slot only when seq == 2pos+2,
 *   then stores seq = 2pos+1 to mark the slot consumed (reclaimable by the next writer).
 * - A slot whose seq is even (and non-zero) is published-but-unconsumed; odd or zero
 *   means the slot is free to be (re)written.
 *
 * Producers are multiple (control plane + real-time thread); the consumer is
 * single (the application drain). The write cursor is monotonically increasing
 * and wraps at 2^32 (far beyond any realistic process lifetime of events).
 */
class EventQueue {
public:
    static constexpr uint32_t kCapacity = 512;      // Must be a power of two.
    static constexpr uint32_t kMaxPushRetries = 8;  // Bounded real-time retry budget.

    struct Slot {
        std::atomic<uint64_t> seq{0};
        AudioEventC payload{};
    };

    EventQueue() = default;

    EventQueue(const EventQueue&) = delete;
    EventQueue& operator=(const EventQueue&) = delete;

    /**
     * @brief Attempts to enqueue one event (real-time safe).
     * @return true if accepted; false if dropped (queue full or retry budget exhausted).
     */
    bool push(const AudioEventC& event) noexcept {
        for (uint32_t attempt = 0; attempt < kMaxPushRetries; ++attempt) {
            const uint32_t pos = writeCursor_.load(std::memory_order_acquire);
            const uint32_t idx = pos & (kCapacity - 1);
            Slot& slot = slots_[idx];

            const uint64_t seq = slot.seq.load(std::memory_order_acquire);
            const bool free = (seq == 0) || ((seq & 1ULL) != 0);
            if (!free) {
                // Consumer has not reclaimed this slot: ring full -> drop (bounded).
                dropped_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }

            uint32_t expected = pos;
            if (writeCursor_.compare_exchange_weak(expected, pos + 1,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
                slot.seq.store(2 * pos + 1, std::memory_order_seq_cst);
                slot.payload = event;
                slot.seq.store(2 * pos + 2, std::memory_order_release);
                return true;
            }
            // CAS lost to a concurrent producer: retry with the fresh cursor (bounded).
        }
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    /**
     * @brief Drains up to `max` queued events in order (single consumer only).
     * @param out Caller-allocated buffer (must be non-null when max > 0).
     * @param max Buffer capacity.
     * @return Number of events copied (0..max). null/0 is a no-op returning 0.
     */
    uint32_t drain(AudioEventC* out, uint32_t max) noexcept {
        if (out == nullptr || max == 0) {
            return 0;
        }

        uint32_t drained = 0;
        const uint32_t bound = writeCursor_.load(std::memory_order_acquire);
        while (drained < max && readCursor_ < bound) {
            const uint32_t pos = readCursor_;
            Slot& slot = slots_[pos & (kCapacity - 1)];

            const uint64_t seq = slot.seq.load(std::memory_order_acquire);
            if (seq != 2 * pos + 2) {
                break;  // Slot not (yet) published — leave it for the next drain.
            }

            out[drained++] = slot.payload;
            readCursor_ = pos + 1;
            slot.seq.store(2 * pos + 1, std::memory_order_seq_cst);  // mark consumed.
        }
        return drained;
    }

    /** Cumulative count of events dropped due to overflow/retry exhaustion. */
    uint32_t dropped() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }

private:
    Slot slots_[kCapacity];
    std::atomic<uint32_t> writeCursor_{0};
    std::atomic<uint32_t> dropped_{0};
    uint32_t readCursor_{0};  // Touched only by the single drain consumer thread.
};

} // namespace pulse::audio
