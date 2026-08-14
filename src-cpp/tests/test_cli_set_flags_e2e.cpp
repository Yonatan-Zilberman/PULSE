#include "../include/WavWriter.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cassert>
#include <cstdlib>
#include <filesystem>

int main() {
    std::cout << "Running PULSE Phase 0 CLI Set Flags E2E Test..." << std::endl;

    std::filesystem::path tempDir = std::filesystem::temp_directory_path() / "pulse_cli_set_flags_e2e";
    std::filesystem::create_directories(tempDir);

    std::string track1 = (tempDir / "track1.wav").string();
    std::string track2 = (tempDir / "track2.wav").string();
    std::string track3 = (tempDir / "track3.wav").string();

    assert(pulse::audio::WavWriter::createSyntheticFixture(track1, 440.0, 5.0, 120.0, 0.8f, 48000));
    assert(pulse::audio::WavWriter::createSyntheticFixture(track2, 523.25, 5.0, 122.0, 0.8f, 48000));
    assert(pulse::audio::WavWriter::createSyntheticFixture(track3, 659.25, 5.0, 124.0, 0.8f, 48000));

    // 1. Create a playlist JSON manifest
    std::string playlistJson = (tempDir / "test_playlist.json").string();
    {
        std::ofstream plFile(playlistJson);
        assert(plFile.is_open());
        plFile << "{\n"
               << "  \"tracks\": [\n"
               << "    \"" << track1 << "\",\n"
               << "    \"" << track2 << "\",\n"
               << "    \"" << track3 << "\"\n"
               << "  ]\n"
               << "}\n";
    }

    std::string outWav = (tempDir / "set_flags_mix_out.wav").string();
    std::string outReport = (tempDir / "set_flags_report.json").string();

    // 2. Test CLI execution with --playlist
    std::string cliBin = "./pulse_cli";
    if (!std::filesystem::exists(cliBin)) {
        cliBin = "./src-cpp/build/pulse_cli";
    }
    if (!std::filesystem::exists(cliBin)) {
        cliBin = "./build/pulse_cli";
    }
    if (!std::filesystem::exists(cliBin)) {
        cliBin = "pulse_cli";
    }

    std::ostringstream cmd1;
    cmd1 << cliBin << " --playlist " << playlistJson
         << " --transition-strategy bass_swap"
         << " --export-snippets"
         << " --out " << outWav
         << " --report " << outReport;

    std::cout << "  Executing: " << cmd1.str() << "\n";
    int ret1 = std::system(cmd1.str().c_str());
    assert(ret1 == 0);
    assert(std::filesystem::exists(outWav));
    assert(std::filesystem::exists(outReport));

    // Verify report contents
    {
        std::ifstream rf(outReport);
        std::stringstream ss;
        ss << rf.rdbuf();
        std::string s = ss.str();
        assert(s.find("\"total_tracks\": 3") != std::string::npos);
        assert(s.find("\"total_transitions\": 2") != std::string::npos);
        assert(s.find("\"clipping_detected\": false") != std::string::npos);
    }

    // 3. Test CLI execution with --track-dir and --auto-sequence
    std::string outWav2 = (tempDir / "track_dir_out.wav").string();
    std::string outReport2 = (tempDir / "track_dir_report.json").string();

    std::ostringstream cmd2;
    cmd2 << cliBin << " --track-dir " << tempDir.string()
         << " --auto-sequence"
         << " --min-transitions 2"
         << " --out " << outWav2
         << " --report " << outReport2;

    std::cout << "  Executing: " << cmd2.str() << "\n";
    int ret2 = std::system(cmd2.str().c_str());
    assert(ret2 == 0);
    assert(std::filesystem::exists(outWav2));
    assert(std::filesystem::exists(outReport2));

    // 4. Test error handling on non-existent playlist
    std::ostringstream cmdErr;
    cmdErr << cliBin << " --playlist " << (tempDir / "non_existent.json").string() << " > /dev/null 2>&1";
    int retErr = std::system(cmdErr.str().c_str());
    assert(retErr != 0);

    // Cleanup
    std::filesystem::remove_all(tempDir);

    std::cout << "PULSE Phase 0 CLI Set Flags E2E Test PASSED!\n";
    return 0;
}
