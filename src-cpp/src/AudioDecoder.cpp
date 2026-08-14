#include "../include/AudioDecoder.h"
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <cmath>
#include <fstream>
#include <algorithm>

namespace pulse::audio {

bool AudioDecoder::decodeFile(const std::string& filePath,
                             DecodedAudio& outAudio,
                             uint32_t targetSampleRate,
                             uint32_t targetChannels) {
    if (filePath.empty() || targetSampleRate == 0 || targetChannels == 0) {
        return false;
    }

    // 1. Verify file exists and has non-zero size
    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return false;
    }
    std::streamsize fileSize = file.tellg();
    file.close();
    if (fileSize <= 44) { // Minimum possible audio header size
        return false;
    }

    // 2. Open audio file with Apple ExtAudioFile API
    CFURLRef fileUrl = CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(filePath.c_str()),
        static_cast<CFIndex>(filePath.length()),
        false
    );

    if (!fileUrl) {
        return false;
    }

    ExtAudioFileRef audioFileRef = nullptr;
    OSStatus status = ExtAudioFileOpenURL(fileUrl, &audioFileRef);
    CFRelease(fileUrl);

    if (status != noErr || !audioFileRef) {
        return false;
    }

    // 3. Query native file format
    AudioStreamBasicDescription fileFormat{};
    UInt32 propSize = sizeof(fileFormat);
    status = ExtAudioFileGetProperty(audioFileRef, kExtAudioFileProperty_FileDataFormat, &propSize, &fileFormat);
    if (status != noErr) {
        ExtAudioFileDispose(audioFileRef);
        return false;
    }

    // 4. Set client format for automatic resampling/conversion to 32-bit float stereo interleaved PCM
    AudioStreamBasicDescription clientFormat{};
    clientFormat.mSampleRate = static_cast<Float64>(targetSampleRate);
    clientFormat.mFormatID = kAudioFormatLinearPCM;
    clientFormat.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    clientFormat.mBitsPerChannel = 32;
    clientFormat.mChannelsPerFrame = targetChannels;
    clientFormat.mBytesPerFrame = targetChannels * sizeof(float);
    clientFormat.mFramesPerPacket = 1;
    clientFormat.mBytesPerPacket = clientFormat.mBytesPerFrame;

    status = ExtAudioFileSetProperty(audioFileRef, kExtAudioFileProperty_ClientDataFormat, sizeof(clientFormat), &clientFormat);
    if (status != noErr) {
        ExtAudioFileDispose(audioFileRef);
        return false;
    }

    // 5. Read decoded audio frames in chunks
    constexpr UInt32 kChunkFrames = 4096;
    std::vector<float> chunkBuffer(kChunkFrames * targetChannels);
    outAudio.samples.clear();
    outAudio.samples.reserve(targetSampleRate * targetChannels * 10); // 10-sec initial reserve

    AudioBufferList bufferList;
    bufferList.mNumberBuffers = 1;
    bufferList.mBuffers[0].mNumberChannels = targetChannels;

    while (true) {
        UInt32 framesToRead = kChunkFrames;
        bufferList.mBuffers[0].mDataByteSize = framesToRead * targetChannels * sizeof(float);
        bufferList.mBuffers[0].mData = chunkBuffer.data();

        status = ExtAudioFileRead(audioFileRef, &framesToRead, &bufferList);
        if (status != noErr) {
            ExtAudioFileDispose(audioFileRef);
            return false;
        }

        if (framesToRead == 0) {
            break; // End of file reached
        }

        size_t samplesRead = framesToRead * targetChannels;
        outAudio.samples.insert(outAudio.samples.end(), chunkBuffer.begin(), chunkBuffer.begin() + samplesRead);
    }

    ExtAudioFileDispose(audioFileRef);

    if (outAudio.samples.empty()) {
        return false;
    }

    // 6. Calculate Telemetry
    outAudio.sampleRate = targetSampleRate;
    outAudio.channels = targetChannels;
    outAudio.totalFrames = outAudio.samples.size() / targetChannels;
    outAudio.durationSeconds = static_cast<double>(outAudio.totalFrames) / static_cast<double>(targetSampleRate);

    float maxPeak = 0.0f;
    for (float sample : outAudio.samples) {
        float absVal = std::abs(sample);
        if (absVal > maxPeak) {
            maxPeak = absVal;
        }
    }
    outAudio.peakAmplitude = maxPeak;
    outAudio.clippingDetected = (maxPeak >= (1.0f - 1e-4f));
    outAudio.maxPeakDb = (maxPeak > 1e-5f) ? 20.0f * std::log10(maxPeak) : -100.0f;

    // 7. Estimate BPM
    outAudio.detectedBpm = estimateBpm(outAudio.samples.data(), outAudio.totalFrames, targetSampleRate, targetChannels);

    return true;
}

double AudioDecoder::estimateBpm(const float* samples, uint64_t totalFrames, uint32_t sampleRate, uint32_t channels) {
    if (!samples || totalFrames == 0 || sampleRate == 0 || channels == 0) {
        return 120.0;
    }

    // Hop size for envelope extraction (~10ms)
    uint32_t hopSize = sampleRate / 100;
    if (hopSize == 0) hopSize = 512;
    size_t numHops = totalFrames / hopSize;
    if (numHops < 50) {
        return 120.0;
    }

    // 1. Extract RMS energy envelope
    std::vector<float> envelope(numHops, 0.0f);
    for (size_t h = 0; h < numHops; ++h) {
        double sumSquares = 0.0;
        size_t startFrame = h * hopSize;
        size_t endFrame = std::min(startFrame + hopSize, static_cast<size_t>(totalFrames));
        for (size_t f = startFrame; f < endFrame; ++f) {
            for (uint32_t c = 0; c < channels; ++c) {
                float s = samples[f * channels + c];
                sumSquares += s * s;
            }
        }
        envelope[h] = static_cast<float>(std::sqrt(sumSquares / ((endFrame - startFrame) * channels)));
    }

    // 2. Compute onset novelty curve (half-wave rectified difference)
    std::vector<float> onset(numHops, 0.0f);
    for (size_t h = 1; h < numHops; ++h) {
        float diff = envelope[h] - envelope[h - 1];
        onset[h] = diff > 0.0f ? diff : 0.0f;
    }

    // 3. Autocorrelation in the BPM range [60, 180]
    double hopsPerSecond = static_cast<double>(sampleRate) / hopSize;
    size_t minLag = static_cast<size_t>((60.0 / 180.0) * hopsPerSecond);
    size_t maxLag = static_cast<size_t>((60.0 / 60.0) * hopsPerSecond);
    if (maxLag >= numHops) maxLag = numHops - 1;

    double bestCorr = -1.0;
    size_t bestLag = minLag;

    for (size_t lag = minLag; lag <= maxLag; ++lag) {
        double corr = 0.0;
        size_t count = numHops - lag;
        for (size_t i = 0; i < count; ++i) {
            corr += onset[i] * onset[i + lag];
        }
        corr /= count;

        if (corr > bestCorr) {
            bestCorr = corr;
            bestLag = lag;
        }
    }

    if (bestCorr <= 1e-6 || bestLag == 0) {
        return 120.0;
    }

    double bpm = (60.0 * hopsPerSecond) / static_cast<double>(bestLag);
    // Round to 1 decimal place
    return std::round(bpm * 10.0) / 10.0;
}

} // namespace pulse::audio
