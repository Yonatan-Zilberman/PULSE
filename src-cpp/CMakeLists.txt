cmake_minimum_required(VERSION 3.20)
project(PulseAudioEngine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler Warnings
if(MSVC)
    add_compile_options(/W4 /WX)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

# Include Directories
include_directories(include)

# Source Files
set(AUDIO_ENGINE_SOURCES
    src/AudioEngine.cpp
    src/DeckPlayer.cpp
    src/Mixer.cpp
    src/AudioBridge.cpp
    src/AudioDecoder.cpp
    src/WavWriter.cpp
    src/TimeStretchEngine.cpp
    src/TempoStrategy.cpp
    src/TransitionPlanner.cpp
    src/TransitionExecutor.cpp
)

# Static Library Target
add_library(pulse_audio_engine STATIC ${AUDIO_ENGINE_SOURCES})

# Target macOS Frameworks
if(APPLE)
    target_link_libraries(pulse_audio_engine PRIVATE
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework Accelerate"
        "-framework CoreFoundation"
    )
endif()

# Standalone CLI Prototype Executable
add_executable(pulse_cli src/pulse_cli.cpp)
target_link_libraries(pulse_cli PRIVATE pulse_audio_engine)
if(APPLE)
    target_link_libraries(pulse_cli PRIVATE
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework Accelerate"
        "-framework CoreFoundation"
    )
endif()

# Fixture Generator Utility
add_executable(generate_fixtures src/generate_fixtures.cpp)
target_link_libraries(generate_fixtures PRIVATE pulse_audio_engine)
if(APPLE)
    target_link_libraries(generate_fixtures PRIVATE
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework Accelerate"
        "-framework CoreFoundation"
    )
endif()

# Test Targets
enable_testing()

add_executable(test_audio_bridge tests/test_audio_bridge.cpp)
target_link_libraries(test_audio_bridge PRIVATE pulse_audio_engine)
if(APPLE)
    target_link_libraries(test_audio_bridge PRIVATE
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework Accelerate"
        "-framework CoreFoundation"
    )
endif()
add_test(NAME AudioBridgeSmokeTest COMMAND test_audio_bridge)

add_executable(test_audio_decoder tests/test_audio_decoder.cpp)
target_link_libraries(test_audio_decoder PRIVATE pulse_audio_engine)
if(APPLE)
    target_link_libraries(test_audio_decoder PRIVATE
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework Accelerate"
        "-framework CoreFoundation"
    )
endif()
add_test(NAME AudioDecoderTest COMMAND test_audio_decoder)

add_executable(test_mixer_dsp tests/test_mixer_dsp.cpp)
target_link_libraries(test_mixer_dsp PRIVATE pulse_audio_engine)
if(APPLE)
    target_link_libraries(test_mixer_dsp PRIVATE
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework Accelerate"
        "-framework CoreFoundation"
    )
endif()
add_test(NAME MixerDSPTest COMMAND test_mixer_dsp)

add_executable(test_cli_e2e tests/test_cli_e2e.cpp)
target_link_libraries(test_cli_e2e PRIVATE pulse_audio_engine)
if(APPLE)
    target_link_libraries(test_cli_e2e PRIVATE
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework Accelerate"
        "-framework CoreFoundation"
    )
endif()
add_test(NAME CliEndToEndTest COMMAND test_cli_e2e)

add_executable(test_time_stretch tests/test_time_stretch.cpp)
target_link_libraries(test_time_stretch PRIVATE pulse_audio_engine)
if(APPLE)
    target_link_libraries(test_time_stretch PRIVATE
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework Accelerate"
        "-framework CoreFoundation"
    )
endif()
add_test(NAME TimeStretchTest COMMAND test_time_stretch)

add_executable(test_cli_tempo_match_e2e tests/test_cli_tempo_match_e2e.cpp)
target_link_libraries(test_cli_tempo_match_e2e PRIVATE pulse_audio_engine)
if(APPLE)
    target_link_libraries(test_cli_tempo_match_e2e PRIVATE
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework Accelerate"
        "-framework CoreFoundation"
    )
endif()
add_test(NAME CliTempoMatchEndToEndTest COMMAND test_cli_tempo_match_e2e)

add_executable(test_phrase_analysis tests/test_phrase_analysis.cpp)
target_link_libraries(test_phrase_analysis PRIVATE pulse_audio_engine)
if(APPLE)
    target_link_libraries(test_phrase_analysis PRIVATE
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework Accelerate"
        "-framework CoreFoundation"
    )
endif()
add_test(NAME PhraseAnalysisTest COMMAND test_phrase_analysis)

add_executable(test_transition_planner tests/test_transition_planner.cpp)
target_link_libraries(test_transition_planner PRIVATE pulse_audio_engine)
if(APPLE)
    target_link_libraries(test_transition_planner PRIVATE
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework Accelerate"
        "-framework CoreFoundation"
    )
endif()
add_test(NAME TransitionPlannerTest COMMAND test_transition_planner)

add_executable(test_cli_phrase_transition_e2e tests/test_cli_phrase_transition_e2e.cpp)
target_link_libraries(test_cli_phrase_transition_e2e PRIVATE pulse_audio_engine)
if(APPLE)
    target_link_libraries(test_cli_phrase_transition_e2e PRIVATE
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework Accelerate"
        "-framework CoreFoundation"
    )
endif()
add_test(NAME CliPhraseTransitionEndToEndTest COMMAND test_cli_phrase_transition_e2e)

add_executable(test_eq_dsp tests/test_eq_dsp.cpp)
target_link_libraries(test_eq_dsp PRIVATE pulse_audio_engine)
if(APPLE)
    target_link_libraries(test_eq_dsp PRIVATE
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework Accelerate"
        "-framework CoreFoundation"
    )
endif()
add_test(NAME EqDSPTest COMMAND test_eq_dsp)

add_executable(test_bass_swap_safety tests/test_bass_swap_safety.cpp)
target_link_libraries(test_bass_swap_safety PRIVATE pulse_audio_engine)
if(APPLE)
    target_link_libraries(test_bass_swap_safety PRIVATE
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework Accelerate"
        "-framework CoreFoundation"
    )
endif()
add_test(NAME BassSwapSafetyTest COMMAND test_bass_swap_safety)

add_executable(test_cli_bass_swap_e2e tests/test_cli_bass_swap_e2e.cpp)
target_link_libraries(test_cli_bass_swap_e2e PRIVATE pulse_audio_engine)
if(APPLE)
    target_link_libraries(test_cli_bass_swap_e2e PRIVATE
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework Accelerate"
        "-framework CoreFoundation"
    )
endif()
add_test(NAME CliBassSwapEndToEndTest COMMAND test_cli_bass_swap_e2e)

add_executable(test_set_mixer_e2e tests/test_set_mixer_e2e.cpp)
target_link_libraries(test_set_mixer_e2e PRIVATE pulse_audio_engine)
if(APPLE)
    target_link_libraries(test_set_mixer_e2e PRIVATE
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework Accelerate"
        "-framework CoreFoundation"
    )
endif()
add_test(NAME SetMixerEndToEndTest COMMAND test_set_mixer_e2e)

add_executable(test_cli_set_flags_e2e tests/test_cli_set_flags_e2e.cpp)
target_link_libraries(test_cli_set_flags_e2e PRIVATE pulse_audio_engine)
if(APPLE)
    target_link_libraries(test_cli_set_flags_e2e PRIVATE
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework Accelerate"
        "-framework CoreFoundation"
    )
endif()
add_test(NAME CliSetFlagsEndToEndTest COMMAND test_cli_set_flags_e2e)

