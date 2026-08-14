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
)

# Static Library Target
add_library(pulse_audio_engine STATIC ${AUDIO_ENGINE_SOURCES})

# Target macOS Frameworks
if(APPLE)
    target_link_libraries(pulse_audio_engine PRIVATE
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework Accelerate"
    )
endif()

# Test Target
enable_testing()
add_executable(test_audio_bridge tests/test_audio_bridge.cpp)
target_link_libraries(test_audio_bridge PRIVATE pulse_audio_engine)

add_test(NAME AudioBridgeSmokeTest COMMAND test_audio_bridge)
