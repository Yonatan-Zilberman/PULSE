#include "../include/AudioDecoder.h"
#include "../include/WavWriter.h"
#include <iostream>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>

int main() {
    std::cout << "Running PULSE Phrase Boundary Analysis Unit Tests..." << std::endl;

    std::filesystem::path tempDir = std::filesystem::temp_directory_path() / "pulse_phrase_tests";
    std::filesystem::create_directories(tempDir);

    std::string steady120Path = (tempDir / "phrase_steady_120.wav").string();
    std::string steady128Path = (tempDir / "phrase_steady_128.wav").string();
    std::string shortAudioPath = (tempDir / "short_audio.wav").string();
    std::string emptyWavPath = (tempDir / "empty.wav").string();
    std::string corruptWavPath = (tempDir / "corrupt.wav").string();

    constexpr uint32_t kSampleRate = 48000;
    constexpr uint32_t kChannels = 2;

    // 1. Generate test fixtures
    // 20 bars at 120 BPM = 40.0s (1 bar = 2.0s, 4 bars = 8.0s, 8 bars = 16.0s, 16 bars = 32.0s)
    assert(pulse::audio::WavWriter::createPhraseFixture(steady120Path, 440.0, 20, 120.0, 0.80f, kSampleRate));

    // 16 bars at 128 BPM = 30.0s (1 bar = 1.875s, 4 bars = 7.5s, 8 bars = 15.0s)
    assert(pulse::audio::WavWriter::createPhraseFixture(steady128Path, 523.25, 16, 128.0, 0.80f, kSampleRate));

    // Short audio: 2 bars at 120 BPM = 4.0s (less than 4-bar phrase)
    assert(pulse::audio::WavWriter::createPhraseFixture(shortAudioPath, 440.0, 2, 120.0, 0.80f, kSampleRate));

    {
        std::ofstream emptyFile(emptyWavPath, std::ios::binary);
    }
    {
        std::ofstream corruptFile(corruptWavPath, std::ios::binary);
        const char garbage[16] = "NOT_AN_AUDIO!";
        corruptFile.write(garbage, sizeof(garbage));
    }

    // Test 1: Steady 120 BPM (20 bars) Phrase Boundary Analysis
    std::cout << "Test 1: Analyzing 120 BPM 20-bar phrase boundaries..." << std::endl;
    pulse::audio::DecodedAudio dec120;
    assert(pulse::audio::AudioDecoder::decodeFile(steady120Path, dec120, kSampleRate, kChannels));
    assert(std::abs(dec120.detectedBpm - 120.0) <= 1.5);
    assert(dec120.phraseProfile.phraseConfidence >= 0.70f);

    const auto& p120 = dec120.phraseProfile;
    std::cout << "  120 BPM Boundaries count: 4-bar=" << p120.boundaries4Bar.size()
              << ", 8-bar=" << p120.boundaries8Bar.size()
              << ", 16-bar=" << p120.boundaries16Bar.size()
              << ", 32-bar=" << p120.boundaries32Bar.size()
              << " (Confidence: " << p120.phraseConfidence << ")" << std::endl;
    // 20 bars should produce 5 4-bar phrases (bars 0, 4, 8, 12, 16) and 3 8-bar phrases (bars 0, 8, 16)
    assert(p120.boundaries4Bar.size() >= 5);
    assert(p120.boundaries8Bar.size() >= 3);
    assert(p120.boundaries16Bar.size() >= 2); // bars 0, 16

    // Verify exact timestamps: at 120 BPM, bar = 2.0s
    // Bar 0 = 0.0s, Bar 4 = 8.0s, Bar 8 = 16.0s, Bar 16 = 32.0s
    assert(std::abs(p120.boundaries4Bar[0] - 0.0) < 0.05);
    assert(std::abs(p120.boundaries4Bar[1] - 8.0) < 0.15);
    assert(std::abs(p120.boundaries8Bar[1] - 16.0) < 0.15);
    assert(std::abs(p120.boundaries16Bar[1] - 32.0) < 0.20);
    std::cout << "  Passed: Exact 4-bar (8.0s), 8-bar (16.0s), and 16-bar (32.0s) timestamps verified." << std::endl;

    // Test 2: Steady 128 BPM (16 bars) Phrase Boundary Analysis
    std::cout << "Test 2: Analyzing 128 BPM 16-bar phrase boundaries..." << std::endl;
    pulse::audio::DecodedAudio dec128;
    assert(pulse::audio::AudioDecoder::decodeFile(steady128Path, dec128, kSampleRate, kChannels));
    assert(std::abs(dec128.detectedBpm - 128.0) <= 1.5);
    assert(dec128.phraseProfile.phraseConfidence >= 0.60f);

    const auto& p128 = dec128.phraseProfile;
    std::cout << "  128 BPM Boundaries count: 4-bar=" << p128.boundaries4Bar.size()
              << ", 8-bar=" << p128.boundaries8Bar.size()
              << ", 16-bar=" << p128.boundaries16Bar.size()
              << " (Confidence: " << p128.phraseConfidence << ")" << std::endl;

    // 1 bar = 1.875s, 4 bars = 7.5s, 8 bars = 15.0s
    assert(p128.boundaries4Bar.size() >= 3); // bars 0, 4, 8
    assert(p128.boundaries8Bar.size() >= 2); // bars 0, 8
    assert(std::abs(p128.boundaries4Bar[1] - 7.5) < 0.20);
    assert(std::abs(p128.boundaries8Bar[1] - 15.0) < 0.25);
    std::cout << "  Passed: Exact 128 BPM 4-bar (7.5s) and 8-bar (15.0s) timestamps verified." << std::endl;

    // Test 3: Short Audio (< 4 bars, 2 bars = 4.0s)
    std::cout << "Test 3: Analyzing short audio (< 4 bars)..." << std::endl;
    pulse::audio::DecodedAudio decShort;
    assert(pulse::audio::AudioDecoder::decodeFile(shortAudioPath, decShort, kSampleRate, kChannels));
    const auto& pShort = decShort.phraseProfile;
    // Should have 1 4-bar entry (bar 0) and 0 further 4-bar entries, and no crashes
    assert(pShort.boundaries4Bar.size() == 1);
    assert(pShort.boundaries8Bar.size() == 1);
    std::cout << "  Passed: Short audio handled safely without out-of-bounds access." << std::endl;

    // Test 4: Empty and Corrupt Inputs
    std::cout << "Test 4: Analyzing empty and corrupt inputs..." << std::endl;
    pulse::audio::DecodedAudio decEmpty;
    assert(!pulse::audio::AudioDecoder::decodeFile(emptyWavPath, decEmpty));
    assert(decEmpty.phraseProfile.phraseConfidence == 0.0f);
    assert(decEmpty.phraseProfile.boundaries4Bar.empty());

    pulse::audio::DecodedAudio decCorrupt;
    assert(!pulse::audio::AudioDecoder::decodeFile(corruptWavPath, decCorrupt));
    assert(decCorrupt.phraseProfile.phraseConfidence == 0.0f);
    assert(decCorrupt.phraseProfile.boundaries4Bar.empty());
    std::cout << "  Passed: Corrupt and empty audio correctly rejected with zero confidence." << std::endl;

    // Cleanup
    std::filesystem::remove_all(tempDir);

    std::cout << "All Phrase Boundary Analysis tests PASSED!" << std::endl;
    return 0;
}
