#include "../include/TimeStretchEngine.h"
#include <cmath>
#include <vector>
#include <algorithm>
#include <cstring>
#include <numbers>

namespace pulse::audio {

struct TimeStretchEngine::Impl {
    TimeStretchConfig config;
    std::atomic<double> tempoRatio{1.0};

    // WSOLA Configuration Parameters
    uint32_t windowSize{1024};       // W (~21.3ms at 48kHz)
    uint32_t synthHop{512};          // Ss = W / 2
    uint32_t maxSearchDelta{480};     // Delta_max (~10ms at 48kHz)

    // Precomputed Hann Window
    std::vector<float> window;

    // Fixed-size Circular Buffers (Pre-allocated for Real-Time Safety)
    std::vector<float> inQueue;
    size_t inReadIndex{0};
    size_t inWriteIndex{0};
    size_t inCount{0};
    size_t inCapacity{0};

    std::vector<float> outQueue;
    size_t outReadIndex{0};
    size_t outWriteIndex{0};
    size_t outCount{0};
    size_t outCapacity{0};

    // Synthesis Overlap-Add State
    std::vector<float> overlapBuffer; // size = windowSize * channels
    std::vector<float> prevTarget;    // size = windowSize (mono)
    double nominalAnalysisPos{0.0};
    bool hasInitialGrain{false};

    // Scratch buffers for correlation search
    std::vector<float> monoInScratch;

    void initBuffers(const TimeStretchConfig& cfg) {
        config = cfg;
        tempoRatio.store(std::max(0.25, std::min(4.0, cfg.tempoRatio)));

        // Window size chosen based on sample rate
        windowSize = (cfg.sampleRate >= 44100) ? 1024 : 512;
        synthHop = windowSize / 2;
        maxSearchDelta = (cfg.sampleRate * 10) / 1000; // 10ms search range

        // Hann window precomputation: w(n) = 0.5 * (1 - cos(2*pi*n / W))
        window.resize(windowSize);
        constexpr double kTwoPi = 2.0 * std::numbers::pi;
        for (uint32_t n = 0; n < windowSize; ++n) {
            window[n] = static_cast<float>(0.5 * (1.0 - std::cos(kTwoPi * static_cast<double>(n) / static_cast<double>(windowSize))));
        }

        uint32_t channels = std::max(1u, cfg.channels);
        // Preallocate generous buffer capacity (e.g. 32 seconds of audio)
        inCapacity = std::max(131072u, cfg.sampleRate * 32) * channels;
        outCapacity = std::max(131072u, cfg.sampleRate * 32) * channels;

        inQueue.assign(inCapacity, 0.0f);
        outQueue.assign(outCapacity, 0.0f);

        inReadIndex = 0;
        inWriteIndex = 0;
        inCount = 0;

        outReadIndex = 0;
        outWriteIndex = 0;
        outCount = 0;

        overlapBuffer.assign(windowSize * channels, 0.0f);
        prevTarget.assign(windowSize, 0.0f);
        monoInScratch.assign((windowSize + 2 * maxSearchDelta) * channels, 0.0f);

        nominalAnalysisPos = 0.0;
        hasInitialGrain = false;
    }

    void reset() {
        inReadIndex = 0;
        inWriteIndex = 0;
        inCount = 0;

        outReadIndex = 0;
        outWriteIndex = 0;
        outCount = 0;

        std::fill(overlapBuffer.begin(), overlapBuffer.end(), 0.0f);
        std::fill(prevTarget.begin(), prevTarget.end(), 0.0f);
        nominalAnalysisPos = 0.0;
        hasInitialGrain = false;
    }

    void writeInQueue(const float* samples, size_t numFrames) {
        uint32_t channels = config.channels;
        size_t totalSamples = numFrames * channels;
        for (size_t i = 0; i < totalSamples; ++i) {
            if (inCount >= inCapacity) {
                // Buffer full guard
                break;
            }
            inQueue[inWriteIndex] = samples[i];
            inWriteIndex = (inWriteIndex + 1) % inCapacity;
            inCount++;
        }
    }

    float getInSample(size_t frameOffset, uint32_t channel) const {
        uint32_t channels = config.channels;
        size_t sampleOffset = (frameOffset * channels + channel);
        if (sampleOffset >= inCount) return 0.0f;
        size_t idx = (inReadIndex + sampleOffset) % inCapacity;
        return inQueue[idx];
    }

    void dropInFrames(size_t framesToDrop) {
        uint32_t channels = config.channels;
        size_t samplesToDrop = std::min(inCount, framesToDrop * channels);
        inReadIndex = (inReadIndex + samplesToDrop) % inCapacity;
        inCount -= samplesToDrop;
    }

    void writeOutSample(float sample) {
        if (outCount < outCapacity) {
            outQueue[outWriteIndex] = sample;
            outWriteIndex = (outWriteIndex + 1) % outCapacity;
            outCount++;
        }
    }

    void processWSOLA() {
        uint32_t channels = config.channels;
        double ratio = tempoRatio.load();

        // 1. Fast path: Direct pass-through if tempo ratio is 1.0 (no stretch)
        if (std::abs(ratio - 1.0) < 1e-5) {
            size_t availableFrames = inCount / channels;
            if (availableFrames == 0) return;

            for (size_t f = 0; f < availableFrames; ++f) {
                for (uint32_t c = 0; c < channels; ++c) {
                    float s = inQueue[inReadIndex];
                    inReadIndex = (inReadIndex + 1) % inCapacity;
                    inCount--;
                    writeOutSample(s);
                }
            }
            return;
        }

        // 2. WSOLA Processing Loop
        // Required input frames before we can safely extract a grain:
        // windowSize + 2 * maxSearchDelta
        size_t neededFrames = windowSize + 2 * maxSearchDelta;
        double analysisHop = ratio * static_cast<double>(synthHop);

        while ((inCount / channels) >= neededFrames) {
            if (!hasInitialGrain) {
                // Initialize with first windowed grain at input offset 0
                for (uint32_t n = 0; n < windowSize; ++n) {
                    float win = window[n];
                    for (uint32_t c = 0; c < channels; ++c) {
                        float s = getInSample(n, c);
                        overlapBuffer[n * channels + c] = s * win;
                    }
                }

                // Output first synthHop frames from overlapBuffer
                for (uint32_t n = 0; n < synthHop; ++n) {
                    for (uint32_t c = 0; c < channels; ++c) {
                        writeOutSample(overlapBuffer[n * channels + c]);
                    }
                }

                // Shift overlapBuffer left by synthHop
                for (uint32_t n = 0; n < (windowSize - synthHop); ++n) {
                    for (uint32_t c = 0; c < channels; ++c) {
                        overlapBuffer[n * channels + c] = overlapBuffer[(n + synthHop) * channels + c];
                    }
                }
                for (uint32_t n = (windowSize - synthHop); n < windowSize; ++n) {
                    for (uint32_t c = 0; c < channels; ++c) {
                        overlapBuffer[n * channels + c] = 0.0f;
                    }
                }

                // Construct natural continuation target (mono) for cross-correlation matching
                for (uint32_t n = 0; n < windowSize; ++n) {
                    float sum = 0.0f;
                    for (uint32_t c = 0; c < channels; ++c) {
                        sum += getInSample(synthHop + n, c);
                    }
                    prevTarget[n] = sum / static_cast<float>(channels);
                }

                nominalAnalysisPos = analysisHop;
                hasInitialGrain = true;
                continue;
            }

            // Find optimal lag delta around nominalAnalysisPos
            int32_t nominalFrame = static_cast<int32_t>(nominalAnalysisPos);
            int32_t minLag = -static_cast<int32_t>(maxSearchDelta);
            int32_t maxLag = static_cast<int32_t>(maxSearchDelta);

            // Bounds check lag range
            if (nominalFrame + minLag < 0) {
                minLag = -nominalFrame;
            }

            int32_t bestLag = 0;
            float bestCorr = -1e30f;

            for (int32_t lag = minLag; lag <= maxLag; ++lag) {
                int32_t candidateStart = nominalFrame + lag;
                if (candidateStart < 0) continue;

                float dotProd = 0.0f;
                float normSq = 0.0f;

                for (uint32_t n = 0; n < windowSize; n += 2) { // Step by 2 for speed
                    float inMono = 0.0f;
                    for (uint32_t c = 0; c < channels; ++c) {
                        inMono += getInSample(candidateStart + n, c);
                    }
                    inMono /= static_cast<float>(channels);

                    dotProd += prevTarget[n] * inMono;
                    normSq += inMono * inMono;
                }

                float score = (normSq > 1e-7f) ? (dotProd / std::sqrt(normSq)) : dotProd;
                if (score > bestCorr) {
                    bestCorr = score;
                    bestLag = lag;
                }
            }

            uint32_t bestStart = static_cast<uint32_t>(std::max(0, nominalFrame + bestLag));

            // Overlap-add the selected windowed grain into overlapBuffer
            for (uint32_t n = 0; n < windowSize; ++n) {
                float win = window[n];
                for (uint32_t c = 0; c < channels; ++c) {
                    float s = getInSample(bestStart + n, c);
                    overlapBuffer[n * channels + c] += s * win;
                }
            }

            // Output first synthHop frames from overlapBuffer
            for (uint32_t n = 0; n < synthHop; ++n) {
                for (uint32_t c = 0; c < channels; ++c) {
                    writeOutSample(overlapBuffer[n * channels + c]);
                }
            }

            // Shift overlapBuffer left by synthHop
            for (uint32_t n = 0; n < (windowSize - synthHop); ++n) {
                for (uint32_t c = 0; c < channels; ++c) {
                    overlapBuffer[n * channels + c] = overlapBuffer[(n + synthHop) * channels + c];
                }
            }
            for (uint32_t n = (windowSize - synthHop); n < windowSize; ++n) {
                for (uint32_t c = 0; c < channels; ++c) {
                    overlapBuffer[n * channels + c] = 0.0f;
                }
            }

            // Prepare next continuation target
            for (uint32_t n = 0; n < windowSize; ++n) {
                float sum = 0.0f;
                for (uint32_t c = 0; c < channels; ++c) {
                    sum += getInSample(bestStart + synthHop + n, c);
                }
                prevTarget[n] = sum / static_cast<float>(channels);
            }

            // Advance analysis position and drop consumed frames from inQueue
            nominalAnalysisPos += analysisHop;
            if (nominalAnalysisPos >= static_cast<double>(synthHop)) {
                size_t framesToDrop = static_cast<size_t>(nominalAnalysisPos);
                dropInFrames(framesToDrop);
                nominalAnalysisPos -= static_cast<double>(framesToDrop);
            }
        }
    }

    void flushBuffers() {
        uint32_t channels = config.channels;
        // Feed padding to flush remaining frames in inQueue through WSOLA
        size_t padFrames = windowSize + 2 * maxSearchDelta;
        std::vector<float> padding(padFrames * channels, 0.0f);
        writeInQueue(padding.data(), padFrames);
        processWSOLA();

        // Output remaining samples in overlapBuffer
        for (uint32_t n = 0; n < windowSize; ++n) {
            for (uint32_t c = 0; c < channels; ++c) {
                writeOutSample(overlapBuffer[n * channels + c]);
            }
        }
        std::fill(overlapBuffer.begin(), overlapBuffer.end(), 0.0f);
    }
};

TimeStretchEngine::TimeStretchEngine() : impl_(std::make_unique<Impl>()) {}
TimeStretchEngine::~TimeStretchEngine() = default;

TimeStretchEngine::TimeStretchEngine(TimeStretchEngine&&) noexcept = default;
TimeStretchEngine& TimeStretchEngine::operator=(TimeStretchEngine&&) noexcept = default;

bool TimeStretchEngine::initialize(const TimeStretchConfig& config) {
    if (config.sampleRate == 0 || config.channels == 0) return false;
    config_ = config;
    impl_->initBuffers(config);
    return true;
}

void TimeStretchEngine::setTempoRatio(double ratio) {
    config_.tempoRatio = ratio;
    if (impl_) {
        impl_->tempoRatio.store(std::max(0.25, std::min(4.0, ratio)));
    }
}

double TimeStretchEngine::getTempoRatio() const {
    return impl_ ? impl_->tempoRatio.load() : config_.tempoRatio;
}

void TimeStretchEngine::putSamples(const float* samples, uint32_t numFrames) {
    if (!impl_ || !samples || numFrames == 0) return;
    uint32_t channels = config_.channels;
    uint32_t chunkFrames = 1024;
    uint32_t offset = 0;

    while (offset < numFrames) {
        uint32_t toWrite = std::min(chunkFrames, numFrames - offset);
        impl_->writeInQueue(samples + offset * channels, toWrite);
        impl_->processWSOLA();
        offset += toWrite;
    }
}

uint32_t TimeStretchEngine::receiveSamples(float* outputBuffer, uint32_t maxFrames) {
    if (!impl_ || !outputBuffer || maxFrames == 0) return 0;
    uint32_t channels = config_.channels;
    uint32_t availableFrames = static_cast<uint32_t>(impl_->outCount / channels);
    uint32_t framesToRead = std::min(maxFrames, availableFrames);

    for (uint32_t f = 0; f < framesToRead; ++f) {
        for (uint32_t c = 0; c < channels; ++c) {
            outputBuffer[f * channels + c] = impl_->outQueue[impl_->outReadIndex];
            impl_->outReadIndex = (impl_->outReadIndex + 1) % impl_->outCapacity;
            impl_->outCount--;
        }
    }
    return framesToRead;
}

uint32_t TimeStretchEngine::numAvailableSamples() const {
    if (!impl_) return 0;
    return static_cast<uint32_t>(impl_->outCount / config_.channels);
}

void TimeStretchEngine::flush() {
    if (impl_) {
        impl_->flushBuffers();
    }
}

void TimeStretchEngine::clear() {
    if (impl_) {
        impl_->reset();
    }
}

double TimeStretchEngine::getLatencySeconds() const {
    if (!impl_ || config_.sampleRate == 0) return 0.0;
    return static_cast<double>(impl_->windowSize + impl_->maxSearchDelta) / config_.sampleRate;
}

} // namespace pulse::audio
