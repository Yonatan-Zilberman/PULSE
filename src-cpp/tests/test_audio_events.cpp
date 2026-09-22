#include "../include/AudioBridgeTypes.h"
#include "../include/AudioEngine.h"
#include "../include/WavWriter.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using pulse::audio::AudioEngine;

void createSineWav(const std::string& path, double freqHz, double durationSec, float amp = 0.1f, uint32_t sampleRate = 48000) {
    const uint64_t totalFrames = static_cast<uint64_t>(durationSec * sampleRate);
    std::vector<float> samples(totalFrames * 2, 0.0f);
    constexpr double twoPi = 6.28318530717958647692;
    for (uint64_t f = 0; f < totalFrames; ++f) {
        const double t = static_cast<double>(f) / sampleRate;
        const float value = static_cast<float>(std::sin(twoPi * freqHz * t) * amp);
        samples[f * 2 + 0] = value;
        samples[f * 2 + 1] = value;
    }
    assert(pulse::audio::WavWriter::writeWav16(path, samples.data(), totalFrames, sampleRate, 2));
}

/** Drains the whole engine queue and returns the events. */
std::vector<AudioEventC> drainAll(AudioEngine& engine) {
    std::vector<AudioEventC> events;
    for (;;) {
        AudioEventC buf[64];
        uint32_t dropped = 0;
        const int n = static_cast<int>(engine.drainEvents(buf, 64, &dropped));
        assert(n >= 0 && n <= 64);
        if (n == 0) {
            break;
        }
        for (int i = 0; i < n; ++i) {
            events.push_back(buf[i]);
        }
    }
    return events;
}

size_t countKind(const std::vector<AudioEventC>& events, uint32_t kind) {
    size_t n = 0;
    for (const AudioEventC& e : events) {
        if (e.kind == kind) ++n;
    }
    return n;
}

size_t findFirst(const std::vector<AudioEventC>& events, uint32_t kind) {
    for (size_t i = 0; i < events.size(); ++i) {
        if (events[i].kind == kind) return i;
    }
    return events.size();
}

void require(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "EVENT-TEST FAILURE: %s\n", what);
        std::exit(1);
    }
}

} // namespace

int main() {
    std::printf("=== Running Audio Engine Event Contract Tests ===\n");

    const std::filesystem::path tempDir = std::filesystem::temp_directory_path() / "pulse_audio_events_test";
    std::filesystem::remove_all(tempDir);
    std::filesystem::create_directories(tempDir);

    auto& engine = AudioEngine::getInstance();
    AudioEngineConfigC config{48000, 512, 2};

    // (g) drain with null/0 is a no-op returning 0.
    require(pulse_audio_drain_events(nullptr, 0, nullptr) == 0, "null drain must return 0");
    require(engine.drainEvents(nullptr, 8, nullptr) == 0, "null drain must return 0");
    std::printf("  [g] null/zero drain no-op: OK\n");

    // (a) Lifecycle: init -> start -> stop -> shutdown yields Started, Stopped, Shutdown in order.
    {
        engine.initialize(config);
        drainAll(engine);
        require(engine.start() == 0, "start must succeed");
        require(engine.stop() == 0, "stop must succeed");
        require(engine.shutdown() == 0, "shutdown must succeed");

        const std::vector<AudioEventC> events = drainAll(engine);
        const size_t iStarted = findFirst(events, PULSE_EVT_ENGINE_STARTED);
        const size_t iStopped = findFirst(events, PULSE_EVT_ENGINE_STOPPED);
        const size_t iShutdown = findFirst(events, PULSE_EVT_ENGINE_SHUTDOWN);
        require(countKind(events, PULSE_EVT_ENGINE_STARTED) == 1, "exactly one EngineStarted");
        require(countKind(events, PULSE_EVT_ENGINE_STOPPED) == 1, "exactly one EngineStopped");
        require(countKind(events, PULSE_EVT_ENGINE_SHUTDOWN) == 1, "exactly one EngineShutdown");
        require(iStarted < iStopped && iStopped < iShutdown, "lifecycle events in order");
        std::printf("  [a] lifecycle event order: OK\n");
    }

    // (b) Track loading via the C trampoline.
    {
        engine.initialize(config);
        drainAll(engine);

        require(pulse_audio_load_track(0, "/nonexistent/pulse_missing.wav") == -1, "missing file must fail");
        {
            const std::vector<AudioEventC> events = drainAll(engine);
            require(countKind(events, PULSE_EVT_TRACK_LOAD_FAILED) == 1, "one TrackLoadFailed");
            const AudioEventC& failed = events[findFirst(events, PULSE_EVT_TRACK_LOAD_FAILED)];
            require(failed.deck_id == 0 && failed.code == -1, "TrackLoadFailed payload");
        }

        const std::string wavPath = (tempDir / "sine_2s.wav").string();
        createSineWav(wavPath, 440.0, 2.0, 0.1f);
        require(pulse_audio_load_track(0, wavPath.c_str()) == 0, "generated sine must load");
        {
            const std::vector<AudioEventC> events = drainAll(engine);
            require(countKind(events, PULSE_EVT_TRACK_LOADED) == 1, "one TrackLoaded");
            const AudioEventC& loaded = events[findFirst(events, PULSE_EVT_TRACK_LOADED)];
            require(loaded.deck_id == 0 && loaded.code == 0, "TrackLoaded payload");
            require(std::abs(loaded.detail - 2.0) <= 0.1, "TrackLoaded detail ~= duration");
        }
        std::printf("  [b] track load events: OK\n");
    }

    // (c) play/pause on a loaded deck -> DeckStateChanged with the new state code.
    {
        drainAll(engine);
        require(pulse_audio_play(0) == 0, "play must succeed");
        {
            const std::vector<AudioEventC> events = drainAll(engine);
            require(countKind(events, PULSE_EVT_DECK_STATE_CHANGED) == 1, "one DeckStateChanged on play");
            const AudioEventC& ev = events[findFirst(events, PULSE_EVT_DECK_STATE_CHANGED)];
            require(ev.deck_id == 0 && ev.code == 3, "DeckStateChanged code = Playing(3)");
        }

        require(pulse_audio_pause(0) == 0, "pause must succeed");
        {
            const std::vector<AudioEventC> events = drainAll(engine);
            require(countKind(events, PULSE_EVT_DECK_STATE_CHANGED) == 1, "one DeckStateChanged on pause");
            const AudioEventC& ev = events[findFirst(events, PULSE_EVT_DECK_STATE_CHANGED)];
            require(ev.deck_id == 0 && ev.code == 4, "DeckStateChanged code = Paused(4)");
        }
        std::printf("  [c] deck state change events: OK\n");
    }

    // (d) Invalid deck id 2: all deck-scoped control calls fail, no events emitted.
    {
        drainAll(engine);
        const std::string wavPath = (tempDir / "sine_2s.wav").string();
        require(pulse_audio_load_track(2, wavPath.c_str()) == -1, "load deck 2 must fail");
        require(pulse_audio_prepare_deck(2, wavPath.c_str(), 0.0, 1.0, 1) == -1, "prepare deck 2 must fail");
        require(pulse_audio_play(2) == -1, "play deck 2 must fail");
        require(pulse_audio_pause(2) == -1, "pause deck 2 must fail");
        require(pulse_audio_stop_deck(2) == -1, "stop deck 2 must fail");
        require(pulse_audio_seek(2, 1.0) == -1, "seek deck 2 must fail");
        require(pulse_audio_set_volume(2, 0.5f) == -1, "volume deck 2 must fail");
        require(pulse_audio_set_eq(2, 0.0f, 0.0f, 0.0f) == -1, "eq deck 2 must fail");
        require(pulse_audio_set_filter(2, 0.0f) == -1, "filter deck 2 must fail");
        require(pulse_audio_set_tempo_ratio(2, 1.0) == -1, "tempo deck 2 must fail");
        require(pulse_audio_set_stem_levels(2, 1.0f, 1.0f, 1.0f, 1.0f) == -1, "stems deck 2 must fail");

        const std::vector<AudioEventC> events = drainAll(engine);
        require(events.empty(), "no events for invalid deck id");
        std::printf("  [d] invalid deck id produces no events: OK\n");
    }

    // (e) Overflow: pushing > 512 events without draining drops the excess; no deadlock.
    {
        drainAll(engine);
        const uint32_t beforeDropped = engine.droppedEvents();

        const int pushCount = 700;
        for (int i = 0; i < pushCount; ++i) {
            engine.emitEvent(PULSE_EVT_TRACK_LOADED, 0, i % 100, static_cast<double>(i));
        }

        AudioEventC buf[512];
        uint32_t dropped = 0;
        const int n = pulse_audio_drain_events(buf, 512, &dropped);
        require(n == 512, "first drain returns the 512-slot capacity");
        require(dropped > beforeDropped, "dropped counter advanced");
        require(dropped - beforeDropped == static_cast<uint32_t>(pushCount) - 512, "exact drop count");
        require(pulse_audio_drain_events(buf, 512, &dropped) == 0, "queue empty after drain");

        const AudioEventC last = buf[511];
        require(last.version == 1 && last.kind == PULSE_EVT_TRACK_LOADED, "drained payload intact");
        std::printf("  [e] bounded overflow drop (dropped=%u): OK\n", dropped - beforeDropped);
    }

    // (f) Real-time path: processAudioBlock edge sampling.
    {
        engine.initialize(config);
        drainAll(engine);

        const std::string wavPath = (tempDir / "sine_rt_2s.wav").string();
        createSineWav(wavPath, 440.0, 2.0, 0.1f);
        require(engine.loadTrack(0, wavPath), "rt track must load");
        drainAll(engine);  // TrackLoaded bookkeeping
        require(engine.playDeck(0), "rt deck must play");
        drainAll(engine);  // DeckStateChanged bookkeeping

        std::vector<float> block(512 * 2, 0.0f);
        for (int i = 0; i < 2000; ++i) {
            engine.processAudioBlock(block.data(), 512, 2);
        }

        std::vector<AudioEventC> events = drainAll(engine);
        require(countKind(events, PULSE_EVT_TRACK_ENDED) == 1, "TrackEnded emitted exactly once");
        require(engine.getDeck(0)->getPlaybackState() == pulse::audio::DeckPlaybackState::Ready,
                "deck auto-stopped to Ready");

        // Short transition: one TransitionStarted, one TransitionCompleted.
        drainAll(engine);
        TransitionCommandC cmd{};
        cmd.version = 1;
        cmd.source_deck = 0;
        cmd.destination_deck = 1;
        cmd.duration_seconds = 1.0;
        cmd.transition_type = 0;
        require(engine.executeTransition(cmd) == 0, "valid transition must be accepted");
        drainAll(engine);  // accepted -> no rejection event

        for (int i = 0; i < 200; ++i) {
            engine.processAudioBlock(block.data(), 512, 2);
        }

        events = drainAll(engine);
        require(countKind(events, PULSE_EVT_TRANSITION_STARTED) == 1, "one TransitionStarted");
        require(countKind(events, PULSE_EVT_TRANSITION_COMPLETED) == 1, "one TransitionCompleted");
        require(findFirst(events, PULSE_EVT_TRANSITION_STARTED) < findFirst(events, PULSE_EVT_TRANSITION_COMPLETED),
                "started before completed");
        require(!engine.getTransitionExecutor()->isTransitionActive(), "transition inactive after completion");
        std::printf("  [f] real-time edge events: OK\n");
    }

    engine.shutdown();
    drainAll(engine);

    std::filesystem::remove_all(tempDir);
    std::printf("=== Audio Event Contract Tests PASSED ===\n");
    return 0;
}
