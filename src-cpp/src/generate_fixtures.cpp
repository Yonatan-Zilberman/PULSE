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

    // 1. Generate base unit test fixtures in tests/audio
    std::filesystem::path audioDir = "tests/audio";
    std::filesystem::create_directories(audioDir);

    pulse::audio::WavWriter::createSyntheticFixture((audioDir / "fixture_a.wav").string(), 440.0, 10.0, 120.0, 0.80f, 48000);
    pulse::audio::WavWriter::createSyntheticFixture((audioDir / "fixture_b.wav").string(), 880.0, 10.0, 120.0, 0.75f, 48000);
    pulse::audio::WavWriter::createSyntheticFixture((audioDir / "fixture_steady_128bpm.wav").string(), 523.25, 10.0, 128.0, 0.80f, 48000);
    pulse::audio::WavWriter::createAmbiguousFixture((audioDir / "fixture_ambiguous_70_140bpm.wav").string(), 440.0, 10.0, 70.0, 0.80f, 48000);
    pulse::audio::WavWriter::createDriftingFixture((audioDir / "fixture_drifting_115_125bpm.wav").string(), 440.0, 10.0, 115.0, 125.0, 0.80f, 48000);
    pulse::audio::WavWriter::createSyncopatedFixture((audioDir / "fixture_syncopated_difficult.wav").string(), 440.0, 10.0, 2.0, 124.0, 0.80f, 48000);
    pulse::audio::WavWriter::createSyntheticFixture((audioDir / "sine_440hz_120bpm_48k.wav").string(), 440.0, 10.0, 120.0, 0.80f, 48000);
    pulse::audio::WavWriter::createSyntheticFixture((audioDir / "sine_880hz_120bpm_48k.wav").string(), 880.0, 10.0, 120.0, 0.75f, 48000);
    pulse::audio::WavWriter::createTempoTestFixture((audioDir / "fixture_tempo_small_122bpm.wav").string(), 440.0, 10.0, 122.0, 0.80f, 48000);
    pulse::audio::WavWriter::createTempoTestFixture((audioDir / "fixture_tempo_moderate_126bpm.wav").string(), 440.0, 10.0, 126.0, 0.80f, 48000);
    pulse::audio::WavWriter::createTempoTestFixture((audioDir / "fixture_tempo_octave_140bpm.wav").string(), 440.0, 10.0, 140.0, 0.80f, 48000);
    pulse::audio::WavWriter::createTempoTestFixture((audioDir / "fixture_tempo_excessive_150bpm.wav").string(), 440.0, 10.0, 150.0, 0.80f, 48000);
    pulse::audio::WavWriter::createBassHeavyFixture((audioDir / "fixture_bass_heavy_120bpm.wav").string(), 60.0, 440.0, 10.0, 120.0, 0.85f, 48000);
    pulse::audio::WavWriter::createBassHeavyFixture((audioDir / "fixture_bass_heavy_122bpm.wav").string(), 60.0, 440.0, 10.0, 122.0, 0.85f, 48000);
    pulse::audio::WavWriter::createHotSignalFixture((audioDir / "fixture_hot_0dbfs.wav").string(), 10.0, 120.0, 0.98f, 48000);

    // Negative unit test fixtures (empty / corrupt) specifically in tests/audio
    {
        std::ofstream ofs((audioDir / "invalid_empty.wav").string(), std::ios::binary);
    }
    {
        std::ofstream ofs((audioDir / "corrupt_header.wav").string(), std::ios::binary);
        const char garbage[32] = "RIFF\0\0\0\0CORRUPT_HEADER_BYTES";
        ofs.write(garbage, sizeof(garbage));
    }

    // 2. Generate 25-Track Golden Corpus in tests/golden-set (or custom outputDir if requested)
    std::filesystem::path goldenDir = outputDir;
    if (goldenDir == "tests/audio") {
        goldenDir = "tests/golden-set";
    }
    std::filesystem::create_directories(goldenDir);

    // Legacy golden set fixtures for backwards-compatibility
    pulse::audio::WavWriter::createSyntheticFixture((goldenDir / "golden_track_440hz_120bpm.wav").string(), 440.0, 10.0, 120.0, 0.80f, 48000);
    pulse::audio::WavWriter::createSyntheticFixture((goldenDir / "golden_track_880hz_120bpm.wav").string(), 880.0, 10.0, 120.0, 0.75f, 48000);
    pulse::audio::WavWriter::createSyntheticFixture((goldenDir / "golden_steady_128bpm.wav").string(), 523.25, 10.0, 128.0, 0.80f, 48000);
    pulse::audio::WavWriter::createAmbiguousFixture((goldenDir / "golden_ambiguous_70_140bpm.wav").string(), 440.0, 10.0, 70.0, 0.80f, 48000);
    pulse::audio::WavWriter::createDriftingFixture((goldenDir / "golden_drifting_115_125bpm.wav").string(), 440.0, 10.0, 115.0, 125.0, 0.80f, 48000);
    pulse::audio::WavWriter::createSyncopatedFixture((goldenDir / "golden_syncopated_difficult.wav").string(), 440.0, 10.0, 2.0, 124.0, 0.80f, 48000);
    pulse::audio::WavWriter::createBassHeavyFixture((goldenDir / "fixture_bass_heavy_120bpm.wav").string(), 60.0, 440.0, 10.0, 120.0, 0.85f, 48000);
    pulse::audio::WavWriter::createBassHeavyFixture((goldenDir / "fixture_bass_heavy_122bpm.wav").string(), 60.0, 440.0, 10.0, 122.0, 0.85f, 48000);
    pulse::audio::WavWriter::createHotSignalFixture((goldenDir / "fixture_hot_0dbfs.wav").string(), 10.0, 120.0, 0.98f, 48000);

    // 25-Track Golden Corpus: golden_track_01.wav through golden_track_25.wav
    const double bpms[25] = {
        120.0, 121.0, 121.5, 122.0, 122.5, 123.0, 123.5, 124.0,
        124.5, 125.0, 125.5, 126.0, 126.5, 127.0, 127.5, 128.0,
        128.5, 129.0, 129.5, 130.0, 128.0, 126.0, 124.0, 122.0, 120.0
    };

    const double baseFreqs[25] = {
        440.00, 466.16, 493.88, 523.25, 554.37, 587.33, 622.25, 659.25,
        698.46, 739.99, 783.99, 830.61, 880.00, 440.00, 493.88, 523.25,
        587.33, 659.25, 698.46, 783.99, 880.00, 523.25, 440.00, 493.88, 523.25
    };

    const double durations[25] = {
        32.0, 32.0, 32.0, 32.0, 32.0, 32.0, 32.0, 32.0,
        32.0, 32.0, 32.0, 32.0, 32.0, 32.0, 32.0, 32.0,
        32.0, 32.0, 32.0, 32.0, 32.0, 32.0, 32.0, 32.0, 32.0
    };

    for (int t = 0; t < 25; ++t) {
        char trackName[64];
        std::snprintf(trackName, sizeof(trackName), "golden_track_%02d.wav", t + 1);
        std::string trackPath = (goldenDir / trackName).string();
        pulse::audio::WavWriter::createBassHeavyFixture(
            trackPath,
            60.0,                // 60 Hz sub-bass
            baseFreqs[t],        // Harmonic key frequency
            durations[t],        // 32.0s duration
            bpms[t],             // Tempo
            0.82f,               // Headroom-safe amplitude
            48000
        );
        std::cout << "  Generated Golden Set Track [" << (t + 1) << "/25]: " << trackName
                  << " (" << bpms[t] << " BPM, " << baseFreqs[t] << " Hz, " << durations[t] << "s)\n";
    }

    std::cout << "Audio fixtures and 25-track golden corpus generation complete!" << std::endl;
    return 0;
}
