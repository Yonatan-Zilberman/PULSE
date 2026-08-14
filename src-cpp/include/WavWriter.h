#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace pulse::audio {

/**
 * @brief Zero-dependency, standard-compliant uncompressed PCM WAV file writer.
 */
class WavWriter {
public:
    /**
     * @brief Writes interleaved 32-bit float samples to a 16-bit PCM WAV file with dithering/clamping.
     * @param filePath Destination file path.
     * @param samples Interleaved float samples normalized to [-1.0, 1.0].
     * @param numFrames Total frame count.
     * @param sampleRate Audio sample rate (e.g. 48000 Hz).
     * @param channels Channel count (e.g. 2 for stereo).
     * @return true on success, false on I/O error.
     */
    static bool writeWav16(const std::string& filePath,
                           const float* samples,
                           uint64_t numFrames,
                           uint32_t sampleRate = 48000,
                           uint32_t channels = 2);

    /**
     * @brief Generates a synthetic deterministic stereo WAV fixture.
     * @param filePath Destination path.
     * @param frequencyHz Base tone frequency (e.g. 440.0 Hz).
     * @param durationSec Duration in seconds (e.g. 10.0s).
     * @param bpm Pulse/click BPM interval (e.g. 120.0 BPM).
     * @param amplitude Peak amplitude (e.g. 0.80).
     * @param sampleRate Sample rate (e.g. 48000 Hz).
     */
    static bool createSyntheticFixture(const std::string& filePath,
                                       double frequencyHz,
                                       double durationSec,
                                       double bpm = 120.0,
                                       float amplitude = 0.80f,
                                       uint32_t sampleRate = 48000);
};

} // namespace pulse::audio
