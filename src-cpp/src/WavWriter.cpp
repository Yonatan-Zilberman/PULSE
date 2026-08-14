#include "../include/WavWriter.h"
#include <fstream>
#include <cmath>
#include <algorithm>
#include <vector>

namespace pulse::audio {

#pragma pack(push, 1)
struct WavHeader {
    char riff[4] = {'R', 'I', 'F', 'F'};
    uint32_t chunkSize = 0;
    char wave[4] = {'W', 'A', 'V', 'E'};
    char fmt[4] = {'f', 'm', 't', ' '};
    uint32_t subchunk1Size = 16;
    uint16_t audioFormat = 1; // PCM
    uint16_t numChannels = 2;
    uint32_t sampleRate = 48000;
    uint32_t byteRate = 48000 * 2 * 2;
    uint16_t blockAlign = 4;
    uint16_t bitsPerSample = 16;
    char data[4] = {'d', 'a', 't', 'a'};
    uint32_t subchunk2Size = 0;
};
#pragma pack(pop)

bool WavWriter::writeWav16(const std::string& filePath,
                           const float* samples,
                           uint64_t numFrames,
                           uint32_t sampleRate,
                           uint32_t channels) {
    if (!samples || numFrames == 0 || sampleRate == 0 || channels == 0) {
        return false;
    }

    std::ofstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    uint32_t dataBytes = static_cast<uint32_t>(numFrames * channels * sizeof(int16_t));
    WavHeader header;
    header.chunkSize = 36 + dataBytes;
    header.numChannels = static_cast<uint16_t>(channels);
    header.sampleRate = sampleRate;
    header.byteRate = sampleRate * channels * 2;
    header.blockAlign = static_cast<uint16_t>(channels * 2);
    header.bitsPerSample = 16;
    header.subchunk2Size = dataBytes;

    file.write(reinterpret_cast<const char*>(&header), sizeof(header));

    // Convert float [-1.0, 1.0] to signed 16-bit PCM [-32768, 32767]
    size_t totalSamples = numFrames * channels;
    std::vector<int16_t> pcmBuffer(totalSamples);

    for (size_t i = 0; i < totalSamples; ++i) {
        float sample = samples[i];
        float clamped = std::clamp(sample, -1.0f, 1.0f);
        pcmBuffer[i] = static_cast<int16_t>(std::round(clamped * 32767.0f));
    }

    file.write(reinterpret_cast<const char*>(pcmBuffer.data()), dataBytes);
    file.close();
    return true;
}

bool WavWriter::createSyntheticFixture(const std::string& filePath,
                                       double frequencyHz,
                                       double durationSec,
                                       double bpm,
                                       float amplitude,
                                       uint32_t sampleRate) {
    if (durationSec <= 0.0 || sampleRate == 0) {
        return false;
    }

    uint64_t totalFrames = static_cast<uint64_t>(durationSec * sampleRate);
    uint32_t channels = 2;
    std::vector<float> samples(totalFrames * channels);

    double beatIntervalFrames = (60.0 / bpm) * sampleRate;
    constexpr double twoPi = 6.28318530717958647692;

    for (uint64_t f = 0; f < totalFrames; ++f) {
        double timeSec = static_cast<double>(f) / sampleRate;

        // Base sine tone
        double baseSine = std::sin(twoPi * frequencyHz * timeSec);

        // Percussive click envelope at beat positions
        double beatPhase = std::fmod(static_cast<double>(f), beatIntervalFrames);
        double clickDecay = std::exp(-beatPhase / (sampleRate * 0.02)); // 20ms click decay
        double click = std::sin(twoPi * 1200.0 * timeSec) * clickDecay * 0.4;

        float value = static_cast<float>((baseSine * 0.6 + click) * amplitude);
        value = std::clamp(value, -1.0f, 1.0f);

        samples[f * channels + 0] = value; // Left
        samples[f * channels + 1] = value; // Right
    }

    return writeWav16(filePath, samples.data(), totalFrames, sampleRate, channels);
}

bool WavWriter::createTempoTestFixture(const std::string& filePath,
                                       double frequencyHz,
                                       double durationSec,
                                       double bpm,
                                       float amplitude,
                                       uint32_t sampleRate) {
    if (durationSec <= 0.0 || sampleRate == 0 || bpm <= 0.0) {
        return false;
    }

    uint64_t totalFrames = static_cast<uint64_t>(durationSec * sampleRate);
    uint32_t channels = 2;
    std::vector<float> samples(totalFrames * channels);

    double beatIntervalFrames = (60.0 / bpm) * sampleRate;
    constexpr double twoPi = 6.28318530717958647692;

    for (uint64_t f = 0; f < totalFrames; ++f) {
        double timeSec = static_cast<double>(f) / sampleRate;

        // Pure reference sinusoidal tone (e.g. 440 Hz for pitch checking)
        double baseSine = std::sin(twoPi * frequencyHz * timeSec);

        // Percussive transient impulse at exact beat intervals
        double beatPhase = std::fmod(static_cast<double>(f), beatIntervalFrames);
        double clickDecay = std::exp(-beatPhase / (sampleRate * 0.015)); // 15ms sharp transient
        double click = std::sin(twoPi * 1500.0 * timeSec) * clickDecay * 0.5;

        float value = static_cast<float>((baseSine * 0.5 + click) * amplitude);
        value = std::clamp(value, -1.0f, 1.0f);

        samples[f * channels + 0] = value;
        samples[f * channels + 1] = value;
    }

    return writeWav16(filePath, samples.data(), totalFrames, sampleRate, channels);
}

bool WavWriter::createAmbiguousFixture(const std::string& filePath,
                                       double frequencyHz,
                                       double durationSec,
                                       double bpmBase,
                                       float amplitude,
                                       uint32_t sampleRate) {
    if (durationSec <= 0.0 || sampleRate == 0) {
        return false;
    }

    uint64_t totalFrames = static_cast<uint64_t>(durationSec * sampleRate);
    uint32_t channels = 2;
    std::vector<float> samples(totalFrames * channels);

    double baseIntervalFrames = (60.0 / bpmBase) * sampleRate;
    double subIntervalFrames = baseIntervalFrames * 0.5; // 2x subdivision
    constexpr double twoPi = 6.28318530717958647692;

    for (uint64_t f = 0; f < totalFrames; ++f) {
        double timeSec = static_cast<double>(f) / sampleRate;
        double baseSine = std::sin(twoPi * frequencyHz * timeSec);

        // Strong beat click at base tempo (e.g. 70 BPM)
        double phaseBase = std::fmod(static_cast<double>(f), baseIntervalFrames);
        double clickDecayBase = std::exp(-phaseBase / (sampleRate * 0.025));
        double clickBase = std::sin(twoPi * 800.0 * timeSec) * clickDecayBase * 0.5;

        // Subdivided syncopated click at double tempo (e.g. 140 BPM)
        double phaseSub = std::fmod(static_cast<double>(f), subIntervalFrames);
        double clickDecaySub = std::exp(-phaseSub / (sampleRate * 0.015));
        double clickSub = std::sin(twoPi * 1800.0 * timeSec) * clickDecaySub * 0.35;

        float value = static_cast<float>((baseSine * 0.4 + clickBase + clickSub) * amplitude);
        value = std::clamp(value, -1.0f, 1.0f);

        samples[f * channels + 0] = value;
        samples[f * channels + 1] = value;
    }

    return writeWav16(filePath, samples.data(), totalFrames, sampleRate, channels);
}

bool WavWriter::createDriftingFixture(const std::string& filePath,
                                      double frequencyHz,
                                      double durationSec,
                                      double bpmStart,
                                      double bpmEnd,
                                      float amplitude,
                                      uint32_t sampleRate) {
    if (durationSec <= 0.0 || sampleRate == 0) {
        return false;
    }

    uint64_t totalFrames = static_cast<uint64_t>(durationSec * sampleRate);
    uint32_t channels = 2;
    std::vector<float> samples(totalFrames * channels);
    constexpr double twoPi = 6.28318530717958647692;

    for (uint64_t f = 0; f < totalFrames; ++f) {
        double timeSec = static_cast<double>(f) / sampleRate;
        double currentBpm = bpmStart + (timeSec / durationSec) * (bpmEnd - bpmStart);
        double instBeatFreq = currentBpm / 60.0;

        // Integral of instBeatFreq: (bpmStart/60)*t + (bpmEnd - bpmStart)/(120*dur) * t^2
        double beatPhase = (bpmStart / 60.0) * timeSec + ((bpmEnd - bpmStart) / (120.0 * durationSec)) * timeSec * timeSec;
        double beatFrac = beatPhase - std::floor(beatPhase);
        double phaseSeconds = beatFrac / instBeatFreq;

        double baseSine = std::sin(twoPi * frequencyHz * timeSec);
        double clickDecay = std::exp(-phaseSeconds / 0.02);
        double click = std::sin(twoPi * 1200.0 * timeSec) * clickDecay * 0.45;

        float value = static_cast<float>((baseSine * 0.55 + click) * amplitude);
        value = std::clamp(value, -1.0f, 1.0f);

        samples[f * channels + 0] = value;
        samples[f * channels + 1] = value;
    }

    return writeWav16(filePath, samples.data(), totalFrames, sampleRate, channels);
}

bool WavWriter::createSyncopatedFixture(const std::string& filePath,
                                        double frequencyHz,
                                        double durationSec,
                                        double silenceIntroSec,
                                        double bpm,
                                        float amplitude,
                                        uint32_t sampleRate) {
    if (durationSec <= 0.0 || sampleRate == 0) {
        return false;
    }

    uint64_t totalFrames = static_cast<uint64_t>(durationSec * sampleRate);
    uint32_t channels = 2;
    std::vector<float> samples(totalFrames * channels, 0.0f);

    double beatIntervalFrames = (60.0 / bpm) * sampleRate;
    uint64_t silenceFrames = static_cast<uint64_t>(silenceIntroSec * sampleRate);
    constexpr double twoPi = 6.28318530717958647692;

    for (uint64_t f = silenceFrames; f < totalFrames; ++f) {
        double timeSec = static_cast<double>(f) / sampleRate;
        double activeTimeSec = static_cast<double>(f - silenceFrames) / sampleRate;

        double baseSine = std::sin(twoPi * frequencyHz * timeSec);

        // Beat clicks starting after silence
        double beatPhase = std::fmod(static_cast<double>(f - silenceFrames), beatIntervalFrames);
        double clickDecay = std::exp(-beatPhase / (sampleRate * 0.02));
        double click = std::sin(twoPi * 1400.0 * activeTimeSec) * clickDecay * 0.5;

        float value = static_cast<float>((baseSine * 0.5 + click) * amplitude);
        value = std::clamp(value, -1.0f, 1.0f);

        samples[f * channels + 0] = value;
        samples[f * channels + 1] = value;
    }

    return writeWav16(filePath, samples.data(), totalFrames, sampleRate, channels);
}

} // namespace pulse::audio
