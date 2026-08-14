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

### Tier 3: Real-Time Audio Engine (C++20 / JUCE 8 Harness)
- **Build System:** `src-cpp/CMakeLists.txt` targeting C++20 with `-Wall -Wextra -Wpedantic -Werror` and Apple frameworks (`CoreAudio`, `AudioToolbox`, `Accelerate`).
- **Headers & Safety:** `AudioEngine.h`, `DeckPlayer.h`, `Mixer.h`, `TransitionExecutor.h`, and `AudioBridgeTypes.h` documenting real-time thread safety (zero heap allocations, zero blocking locks).
- **FFI Boundary:** `AudioBridge.cpp` implementing `extern "C"` ABI functions (`pulse_audio_init`, `pulse_audio_load_track`, `pulse_audio_play_pause`, `pulse_audio_get_deck_state`, `pulse_audio_execute_transition`).
- **Test Harness:** `src-cpp/tests/test_audio_bridge.cpp` verifying struct sizes, alignment, engine lifecycle, and state mutation.

---

## 3. Verification Commands & Quality Gates

| Quality Gate | Command | Scope |
|---|---|---|
| **Frontend Linting** | `pnpm lint` | ESLint across TypeScript & JSX |
| **Frontend Typecheck & Build** | `pnpm build` | `tsc` compilation & Vite bundling |
| **Frontend Unit Tests** | `pnpm test` | Vitest React & Zustand store tests |
| **Rust Formatting** | `cargo fmt --manifest-path src-tauri/Cargo.toml -- --check` | Rustfmt style adherence |
| **Rust Linting** | `cargo clippy --manifest-path src-tauri/Cargo.toml -- -D warnings` | Zero clippy warnings |
| **Rust Core Unit Tests** | `cargo test --manifest-path src-tauri/Cargo.toml` | Serde roundtrips, score validations, SQLite DDL |
| **C++ Build & Smoke Test** | `clang++ -std=c++20 -Isrc-cpp/include src-cpp/src/*.cpp src-cpp/tests/test_audio_bridge.cpp -o test_harness && ./test_harness && rm test_harness` | C++20 ABI & state machine assertions |

---

## 4. Invariants & Zero-Cost Compliance Audit

- **Zero Cloud / Local-First:** All dependencies, crate manifests, and configurations run 100% locally with no telemetry, cloud endpoints, or external API requirements.
- **Licensing Audit:** All adopted dependencies (Tauri, React, Zustand, Rusqlite, SoundTouch LGPL, ONNX Runtime MIT) comply with zero-cost open-source distribution rules.
- **Real-Time Safety:** Audio callback processing in `src-cpp/` strictly isolates DSP from dynamic memory allocation, file I/O, and ML inference.
