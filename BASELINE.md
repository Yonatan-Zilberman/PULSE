# PULSE — Engineering Baseline Report

**Date:** 2026-08-14  
**Target Platform:** macOS Apple Silicon (`arm64-apple-darwin25.5.0`)  
**Repository State:** Greenfield Multi-Language Scaffolding Complete  
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
  - `src-tauri/src/dj_brain/`: Scaffolding for `set_planner`, `candidate_generator`, `energy_planner`, `recovery`, and `transition_graph`.
  - `src-tauri/src/analysis/`: Scaffolding for `bpm`, `beatgrid`, `key`, `structure`, and `loudness` analyzers.
  - `src-tauri/src/library/`: SQLite table DDL (`CREATE_TRACKS_TABLE`, indexes) and in-memory `LibraryCache` initialization tests.
  - `src-tauri/src/audio_bridge/`: `types.rs` and `ffi.rs` with C-compatible POD types matching C++ structs.

### Tier 3: Real-Time Audio Engine & Phase 0 CLI Prototype (C++20 / Apple Frameworks)
- **Build System:** `src-cpp/CMakeLists.txt` targeting C++20 with `-Wall -Wextra -Wpedantic -Werror` and native Apple frameworks (`CoreAudio`, `AudioToolbox`, `Accelerate`, `CoreFoundation`).
- **Headers & Safety:** `AudioEngine.h`, `DeckPlayer.h`, `Mixer.h`, `TransitionExecutor.h`, `AudioDecoder.h`, `WavWriter.h`, and `AudioBridgeTypes.h` documenting real-time thread safety (zero heap allocations, zero blocking locks in audio callbacks).
- **Native Audio Decoding:** `AudioDecoder.cpp` wrapping `ExtAudioFile` / `AudioToolbox` for hardware-accelerated local ingestion with accurate sample rate, channel count, duration, peak telemetry, and autocorrelation tempo estimation.
- **Audio Output:** `WavWriter.cpp` implementing zero-dependency 16-bit / 48kHz PCM WAV file serialization and synthetic fixture generation.
- **Phase 0 CLI Target:** `pulse_cli` binary executing automated offline transition mixes between dual decks with structured JSON telemetry emission.
- **FFI Boundary:** `AudioBridge.cpp` implementing `extern "C"` ABI functions (`pulse_audio_init`, `pulse_audio_load_track`, `pulse_audio_play_pause`, `pulse_audio_get_deck_state`, `pulse_audio_execute_transition`).
- **Test Suite:**
  - `test_audio_bridge`: C ABI size, alignment, and state mutation tests.
  - `test_audio_decoder`: Audio file decoding, 0-byte resilience, corrupt header detection, and timing accuracy tests.
  - `test_mixer_dsp`: Equal-power crossfader, volume scaling, and peak limiter tests.
  - `test_cli_e2e`: End-to-end transition rendering and JSON artifact emission verification.
- **Deterministic Audio Fixtures (`tests/audio/`):**
  - `fixture_a.wav` (48kHz stereo, 440 Hz + 120 BPM clicks, 10.0s)
  - `fixture_b.wav` (48kHz stereo, 880 Hz + 120 BPM clicks, 10.0s)
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
| **Rust Core Unit Tests** | `cargo test --manifest-path src-tauri/Cargo.toml` | Serde roundtrips, tempo detection, beatgrid alignment, SQLite DDL |
| **C++ Build & CTest Suite** | `cmake -B src-cpp/build -S src-cpp && cmake --build src-cpp/build && ctest --test-dir src-cpp/build --output-on-failure` | C++20 ABI, AudioDecoder, Mixer DSP, and CLI E2E tests |
| **Phase 0 CLI Feasibility Run** | `./src-cpp/build/pulse_cli --deck-a tests/audio/fixture_a.wav --deck-b tests/audio/fixture_b.wav --out tests/audio/test_out.wav --report tests/audio/test_report.json` | End-to-end automated transition mix & JSON telemetry |


---

## 4. Invariants & Zero-Cost Compliance Audit

- **Master Dependency Registry:** Formal single-source-of-truth established in [DEPENDENCIES.md](DEPENDENCIES.md) and [Docs/Pulse Dependency Audit.md](Docs/Pulse%20Dependency%20Audit.md) covering all 9 technical domains.
- **Zero Cloud / Local-First:** All dependencies, crate manifests, and configurations run 100% locally with zero telemetry, zero cloud endpoints, and zero external API requirements. Desktop shell enforces `default-src 'self'` CSP.
- **Licensing Audit:** All adopted dependencies (Tauri, React, Zustand, Rusqlite, SoundTouch LGPL, ONNX Runtime MIT) comply with zero-cost open-source distribution rules. SoundTouch is isolated via dynamic `.dylib` linkage.
- **Distribution Cost Segregation:** Apple Developer Program membership ($99/year) is documented strictly as an optional OS distribution / Gatekeeper notarization cost for signed DMGs. Development and local builds run at **$0 cost**.
- **Real-Time Safety:** Audio callback processing in `src-cpp/` strictly isolates DSP from dynamic memory allocation, file I/O, and ML inference.
