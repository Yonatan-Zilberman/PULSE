#include "../include/AudioEngine.h"
#include "../include/AudioDecoder.h"
#include "../include/WavWriter.h"
#include <iostream>
#include <fstream>
#include <cassert>
#include <cmath>
#include <filesystem>

int main() {
    std::cout << "Running PULSE Phase 0 CLI End-to-End Verification Test..." << std::endl;

    std::filesystem::path tempDir = std::filesystem::temp_directory_path() / "pulse_cli_e2e";
    std::filesystem::create_directories(tempDir);

    std::string deckAPath = (tempDir / "track_a.wav").string();
    std::string deckBPath = (tempDir / "track_b.wav").string();
    std::string outWavPath = (tempDir / "mix_out.wav").string();
    std::string outJsonPath = (tempDir / "report_out.json").string();

    // 1. Generate synthetic tracks A (440Hz, 4.0s) and B (880Hz, 4.0s)
    bool createdA = pulse::audio::WavWriter::createSyntheticFixture(deckAPath, 440.0, 4.0, 120.0, 0.80f, 48000);
    bool createdB = pulse::audio::WavWriter::createSyntheticFixture(deckBPath, 880.0, 4.0, 120.0, 0.75f, 48000);
    assert(createdA && createdB);

    // 2. Initialize Engine and Load Tracks
    auto& engine = pulse::audio::AudioEngine::getInstance();
    AudioEngineConfigC config{48000, 512, 2};
    assert(engine.initialize(config) == 0);

    assert(engine.loadTrack(0, deckAPath));
    assert(engine.loadTrack(1, deckBPath));

    auto* deckA = engine.getDeck(0);
    auto* deckB = engine.getDeck(1);
    auto* mixer = engine.getMixer();
    auto* transExec = engine.getTransitionExecutor();

    assert(deckA != nullptr && deckB != nullptr && mixer != nullptr && transExec != nullptr);

    // 3. Configure transition: start at 2.0s, transition for 2.0s, total mix 6.0s
    double transStartSec = 2.0;
    double transDurationSec = 2.0;
    double totalRenderSec = 6.0;

    mixer->setCrossfader(-1.0f);
    deckA->setPlaybackPosition(0.0);
    deckA->setPlaying(true);

    deckB->setPlaybackPosition(0.0);
    deckB->setPlaying(false);

    uint32_t sampleRate = 48000;
    uint32_t blockSize = 512;
    uint32_t channels = 2;
    double dt = static_cast<double>(blockSize) / sampleRate;

    uint64_t totalFrames = static_cast<uint64_t>(totalRenderSec * sampleRate);
    std::vector<float> renderedMix;
    renderedMix.reserve(totalFrames * channels);

    std::vector<float> block(blockSize * channels, 0.0f);
    double currentTime = 0.0;
    bool transitionTriggered = false;

    while (currentTime < totalRenderSec) {
        if (currentTime >= transStartSec && !transitionTriggered) {
            deckB->setPlaying(true);
            TransitionCommandC cmd{0, 1, transDurationSec, 1};
            transExec->startTransition(cmd);
            transitionTriggered = true;
        }

        if (transitionTriggered && currentTime >= transStartSec) {
            transExec->updateAutomation(currentTime - transStartSec, *mixer);
        }

        engine.processAudioBlock(block.data(), blockSize, channels);
        renderedMix.insert(renderedMix.end(), block.begin(), block.end());
        currentTime += dt;
    }

    uint64_t renderedFrames = renderedMix.size() / channels;
    assert(renderedFrames >= totalFrames);

    // 4. Verify rendered mix is non-silent and not clipping
    float maxPeak = 0.0f;
    for (float s : renderedMix) {
        maxPeak = std::max(maxPeak, std::abs(s));
    }
    assert(maxPeak > 0.5f && maxPeak <= 1.0f);
    std::cout << "  Rendered mix verified: frames=" << renderedFrames << ", maxPeak=" << maxPeak << std::endl;

    // 5. Write output WAV and verify file exists
    bool wavWritten = pulse::audio::WavWriter::writeWav16(outWavPath, renderedMix.data(), renderedFrames, sampleRate, channels);
    assert(wavWritten);
    assert(std::filesystem::exists(outWavPath));
    assert(std::filesystem::file_size(outWavPath) > 1000);

    // 6. Test decoding the newly rendered WAV file
    pulse::audio::DecodedAudio verifyDecoded;
    bool decodeRendered = pulse::audio::AudioDecoder::decodeFile(outWavPath, verifyDecoded, 48000, 2);
    assert(decodeRendered);
    assert(verifyDecoded.sampleRate == 48000);
    assert(verifyDecoded.channels == 2);
    assert(std::abs(verifyDecoded.durationSeconds - totalRenderSec) < 0.1);

    // Cleanup
    std::filesystem::remove_all(tempDir);

    std::cout << "End-to-End CLI verification test PASSED!" << std::endl;
    return 0;
}
