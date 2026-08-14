#include "../include/AudioDecoder.h"
#include "../include/WavWriter.h"
#include <iostream>
#include <cassert>
#include <cmath>
#include <fstream>
#include <filesystem>

int main() {
    std::cout << "Running PULSE AudioDecoder Unit Tests..." << std::endl;

    std::filesystem::path tempDir = std::filesystem::temp_directory_path() / "pulse_decoder_tests";
    std::filesystem::create_directories(tempDir);

    std::string validWavPath = (tempDir / "valid_fixture.wav").string();
    std::string emptyWavPath = (tempDir / "empty.wav").string();
    std::string corruptWavPath = (tempDir / "corrupt.wav").string();
    std::string missingWavPath = (tempDir / "does_not_exist.wav").string();

    // 1. Create a known golden fixture (440Hz, 3.0s, 120 BPM, peak 0.80)
    bool created = pulse::audio::WavWriter::createSyntheticFixture(
        validWavPath, 440.0, 3.0, 120.0, 0.80f, 48000
    );
    assert(created);

    // 2. Create 0-byte file
    {
        std::ofstream emptyFile(emptyWavPath, std::ios::binary);
    }

    // 3. Create corrupt header file
    {
        std::ofstream corruptFile(corruptWavPath, std::ios::binary);
        const char garbage[20] = "NOT_A_VALID_HEADER!";
        corruptFile.write(garbage, sizeof(garbage));
    }

    // Test 1: Decode Valid File
    std::cout << "Test 1: Decoding valid WAV fixture..." << std::endl;
    pulse::audio::DecodedAudio decoded;
    bool success = pulse::audio::AudioDecoder::decodeFile(validWavPath, decoded, 48000, 2);
    assert(success);
    assert(decoded.sampleRate == 48000);
    assert(decoded.channels == 2);
    assert(std::abs(decoded.durationSeconds - 3.0) < 0.01);
    assert(decoded.samples.size() == 48000 * 2 * 3);
    assert(decoded.peakAmplitude > 0.60f && decoded.peakAmplitude <= 0.85f);
    assert(!decoded.clippingDetected);
    std::cout << "  Decoded valid file successfully: duration=" << decoded.durationSeconds
              << "s, peak=" << decoded.peakAmplitude << ", bpm=" << decoded.detectedBpm << std::endl;

    // Test 2: Missing File
    std::cout << "Test 2: Decoding non-existent file..." << std::endl;
    pulse::audio::DecodedAudio missingDecoded;
    bool missingRes = pulse::audio::AudioDecoder::decodeFile(missingWavPath, missingDecoded);
    assert(!missingRes);
    std::cout << "  Correctly failed on missing file." << std::endl;

    // Test 3: Empty (0-byte) File
    std::cout << "Test 3: Decoding 0-byte empty file..." << std::endl;
    pulse::audio::DecodedAudio emptyDecoded;
    bool emptyRes = pulse::audio::AudioDecoder::decodeFile(emptyWavPath, emptyDecoded);
    assert(!emptyRes);
    std::cout << "  Correctly failed on 0-byte file." << std::endl;

    // Test 4: Corrupt Header File
    std::cout << "Test 4: Decoding corrupt header file..." << std::endl;
    pulse::audio::DecodedAudio corruptDecoded;
    bool corruptRes = pulse::audio::AudioDecoder::decodeFile(corruptWavPath, corruptDecoded);
    assert(!corruptRes);
    std::cout << "  Correctly failed on corrupted header." << std::endl;

    // Cleanup temp directory
    std::filesystem::remove_all(tempDir);

    std::cout << "All AudioDecoder tests PASSED!" << std::endl;
    return 0;
}
