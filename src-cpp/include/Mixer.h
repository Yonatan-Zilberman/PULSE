#pragma once

#include <cstdint>
#include <atomic>

namespace pulse::audio {

/**
 * @brief Master Mixer and Crossfader Engine.
 *
 * REAL-TIME SAFETY CONTRACT:
 * - Deterministic gain curves and LUFS limiting without memory allocation.
 */
class Mixer {
public:
    Mixer();
    ~Mixer() = default;

    void setCrossfader(float position); // -1.0 (Deck A) to 1.0 (Deck B)
    float getCrossfader() const noexcept;

    void setMasterVolume(float vol);
    float getMasterVolume() const noexcept;

    // Mix stereo buffers from Deck A and Deck B into master output
    void mix(const float* deckABuffer, const float* deckBBuffer, float* masterBuffer, uint32_t numSamples, uint32_t numChannels) noexcept;

private:
    std::atomic<float> crossfaderPosition_{-1.0f};
    std::atomic<float> masterVolume_{1.0f};
};

} // namespace pulse::audio
