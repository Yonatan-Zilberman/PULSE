#pragma once

#include "AudioBridgeTypes.h"
#include "AudioDecoder.h"
#include "TimeStretchEngine.h"
#include <string>
#include <atomic>
#include <memory>
#include <vector>

namespace pulse::audio {

/**
 * @brief Real-time Deck Audio Player with Pitch-Preserving Time-Stretching.
 *
 * REAL-TIME SAFETY CONTRACT:
 * - The processBlock() method runs directly on the high-priority OS audio thread.
 * - Forbids dynamic heap memory allocations (new/malloc).
 * - Forbids blocking synchronization primitives (mutex/condition_variable).
 * - Forbids file I/O and network operations.
 */
class DeckPlayer {
public:
    explicit DeckPlayer(uint8_t deckId);
    ~DeckPlayer();

    DeckPlayer(const DeckPlayer&) = delete;
    DeckPlayer& operator=(const DeckPlayer&) = delete;
    DeckPlayer(DeckPlayer&&) = delete;
    DeckPlayer& operator=(DeckPlayer&&) = delete;

    bool loadFile(const std::string& filePath);
    void setPlaying(bool playing);
    bool isPlaying() const;

    void setPlaybackPosition(double seconds);
    double getPlaybackPosition() const;
    double getDuration() const;

    void setVolume(float vol);
    float getVolume() const;

    void setEq(float low, float mid, float high);
    void setFilter(float filterVal);

    void setStemLevels(float vocal, float drum, float bass, float other);

    // Tempo and Pitch-Preservation Controls
    void setTempoRatio(double ratio);
    double getTempoRatio() const;
    void setPitchPreservation(bool enabled);
    bool isPitchPreserved() const;

    // Audio callback processing block (Real-Time thread)
    void processBlock(float* outputBuffer, uint32_t numSamples, uint32_t numChannels) noexcept;

    DeckStateC getState() const noexcept;
    const DecodedAudio& getDecodedAudio() const noexcept { return loadedAudio_; }

private:
    uint8_t deckId_;
    std::atomic<bool> isPlaying_{false};
    std::atomic<double> playbackPosition_{0.0};
    std::atomic<float> volume_{1.0f};
    std::atomic<float> lowEq_{0.0f};
    std::atomic<float> midEq_{0.0f};
    std::atomic<float> highEq_{0.0f};
    std::atomic<float> filter_{0.0f};
    std::atomic<float> vocalStem_{1.0f};
    std::atomic<float> drumStem_{1.0f};
    std::atomic<float> bassStem_{1.0f};
    std::atomic<float> otherStem_{1.0f};

    // Tempo Scaling State
    std::atomic<double> tempoRatio_{1.0};
    std::atomic<bool> preservePitch_{true};

    DecodedAudio loadedAudio_{};
    std::unique_ptr<TimeStretchEngine> stretchEngine_;

    // Pre-allocated Real-Time scratch buffers
    std::vector<float> inputBlockScratch_;
    std::vector<float> outputBlockScratch_;
};

} // namespace pulse::audio
