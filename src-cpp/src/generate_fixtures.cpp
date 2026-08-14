#include "../include/WavWriter.h"
#include <iostream>
#include <fstream>
#include <filesystem>

int main(int argc, char* argv[]) {
    std::filesystem::path outputDir = "tests/audio";
    if (argc > 1) {
        outputDir = argv[1];
    }
    std::filesystem::create_directories(outputDir);

    std::cout << "Generating deterministic PULSE audio fixtures in " << outputDir << "..." << std::endl;

    // 1. Fixture A: 440Hz tone, 120 BPM rhythmic pulses, 10.0s, 48kHz, peak 0.80
    std::string fixtureA = (outputDir / "fixture_a.wav").string();
    pulse::audio::WavWriter::createSyntheticFixture(fixtureA, 440.0, 10.0, 120.0, 0.80f, 48000);
    std::cout << "  Generated " << fixtureA << std::endl;

    // 2. Fixture B: 880Hz tone, 120 BPM rhythmic pulses, 10.0s, 48kHz, peak 0.75
    std::string fixtureB = (outputDir / "fixture_b.wav").string();
    pulse::audio::WavWriter::createSyntheticFixture(fixtureB, 880.0, 10.0, 120.0, 0.75f, 48000);
    std::cout << "  Generated " << fixtureB << std::endl;

    // 3. Sine 440Hz 120 BPM 48k
    std::string sine440 = (outputDir / "sine_440hz_120bpm_48k.wav").string();
    pulse::audio::WavWriter::createSyntheticFixture(sine440, 440.0, 10.0, 120.0, 0.80f, 48000);
    std::cout << "  Generated " << sine440 << std::endl;

    // 4. Sine 880Hz 120 BPM 48k
    std::string sine880 = (outputDir / "sine_880hz_120bpm_48k.wav").string();
    pulse::audio::WavWriter::createSyntheticFixture(sine880, 880.0, 10.0, 120.0, 0.75f, 48000);
    std::cout << "  Generated " << sine880 << std::endl;

    // 5. 0-byte invalid empty file
    std::string emptyFile = (outputDir / "invalid_empty.wav").string();
    {
        std::ofstream ofs(emptyFile, std::ios::binary);
    }
    std::cout << "  Generated " << emptyFile << std::endl;

    // 6. Corrupt header file
    std::string corruptFile = (outputDir / "corrupt_header.wav").string();
    {
        std::ofstream ofs(corruptFile, std::ios::binary);
        const char garbage[32] = "RIFF\0\0\0\0CORRUPT_HEADER_BYTES";
        ofs.write(garbage, sizeof(garbage));
    }
    std::cout << "  Generated " << corruptFile << std::endl;

    // Also populate golden-set
    std::filesystem::path goldenDir = "tests/golden-set";
    std::filesystem::create_directories(goldenDir);
    pulse::audio::WavWriter::createSyntheticFixture((goldenDir / "golden_track_440hz_120bpm.wav").string(), 440.0, 10.0, 120.0, 0.80f, 48000);
    pulse::audio::WavWriter::createSyntheticFixture((goldenDir / "golden_track_880hz_120bpm.wav").string(), 880.0, 10.0, 120.0, 0.75f, 48000);

    std::cout << "Audio fixtures generation complete!" << std::endl;
    return 0;
}
