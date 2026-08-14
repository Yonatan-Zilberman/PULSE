#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace pulse::audio {

/**
 * @brief Decoded audio buffer and associated stream telemetry.
 */
struct DecodedAudio {
    std::vector<float> samples;      // Interleaved 32-bit float PCM [-1.0, 1.0]
    uint32_t sampleRate{0};          // e.g. 48000 Hz
    uint32_t channels{0};            // e.g. 2 (stereo)
    uint64_t totalFrames{0};         // Total frames (samples / channels)
    double durationSeconds{0.0};     // totalFrames / sampleRate
    float peakAmplitude{0.0f};       // Maximum absolute sample value
    float maxPeakDb{-100.0f};        // 20 * log10(peakAmplitude)
    bool clippingDetected{false};    // true if peakAmplitude >= 1.0f - 1e-4f
    double detectedBpm{120.0};       // Estimated tempo from onset/energy intervals
};

/**
 * @brief High-performance offline audio decoder using Apple ExtAudioFile / AudioToolbox.
 */
class AudioDecoder {
public:
    /**
     * @brief Decodes any supported audio format into 32-bit float interleaved PCM at target sample rate and channels.
     * @param filePath Path to input audio file.
     * @param outAudio Destination DecodedAudio struct.
     * @param targetSampleRate Target output sample rate (default 48000 Hz).
     * @param targetChannels Target output channel count (default 2 for stereo).
     * @return true on success, false on failure (e.g. missing file, 0-byte, corrupt header).
     */
    static bool decodeFile(const std::string& filePath,
                           DecodedAudio& outAudio,
                           uint32_t targetSampleRate = 48000,
                           uint32_t targetChannels = 2);

    /**
     * @brief Estimates tempo (BPM) from audio waveform using energy onset envelope autocorrelation.
     */
    static double estimateBpm(const float* samples, uint64_t totalFrames, uint32_t sampleRate, uint32_t channels);
};

} // namespace pulse::audio
