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

    // 3. Fixture Steady 128 BPM: 523.25Hz tone, 128 BPM rhythmic pulses, 10.0s, 48kHz, peak 0.80
    std::string fixture128 = (outputDir / "fixture_steady_128bpm.wav").string();
    pulse::audio::WavWriter::createSyntheticFixture(fixture128, 523.25, 10.0, 128.0, 0.80f, 48000);
    std::cout << "  Generated " << fixture128 << std::endl;

    // 4. Fixture Ambiguous 70/140 BPM: 440Hz tone, 70 BPM base clicks + 140 BPM syncopations, 10.0s
    std::string fixtureAmbiguous = (outputDir / "fixture_ambiguous_70_140bpm.wav").string();
    pulse::audio::WavWriter::createAmbiguousFixture(fixtureAmbiguous, 440.0, 10.0, 70.0, 0.80f, 48000);
    std::cout << "  Generated " << fixtureAmbiguous << std::endl;

    // 5. Fixture Drifting 115-125 BPM: 440Hz tone, smooth linear acceleration 115 -> 125 BPM, 10.0s
    std::string fixtureDrifting = (outputDir / "fixture_drifting_115_125bpm.wav").string();
    pulse::audio::WavWriter::createDriftingFixture(fixtureDrifting, 440.0, 10.0, 115.0, 125.0, 0.80f, 48000);
    std::cout << "  Generated " << fixtureDrifting << std::endl;

    // 6. Fixture Syncopated Difficult: 440Hz tone, 2.0s silent intro + swing rhythm, 10.0s
    std::string fixtureSyncopated = (outputDir / "fixture_syncopated_difficult.wav").string();
    pulse::audio::WavWriter::createSyncopatedFixture(fixtureSyncopated, 440.0, 10.0, 2.0, 124.0, 0.80f, 48000);
    std::cout << "  Generated " << fixtureSyncopated << std::endl;

    // 7. Sine 440Hz 120 BPM 48k
    std::string sine440 = (outputDir / "sine_440hz_120bpm_48k.wav").string();
    pulse::audio::WavWriter::createSyntheticFixture(sine440, 440.0, 10.0, 120.0, 0.80f, 48000);
    std::cout << "  Generated " << sine440 << std::endl;

    // 8. Sine 880Hz 120 BPM 48k
    std::string sine880 = (outputDir / "sine_880hz_120bpm_48k.wav").string();
    pulse::audio::WavWriter::createSyntheticFixture(sine880, 880.0, 10.0, 120.0, 0.75f, 48000);
    std::cout << "  Generated " << sine880 << std::endl;

    // 9. 0-byte invalid empty file
    std::string emptyFile = (outputDir / "invalid_empty.wav").string();
    {
        std::ofstream ofs(emptyFile, std::ios::binary);
    }
    std::cout << "  Generated " << emptyFile << std::endl;

    // 10. Corrupt header file
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
    pulse::audio::WavWriter::createSyntheticFixture((goldenDir / "golden_steady_128bpm.wav").string(), 523.25, 10.0, 128.0, 0.80f, 48000);
    pulse::audio::WavWriter::createAmbiguousFixture((goldenDir / "golden_ambiguous_70_140bpm.wav").string(), 440.0, 10.0, 70.0, 0.80f, 48000);
    pulse::audio::WavWriter::createDriftingFixture((goldenDir / "golden_drifting_115_125bpm.wav").string(), 440.0, 10.0, 115.0, 125.0, 0.80f, 48000);
    pulse::audio::WavWriter::createSyncopatedFixture((goldenDir / "golden_syncopated_difficult.wav").string(), 440.0, 10.0, 2.0, 124.0, 0.80f, 48000);

    std::cout << "Audio fixtures generation complete!" << std::endl;
    return 0;
}
