#include "../include/AudioDecoder.h"
#include "../include/WavWriter.h"
#include <iostream>
#include <cassert>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <algorithm>

int main() {
    std::cout << "Running PULSE AudioDecoder & DSP Beat Tracker Unit Tests..." << std::endl;

    std::filesystem::path tempDir = std::filesystem::temp_directory_path() / "pulse_decoder_tests";
    std::filesystem::create_directories(tempDir);

    std::string steady120Path = (tempDir / "steady_120.wav").string();
    std::string steady128Path = (tempDir / "steady_128.wav").string();
    std::string ambiguousPath = (tempDir / "ambiguous_70_140.wav").string();
    std::string driftingPath = (tempDir / "drifting_115_125.wav").string();
    std::string syncopatedPath = (tempDir / "syncopated_difficult.wav").string();
    std::string emptyWavPath = (tempDir / "empty.wav").string();
    std::string corruptWavPath = (tempDir / "corrupt.wav").string();
    std::string missingWavPath = (tempDir / "does_not_exist.wav").string();

    // 1. Generate test fixtures
    assert(pulse::audio::WavWriter::createSyntheticFixture(steady120Path, 440.0, 10.0, 120.0, 0.80f, 48000));
    assert(pulse::audio::WavWriter::createSyntheticFixture(steady128Path, 523.25, 10.0, 128.0, 0.80f, 48000));
    assert(pulse::audio::WavWriter::createAmbiguousFixture(ambiguousPath, 440.0, 10.0, 70.0, 0.80f, 48000));
    assert(pulse::audio::WavWriter::createDriftingFixture(driftingPath, 440.0, 10.0, 115.0, 125.0, 0.80f, 48000));
    assert(pulse::audio::WavWriter::createSyncopatedFixture(syncopatedPath, 440.0, 10.0, 2.0, 124.0, 0.80f, 48000));

    {
        std::ofstream emptyFile(emptyWavPath, std::ios::binary);
    }
    {
        std::ofstream corruptFile(corruptWavPath, std::ios::binary);
        const char garbage[20] = "NOT_A_VALID_HEADER!";
        corruptFile.write(garbage, sizeof(garbage));
    }

    // Test 1: Decode and Track Steady 120 BPM
    std::cout << "Test 1: Decoding steady 120 BPM fixture..." << std::endl;
    pulse::audio::DecodedAudio dec120;
    assert(pulse::audio::AudioDecoder::decodeFile(steady120Path, dec120, 48000, 2));
    assert(dec120.sampleRate == 48000 && dec120.channels == 2);
    assert(std::abs(dec120.durationSeconds - 10.0) < 0.05);
    assert(std::abs(dec120.detectedBpm - 120.0) <= 1.5);
    assert(dec120.tempoProfile.confidence > 0.60f);
    assert(!dec120.tempoProfile.beatPositions.empty());
    assert(!dec120.tempoProfile.downbeatPositions.empty());
    assert(!dec120.tempoProfile.isVariableTempo);
    std::cout << "  Passed: BPM=" << dec120.detectedBpm << ", Conf=" << dec120.tempoProfile.confidence
              << ", Beats=" << dec120.tempoProfile.beatPositions.size()
              << ", Downbeats=" << dec120.tempoProfile.downbeatPositions.size() << std::endl;

    // Test 2: Steady 128 BPM
    std::cout << "Test 2: Decoding steady 128 BPM fixture..." << std::endl;
    pulse::audio::DecodedAudio dec128;
    assert(pulse::audio::AudioDecoder::decodeFile(steady128Path, dec128, 48000, 2));
    assert(std::abs(dec128.detectedBpm - 128.0) <= 1.5);
    assert(dec128.tempoProfile.confidence > 0.60f);
    std::cout << "  Passed: BPM=" << dec128.detectedBpm << ", Conf=" << dec128.tempoProfile.confidence << std::endl;

    // Test 3: Ambiguous 70 / 140 BPM
    std::cout << "Test 3: Decoding ambiguous 70/140 BPM fixture..." << std::endl;
    pulse::audio::DecodedAudio decAmb;
    assert(pulse::audio::AudioDecoder::decodeFile(ambiguousPath, decAmb, 48000, 2));
    bool is70 = std::abs(decAmb.detectedBpm - 70.0) <= 2.5;
    bool is140 = std::abs(decAmb.detectedBpm - 140.0) <= 3.5;
    assert(is70 || is140);
    // Verify alternative hypothesis has the other candidate
    bool hasAltCandidate = false;
    for (double alt : decAmb.tempoProfile.alternativeHypotheses) {
        if (is70 && std::abs(alt - 140.0) <= 3.5) hasAltCandidate = true;
        if (is140 && std::abs(alt - 70.0) <= 2.5) hasAltCandidate = true;
    }
    assert(hasAltCandidate);
    std::cout << "  Passed: Primary BPM=" << decAmb.detectedBpm << ", Alt hypotheses count="
              << decAmb.tempoProfile.alternativeHypotheses.size() << std::endl;

    // Test 4: Drifting Tempo (115 -> 125 BPM)
    std::cout << "Test 4: Decoding drifting 115-125 BPM fixture..." << std::endl;
    pulse::audio::DecodedAudio decDrift;
    assert(pulse::audio::AudioDecoder::decodeFile(driftingPath, decDrift, 48000, 2));
    assert(decDrift.tempoProfile.isVariableTempo);
    assert(decDrift.tempoProfile.tempoDriftMinBpm < decDrift.tempoProfile.tempoDriftMaxBpm);
    assert(decDrift.tempoProfile.tempoDriftMinBpm >= 110.0 && decDrift.tempoProfile.tempoDriftMaxBpm <= 130.0);
    std::cout << "  Passed: Variable tempo detected! Range: [" << decDrift.tempoProfile.tempoDriftMinBpm
              << " - " << decDrift.tempoProfile.tempoDriftMaxBpm << "] BPM" << std::endl;

    // Test 5: Syncopated Difficult Rhythm with Silence Intro
    std::cout << "Test 5: Decoding syncopated fixture with 2.0s silence intro..." << std::endl;
    pulse::audio::DecodedAudio decSync;
    assert(pulse::audio::AudioDecoder::decodeFile(syncopatedPath, decSync, 48000, 2));
    assert(decSync.tempoProfile.gridOffsetSeconds >= 1.8);
    std::cout << "  Passed: First beat onset detected after silence at "
              << decSync.tempoProfile.gridOffsetSeconds << "s" << std::endl;

    // Test 6: Missing / Empty / Corrupt Files
    std::cout << "Test 6: Error handling on missing, empty, and corrupt files..." << std::endl;
    pulse::audio::DecodedAudio errDec;
    assert(!pulse::audio::AudioDecoder::decodeFile(missingWavPath, errDec));
    assert(!pulse::audio::AudioDecoder::decodeFile(emptyWavPath, errDec));
    assert(!pulse::audio::AudioDecoder::decodeFile(corruptWavPath, errDec));
    std::cout << "  Passed: Correctly rejected invalid files." << std::endl;

    // Cleanup temp directory
    std::filesystem::remove_all(tempDir);

    std::cout << "All AudioDecoder & DSP Beat Tracker tests PASSED!" << std::endl;
    return 0;
}
