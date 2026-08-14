#pragma once

#include <cstdint>
#include <vector>
#include <memory>
#include <atomic>

/**
 * SoundTouch is licensed under LGPL-2.1.
 * To satisfy Section 6 relinking requirements, TimeStretchEngine dynamically links
 * libsoundtouch or isolates SoundTouch calls behind a clean C/C++ interface, ensuring
 * that users can replace the dynamic library without modifying PULSE's proprietary core.
 */

namespace pulse::audio {

/**
 * @brief Tempo matching mode for dual-deck transitions.
 */
enum class TempoMatchMode {
    None,          // 1.0x native playback
    MatchSource,   // Stretch destination track to match source track BPM
    MatchDest,     // Stretch source track to match destination track BPM
    MatchMaster    // Stretch both tracks to target master BPM
};

/**
 * @brief Configuration parameters for the TimeStretchEngine.
 */
struct TimeStretchConfig {
    uint32_t sampleRate{48000};
    uint32_t channels{2};
    double tempoRatio{1.0};      // new_tempo / old_tempo (e.g. 128.0 / 120.0 = 1.0667)
    float pitchSemiTones{0.0f};  // Strictly 0.0 for pure pitch-preserving time-stretch
    bool preservePitch{true};
    bool quickSeek{false};       // False for highest quality WSOLA/phase coherence
};

/**
 * @brief High-precision, real-time safe WSOLA time-stretching engine.
 *
 * Provides pitch-invariant tempo scaling (0.0 semitone pitch shift) with
 * bounded latency and zero heap allocations on the audio processing path.
 */
class TimeStretchEngine {
public:
    TimeStretchEngine();
    ~TimeStretchEngine();

    // Movable but non-copyable
    TimeStretchEngine(TimeStretchEngine&&) noexcept;
    TimeStretchEngine& operator=(TimeStretchEngine&&) noexcept;
    TimeStretchEngine(const TimeStretchEngine&) = delete;
    TimeStretchEngine& operator=(const TimeStretchEngine&) = delete;

    /**
     * @brief Initializes internal buffers and windowing lookup tables.
     * @param config Engine configuration parameters.
     * @return true on success, false on invalid parameters.
     */
    bool initialize(const TimeStretchConfig& config);

    /**
     * @brief Updates the target tempo ratio without reallocating buffers.
     * @param ratio Target tempo ratio (e.g. 1.05 = 5% faster).
     */
    void setTempoRatio(double ratio);

    /**
     * @brief Retrieves the active tempo ratio.
     */
    double getTempoRatio() const;

    /**
     * @brief Feeds input interleaved PCM samples into the stretch pipeline.
     * @param samples Interleaved float samples [-1.0, 1.0].
     * @param numFrames Number of multi-channel frames to feed.
     */
    void putSamples(const float* samples, uint32_t numFrames);

    /**
     * @brief Retrieves stretched interleaved PCM samples from the pipeline.
     * @param outputBuffer Destination buffer for interleaved float samples.
     * @param maxFrames Maximum number of multi-channel frames to read.
     * @return Actual number of frames written to outputBuffer.
     */
    uint32_t receiveSamples(float* outputBuffer, uint32_t maxFrames);

    /**
     * @brief Number of ready output frames available to read.
     */
    uint32_t numAvailableSamples() const;

    /**
     * @brief Flushes all remaining internal state into the output queue.
     */
    void flush();

    /**
     * @brief Clears all internal input/output queues.
     */
    void clear();

    /**
     * @brief Estimated internal group delay / latency in seconds.
     */
    double getLatencySeconds() const;

    /**
     * @brief Returns current configuration.
     */
    const TimeStretchConfig& getConfig() const { return config_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    TimeStretchConfig config_;
};

} // namespace pulse::audio
