# PULSE — Engineering Baseline Report

**Date:** 2026-08-14  
**Target Platform:** macOS Apple Silicon (`arm64-apple-darwin25.5.0`)  
**Repository State:** Pitch-Preserving Tempo Matching & Bounded Time-Stretch Pipeline Complete  
**Architecture Conformance:** Matches [PRD v4.0](Docs/Pulse%20PRD.md) & [Technical Design v1.0](Docs/Pulse%20Technical%20Design.md)  

---

## 1. Verified Host Toolchain Baseline

| Component | Tool / Runtime | Verified Version | Host Location |
|---|---|---|---|
| **Rust Core** | `rustc` | `1.95.0 (59807616e 2026-04-14)` | Local toolchain |
| **Rust Package Manager** | `cargo` | `1.95.0 (f2d3ce0bd 2026-03-21)` | Local toolchain |
| **Node.js Runtime** | `node` | `v26.0.0` | `/Users/yonatanzilberman/.nvm/...` |
| **Frontend Package Manager** | `pnpm` | `11.3.0` | Local toolchain |
| **Frontend Alternative** | `npm` | `11.3.0` | Local toolchain |
| **C++ Toolchain** | Apple `clang++` | `21.0.0 (clang-2100.1.1.101)` | `/Library/Developer/CommandLineTools/usr/bin` |
| **Architecture** | Target ABI | `arm64` (Apple Silicon M1–M5) | POSIX / Mach-O |

---

## 2. Multi-Language Tier Scaffolding Summary

### Tier 1: Frontend (React 18 + TypeScript + Zustand + Vite)
- **Root Files:** `package.json`, `tsconfig.json`, `tsconfig.node.json`, `vite.config.ts`, `eslint.config.js`, `.prettierrc`, `index.html`.
- **UI Architecture:** `src/components/` with isolated components for `AutoDJ`, `Waveform`, `Queue`, `TransitionPreview`, `EnergyCurve`, and `Mixer`.
- **State Layer:** `src/state/useAppStore.ts` implemented via Zustand, managing dual-deck telemetry, DJ Brain queue, active transition candidates, and energy curve targets.
- **Unit Tests:** `src/App.test.tsx` and `src/state/useAppStore.test.ts` runnable deterministically via Vitest.

### Tier 2: Application Core (Rust & Tauri 2)
- **Crate Root:** `src-tauri/Cargo.toml` configured with `tauri 2.1`, `serde`, `serde_json`, `rusqlite 0.32`, `tokio 1.42`, `thiserror 2.0`, and strict Clippy rules.
- **Domain Modules:**
  - `src-tauri/src/models/`: Strongly typed schemas for `TrackProfile`, `TransitionCandidate`, `TransitionPlan`, `SetPlan`, and `CandidateScore` with serialization unit tests.
  - `src-tauri/src/dj_brain/`: `candidate_generator` implementing bounded tempo compatibility scoring ($\text{BPM\_Diff\_norm}$ formulas, octave hypothesis resolution, artifact risk penalties), `set_planner`, `energy_planner`, `recovery`, and `transition_graph`.
  - `src-tauri/src/analysis/`: Scaffolding for `bpm`, `beatgrid`, `key`, `structure`, and `loudness` analyzers.
  - `src-tauri/src/library/`: SQLite table DDL (`CREATE_TRACKS_TABLE`, indexes) and in-memory `LibraryCache` initialization tests.
  - `src-tauri/src/audio_bridge/`: `types.rs` and `ffi.rs` with C-compatible POD types matching C++ structs.

### Tier 3: Real-Time Audio Engine & Phase 0 CLI Prototype (C++20 / Apple Frameworks)
- **Build System:** `src-cpp/CMakeLists.txt` targeting C++20 with `-Wall -Wextra -Wpedantic -Werror` and native Apple frameworks (`CoreAudio`, `AudioToolbox`, `Accelerate`, `CoreFoundation`).
- **Headers & Safety:** `AudioEngine.h`, `DeckPlayer.h`, `Mixer.h`, `TransitionExecutor.h`, `AudioDecoder.h`, `WavWriter.h`, `TimeStretchEngine.h`, `TempoStrategy.h`, and `AudioBridgeTypes.h` documenting real-time thread safety (zero heap allocations, zero blocking locks in audio callbacks).
- **Pitch-Preserving Time-Stretching:** `TimeStretchEngine.cpp` implementing real-time safe, pitch-invariant WSOLA time-stretching (0.0 semitone pitch shift) with SoundTouch dynamic linkage isolation.
- **Bounded Tempo Strategy:** `TempoStrategy.cpp` implementing 4-tier decision policies: Small ($\le \pm 3\%$), Moderate ($\pm 3\%$ to $\pm 6\%$), Octave-Compatible ($0.5\times/2.0\times$), and Excessive ($> \pm 6\%$ rejected/penalized).
- **Native Audio Decoding & DSP Beat Tracking:** `AudioDecoder.cpp` wrapping `ExtAudioFile` / `AudioToolbox` for hardware-accelerated local ingestion with sample rate, channels, peak telemetry, RMS energy novelty extraction, autocorrelation tempo estimation, candidate hypotheses resolution (0.5x, 2.0x, secondary peaks), phase offset optimization, 4/4 downbeat placement, and sliding-window tempo drift detection.
- **Audio Output & Synthetic Generators:** `WavWriter.cpp` implementing zero-dependency 16-bit / 48kHz PCM WAV file serialization and synthetic fixture generators for steady, ambiguous, drifting, syncopated, and tempo-test audio cases.
- **Phase 0 CLI Target:** `pulse_cli` binary executing automated offline transition mixes between dual decks with musical beat and downbeat phase alignment (`--align-beats`, `--snap-to-bar`), tempo matching policies (`--tempo-strategy`, `--target-bpm`, `--max-stretch-pct`, `--force-stretch`), and structured `tempo_matching` JSON telemetry emission.
- **FFI Boundary:** `AudioBridge.cpp` implementing `extern "C"` ABI functions (`pulse_audio_init`, `pulse_audio_load_track`, `pulse_audio_play_pause`, `pulse_audio_get_deck_state`, `pulse_audio_execute_transition`).
- **Test Suite (6 CTest Targets):**
  - `test_audio_bridge` (`AudioBridgeSmokeTest`): C ABI size, alignment, and state mutation tests.
  - `test_audio_decoder` (`AudioDecoderTest`): Steady 120/128 BPM, ambiguous 70/140 BPM, drifting tempo, syncopated rhythm with silence intro, and corrupt/empty file error tests.
  - `test_mixer_dsp` (`MixerDSPTest`): Equal-power crossfader, volume scaling, and peak limiter tests.
  - `test_cli_e2e` (`CliEndToEndTest`): End-to-end beat-aligned transition rendering and JSON artifact emission verification.
  - `test_time_stretch` (`TimeStretchTest`): Pitch invariance (0.0 semitones delta), duration scaling, transient preservation ($\ge 85\%$), octave resolution, excessive stretch rejection, and phase-aligned mixing tests.
  - `test_cli_tempo_match_e2e` (`CliTempoMatchEndToEndTest`): E2E tempo-matched transition mix verification, non-clipping WAV rendering, and JSON report validation.
- **Deterministic Audio Fixtures (`tests/audio/` & `tests/golden-set/`):**
  - `fixture_a.wav` (48kHz stereo, 440 Hz + 120 BPM clicks, 10.0s)
  - `fixture_b.wav` (48kHz stereo, 880 Hz + 120 BPM clicks, 10.0s)
  - `fixture_steady_128bpm.wav` (48kHz stereo, 523.25 Hz + 128 BPM clicks, 10.0s)
  - `fixture_ambiguous_70_140bpm.wav` (48kHz stereo, 70/140 BPM syncopated pulses, 10.0s)
  - `fixture_drifting_115_125bpm.wav` (48kHz stereo, 115 -> 125 BPM accelerating tempo, 10.0s)
  - `fixture_syncopated_difficult.wav` (48kHz stereo, 2.0s silence intro + swing rhythm, 10.0s)
  - `fixture_tempo_small_122bpm.wav` (48kHz stereo, 440 Hz + 122 BPM clicks, 10.0s)
  - `fixture_tempo_moderate_126bpm.wav` (48kHz stereo, 440 Hz + 126 BPM clicks, 10.0s)
  - `fixture_tempo_octave_140bpm.wav` (48kHz stereo, 440 Hz + 140 BPM clicks, 10.0s)
  - `fixture_tempo_excessive_150bpm.wav` (48kHz stereo, 440 Hz + 150 BPM clicks, 10.0s)
  - `sine_440hz_120bpm_48k.wav` & `sine_880hz_120bpm_48k.wav`
  - `invalid_empty.wav` (0-byte error test)
  - `corrupt_header.wav` (corrupt header error test)

---

## 3. Verification Commands & Quality Gates

| Quality Gate | Command | Scope |
|---|---|---|
| **Licensing & Dependency Audit** | `pnpm audit:licenses` (or `./scripts/audit_dependencies.sh`) | Validates all manifests against SPDX whitelist & zero-cloud invariants |
| **Frontend Linting** | `pnpm lint` | ESLint across TypeScript & JSX |
| **Frontend Typecheck & Build** | `pnpm build` | `tsc` compilation & Vite bundling |
| **Frontend Unit Tests** | `pnpm test` | Vitest React & Zustand store tests |
| **Rust Formatting** | `cargo fmt --manifest-path src-tauri/Cargo.toml -- --check` | Rustfmt style adherence |
| **Rust Linting** | `cargo clippy --manifest-path src-tauri/Cargo.toml -- -D warnings` | Zero clippy warnings |
| **Rust Core Unit Tests** | `cargo test --manifest-path src-tauri/Cargo.toml` | Serde roundtrips, tempo detection, beatgrid alignment, candidate scoring, SQLite DDL |
| **C++ Build & CTest Suite** | `cmake -B src-cpp/build -S src-cpp && cmake --build src-cpp/build && ctest --test-dir src-cpp/build --output-on-failure` | 6 CTest suites (ABI, AudioDecoder, Mixer DSP, CLI E2E, TimeStretch, CLI Tempo E2E) |
| **Phase 0 CLI Feasibility Run** | `./src-cpp/build/pulse_cli --deck-a tests/audio/fixture_a.wav --deck-b tests/audio/fixture_steady_128bpm.wav --out tests/audio/test_out.wav --report tests/audio/test_report.json --tempo-strategy source` | End-to-end automated transition mix & JSON telemetry |

---

## 4. Invariants & Zero-Cost Compliance Audit

- **Master Dependency Registry:** Formal single-source-of-truth established in [DEPENDENCIES.md](DEPENDENCIES.md) and [Docs/Pulse Dependency Audit.md](Docs/Pulse%20Dependency%20Audit.md) covering all 9 technical domains.
- **Zero Cloud / Local-First:** All dependencies, crate manifests, and configurations run 100% locally with zero telemetry, zero cloud endpoints, and zero external API requirements. Desktop shell enforces `default-src 'self'` CSP.
- **Licensing Audit:** All adopted dependencies (Tauri, React, Zustand, Rusqlite, SoundTouch LGPL, ONNX Runtime MIT) comply with zero-cost open-source distribution rules. SoundTouch is isolated via dynamic `.dylib` linkage.
- **Distribution Cost Segregation:** Apple Developer Program membership ($99/year) is documented strictly as an optional OS distribution / Gatekeeper notarization cost for signed DMGs. Development and local builds run at **$0 cost**.
- **Real-Time Safety:** Audio callback processing in `src-cpp/` strictly isolates DSP from dynamic memory allocation, file I/O, and ML inference.
