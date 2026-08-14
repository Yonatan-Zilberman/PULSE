#include "../include/AudioDecoder.h"
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <cmath>
#include <fstream>
#include <algorithm>
#include <set>

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

    // 7. Estimate BPM and perform rich tempo / beat tracking analysis
    outAudio.tempoProfile = analyzeTempo(outAudio.samples.data(), outAudio.totalFrames, targetSampleRate, targetChannels);
    outAudio.detectedBpm = outAudio.tempoProfile.primaryBpm;
    outAudio.phraseProfile = outAudio.tempoProfile.phraseProfile;

    return true;
}

double AudioDecoder::estimateBpm(const float* samples, uint64_t totalFrames, uint32_t sampleRate, uint32_t channels) {
    TempoAnalysisResult result = analyzeTempo(samples, totalFrames, sampleRate, channels);
    return result.primaryBpm;
}

TempoAnalysisResult AudioDecoder::analyzeTempo(const float* samples, uint64_t totalFrames, uint32_t sampleRate, uint32_t channels) {
    TempoAnalysisResult result;
    result.primaryBpm = 120.0;
    result.confidence = 0.0f;
    result.alternativeHypotheses = {60.0, 240.0};
    result.gridOffsetSeconds = 0.0;
    result.isVariableTempo = false;
    result.tempoDriftMinBpm = 120.0;
    result.tempoDriftMaxBpm = 120.0;

    if (!samples || totalFrames == 0 || sampleRate == 0 || channels == 0) {
        return result;
    }

    double totalDurationSec = static_cast<double>(totalFrames) / static_cast<double>(sampleRate);

    // Hop size for envelope extraction (~10ms)
    uint32_t hopSize = sampleRate / 100;
    if (hopSize == 0) hopSize = 480;
    size_t numHops = totalFrames / hopSize;
    if (numHops < 50) {
        return result;
    }

    double hopsPerSecond = static_cast<double>(sampleRate) / hopSize;

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

    // Adaptive local moving-average threshold subtraction to isolate sharp transient peaks
    std::vector<float> onsetNorm(numHops, 0.0f);
    size_t localWindowHops = static_cast<size_t>(hopsPerSecond * 0.2); // ~200ms
    if (localWindowHops < 3) localWindowHops = 3;

    float maxNovelty = 0.0f;
    for (size_t h = 0; h < numHops; ++h) {
        size_t wStart = (h > localWindowHops) ? (h - localWindowHops) : 0;
        size_t wEnd = std::min(h + localWindowHops, numHops);
        float localSum = 0.0f;
        for (size_t i = wStart; i < wEnd; ++i) {
            localSum += onset[i];
        }
        float localMean = localSum / (wEnd - wStart);
        float val = onset[h] - localMean * 0.5f;
        onsetNorm[h] = (val > 0.0f) ? val : 0.0f;
        if (onsetNorm[h] > maxNovelty) {
            maxNovelty = onsetNorm[h];
        }
    }

    if (maxNovelty <= 1e-6f) {
        return result;
    }

    // Helper lambda for autocorrelation over a given onset slice and lag range
    auto computeAutocorrelation = [&](const float* data, size_t len, size_t minL, size_t maxL,
                                      size_t& outBestLag, double& outBestCorr, double& outMeanCorr,
                                      std::vector<std::pair<size_t, double>>& outPeaks) {
        outBestCorr = -1.0;
        outBestLag = minL;
        outMeanCorr = 0.0;
        size_t lagCount = 0;
        std::vector<double> corrArray(maxL + 2, 0.0);

        for (size_t lag = minL; lag <= maxL; ++lag) {
            double corr = 0.0;
            size_t count = len - lag;
            for (size_t i = 0; i < count; ++i) {
                corr += data[i] * data[i + lag];
            }
            corr /= count;
            corrArray[lag] = corr;
            outMeanCorr += corr;
            lagCount++;

            if (corr > outBestCorr) {
                outBestCorr = corr;
                outBestLag = lag;
            }
        }

        if (lagCount > 0) {
            outMeanCorr /= lagCount;
        }

        // Peak picking: find local maxima in correlation
        for (size_t lag = minL + 1; lag < maxL; ++lag) {
            if (corrArray[lag] > corrArray[lag - 1] && corrArray[lag] > corrArray[lag + 1] && corrArray[lag] > outMeanCorr) {
                outPeaks.push_back({lag, corrArray[lag]});
            }
        }
        std::sort(outPeaks.begin(), outPeaks.end(), [](const auto& a, const auto& b) {
            return a.second > b.second;
        });

        // Preference for fundamental beat lag over subharmonic multiple (e.g. 120 BPM over 60 BPM)
        if (outBestLag >= minL * 2) {
            size_t halfLag = outBestLag / 2;
            size_t searchStart = (halfLag > 2) ? halfLag - 2 : minL;
            size_t searchEnd = std::min(halfLag + 2, maxL);
            size_t bestHalfLag = halfLag;
            double bestHalfCorr = 0.0;
            for (size_t l = searchStart; l <= searchEnd; ++l) {
                if (l < corrArray.size() && corrArray[l] > bestHalfCorr) {
                    bestHalfCorr = corrArray[l];
                    bestHalfLag = l;
                }
            }
            if (bestHalfCorr >= 0.80 * outBestCorr && bestHalfLag >= minL) {
                outBestLag = bestHalfLag;
                outBestCorr = bestHalfCorr;
            }
        }
    };

    // 3. Global Autocorrelation in the BPM range [50, 220]
    size_t minLag = static_cast<size_t>((60.0 / 220.0) * hopsPerSecond);
    size_t maxLag = static_cast<size_t>((60.0 / 50.0) * hopsPerSecond);
    if (maxLag >= numHops) maxLag = numHops - 1;
    if (minLag < 2) minLag = 2;

    size_t bestLag = minLag;
    double bestCorr = 0.0;
    double meanCorr = 0.0;
    std::vector<std::pair<size_t, double>> globalPeaks;

    computeAutocorrelation(onsetNorm.data(), numHops, minLag, maxLag, bestLag, bestCorr, meanCorr, globalPeaks);

    if (bestCorr <= 1e-6 || bestLag == 0) {
        return result;
    }

    double primaryBpmRaw = (60.0 * hopsPerSecond) / static_cast<double>(bestLag);
    double primaryBpm = std::round(primaryBpmRaw * 10.0) / 10.0;

    // Confidence calculation
    float confidence = static_cast<float>(std::clamp((bestCorr - meanCorr) / (bestCorr + 1e-6), 0.0, 1.0));

    // Alternative Hypotheses Resolution (e.g. Half-Time / Double-Time / Secondary Peaks)
    std::set<double> candidateBpms;
    candidateBpms.insert(std::round((primaryBpm / 2.0) * 10.0) / 10.0);
    candidateBpms.insert(std::round((primaryBpm * 2.0) * 10.0) / 10.0);

    for (const auto& pk : globalPeaks) {
        double pkBpm = std::round(((60.0 * hopsPerSecond) / static_cast<double>(pk.first)) * 10.0) / 10.0;
        if (std::abs(pkBpm - primaryBpm) > 2.0 && pk.second > 0.5 * bestCorr) {
            candidateBpms.insert(pkBpm);
        }
    }

    std::vector<double> altHypotheses(candidateBpms.begin(), candidateBpms.end());

    // 4. Sliding-Window Drift / Variable Tempo Analysis
    bool isVariableTempo = false;
    double driftMinBpm = primaryBpm;
    double driftMaxBpm = primaryBpm;
    std::vector<double> localBpms;

    if (totalDurationSec >= 6.0) {
        double windowDurationSec = 4.0;
        double hopDurationSec = 1.0;
        size_t windowHops = static_cast<size_t>(windowDurationSec * hopsPerSecond);
        size_t stepHops = static_cast<size_t>(hopDurationSec * hopsPerSecond);

        for (size_t startH = 0; startH + windowHops <= numHops; startH += stepHops) {
            size_t locBestLag = minLag;
            double locBestCorr = 0.0;
            double locMeanCorr = 0.0;
            std::vector<std::pair<size_t, double>> locPeaks;

            computeAutocorrelation(onsetNorm.data() + startH, windowHops, minLag, maxLag,
                                   locBestLag, locBestCorr, locMeanCorr, locPeaks);

            if (locBestCorr > 1e-6 && locBestLag > 0) {
                double locBpm = (60.0 * hopsPerSecond) / static_cast<double>(locBestLag);
                // Filter out octave jumps from primary tempo
                if (std::abs(locBpm * 2.0 - primaryBpm) < std::abs(locBpm - primaryBpm)) {
                    locBpm *= 2.0;
                } else if (std::abs(locBpm * 0.5 - primaryBpm) < std::abs(locBpm - primaryBpm)) {
                    locBpm *= 0.5;
                }
                localBpms.push_back(locBpm);
            }
        }

        if (localBpms.size() >= 3) {
            double minLoc = *std::min_element(localBpms.begin(), localBpms.end());
            double maxLoc = *std::max_element(localBpms.begin(), localBpms.end());
            if ((maxLoc - minLoc) > 2.0) {
                isVariableTempo = true;
                driftMinBpm = std::round(minLoc * 10.0) / 10.0;
                driftMaxBpm = std::round(maxLoc * 10.0) / 10.0;
            }
        }
    }

    // 5. Phase Offset Optimization & First Beat Onset Detection
    double beatIntervalSec = 60.0 / primaryBpm;
    double beatIntervalHops = beatIntervalSec * hopsPerSecond;
    size_t searchPhaseMaxHops = static_cast<size_t>(std::ceil(beatIntervalHops));
    if (searchPhaseMaxHops >= numHops) searchPhaseMaxHops = numHops - 1;

    size_t bestPhaseHop = 0;
    double bestPhaseScore = -1.0;

    for (size_t phase = 0; phase < searchPhaseMaxHops; ++phase) {
        double score = 0.0;
        size_t count = 0;
        for (double h = static_cast<double>(phase); h < static_cast<double>(numHops); h += beatIntervalHops) {
            size_t hopIdx = static_cast<size_t>(std::round(h));
            if (hopIdx < numHops) {
                score += onsetNorm[hopIdx];
                count++;
            }
        }
        if (count > 0) {
            score /= count;
            if (score > bestPhaseScore) {
                bestPhaseScore = score;
                bestPhaseHop = phase;
            }
        }
    }

    // Detect actual first onset after any leading silence
    double firstOnsetSec = 0.0;
    if (envelope[0] <= 0.01f) {
        double thresholdNovelty = maxNovelty * 0.15f;
        for (double h = static_cast<double>(bestPhaseHop); h < static_cast<double>(numHops); h += beatIntervalHops) {
            size_t hopIdx = static_cast<size_t>(std::round(h));
            if (hopIdx < numHops && onsetNorm[hopIdx] >= thresholdNovelty) {
                firstOnsetSec = static_cast<double>(hopIdx) / hopsPerSecond;
                break;
            }
        }
    }

    // 6. Downbeat & Bar Alignment (4/4 Meter Heuristic)
    size_t bestDownbeatOffset = 0;
    double bestDownbeatScore = -1.0;

    for (size_t barBeat = 0; barBeat < 4; ++barBeat) {
        double score = 0.0;
        size_t count = 0;
        double startHop = firstOnsetSec * hopsPerSecond + static_cast<double>(barBeat) * beatIntervalHops;
        for (double h = startHop; h < static_cast<double>(numHops); h += (4.0 * beatIntervalHops)) {
            size_t hopIdx = static_cast<size_t>(std::round(h));
            if (hopIdx < numHops) {
                float localMax = onsetNorm[hopIdx];
                if (hopIdx > 0 && onsetNorm[hopIdx - 1] > localMax) {
                    localMax = onsetNorm[hopIdx - 1];
                }
                if (hopIdx + 1 < numHops && onsetNorm[hopIdx + 1] > localMax) {
                    localMax = onsetNorm[hopIdx + 1];
                }
                score += localMax;
                count++;
            }
        }
        if (count > 0) {
            score /= count;
            if (score > bestDownbeatScore * 1.15) {
                bestDownbeatScore = score;
                bestDownbeatOffset = barBeat;
            }
        }
    }

    // 7. Beat & Downbeat Timestamp Generation
    std::vector<double> beats;
    std::vector<double> downbeats;
    std::vector<double> bars;

    if (isVariableTempo && !localBpms.empty()) {
        // Dynamic beat placement across local tempo contour
        double currentPos = firstOnsetSec;
        size_t beatCount = 0;
        while (currentPos < totalDurationSec) {
            double roundedPos = std::round(currentPos * 1000.0) / 1000.0;
            beats.push_back(roundedPos);

            if ((beatCount % 4) == bestDownbeatOffset) {
                downbeats.push_back(roundedPos);
                bars.push_back(roundedPos);
            }

            // Estimate current local tempo from progression
            double progress = currentPos / totalDurationSec;
            size_t idx = static_cast<size_t>(progress * (localBpms.size() - 1));
            idx = std::min(idx, localBpms.size() - 1);
            double currentBpm = localBpms[idx];
            double currentInterval = 60.0 / currentBpm;

            currentPos += currentInterval;
            beatCount++;
        }
    } else {
        // Uniform deterministic beat grid
        double currentPos = firstOnsetSec;
        size_t beatCount = 0;
        while (currentPos < totalDurationSec) {
            double roundedPos = std::round(currentPos * 1000.0) / 1000.0;
            beats.push_back(roundedPos);

            if ((beatCount % 4) == bestDownbeatOffset) {
                downbeats.push_back(roundedPos);
                bars.push_back(roundedPos);
            }

            currentPos += beatIntervalSec;
            beatCount++;
        }
    }

    // 8. Musical Phrase Boundary Analysis (4, 8, 16, 32 Bars)
    PhraseProfile phraseProf;
    if (!bars.empty() && confidence > 0.0f) {
        for (size_t i = 0; i < bars.size(); ++i) {
            if (i % 4 == 0) {
                phraseProf.boundaries4Bar.push_back(bars[i]);
            }
            if (i % 8 == 0) {
                phraseProf.boundaries8Bar.push_back(bars[i]);
            }
            if (i % 16 == 0) {
                phraseProf.boundaries16Bar.push_back(bars[i]);
            }
            if (i % 32 == 0) {
                phraseProf.boundaries32Bar.push_back(bars[i]);
            }
        }
        float barCountFactor = std::min(1.0f, static_cast<float>(bars.size()) / 4.0f);
        phraseProf.phraseConfidence = confidence * barCountFactor;
    } else {
        phraseProf.phraseConfidence = 0.0f;
    }

    result.primaryBpm = primaryBpm;
    result.confidence = confidence;
    result.alternativeHypotheses = altHypotheses;
    result.gridOffsetSeconds = std::round(firstOnsetSec * 1000.0) / 1000.0;
    result.beatPositions = beats;
    result.downbeatPositions = downbeats;
    result.barPositions = bars;
    result.phraseProfile = phraseProf;
    result.isVariableTempo = isVariableTempo;
    result.tempoDriftMinBpm = driftMinBpm;
    result.tempoDriftMaxBpm = driftMaxBpm;

    return result;
}

} // namespace pulse::audio
