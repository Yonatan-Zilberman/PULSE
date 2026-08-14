# PULSE — Engineering Baseline Report

**Date:** 2026-08-14  
**Target Platform:** macOS Apple Silicon (`arm64-apple-darwin25.5.0`)  
**Repository State:** Phase 0 Feasibility Gate Automated Multi-Track Mixing Engine & 20-Transition Blind Evaluation Pipeline Complete  
**Architecture Conformance:** Matches [PRD v4.0](Docs/Pulse%20PRD.md) (Section 18 Phase 0 Gate & Section 12.5) & [Technical Design v1.0](Docs/Pulse%20Technical%20Design.md) (Section 9)  

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
  - `src-tauri/src/dj_brain/`:
    - `candidate_generator`: Bounded tempo compatibility scoring ($\text{BPM\_Diff\_norm}$ formulas, octave hypothesis resolution, artifact risk penalties), dynamic phrase scoring (`phrase_score`) based on structural phrase boundary alignment, phrase-aligned duration scaling (8 bars / 16 bars at target BPM), default recommendation of `TransitionType::BassSwap` for tempo differences $\le 6\%$.
    - `set_planner`: Multi-track greedy set sequencing over track pools ($\ge 25$ tracks), evaluating Camelot harmonic wheel distance (adjacent $\pm 1$, relative major/minor, dominant/subdominant), energy progression curve matching, candidate ranking, and constructing full $\ge 20$-transition `SetPlan` sequences.
    - `energy_planner`, `recovery`, and `transition_graph`.
  - `src-tauri/src/analysis/`: Scaffolding for `bpm`, `beatgrid`, `key`, `structure`, and `loudness` analyzers.
  - `src-tauri/src/library/`: SQLite table DDL (`CREATE_TRACKS_TABLE`, indexes) and in-memory `LibraryCache` initialization tests.
  - `src-tauri/src/audio_bridge/`: `types.rs` and `ffi.rs` with C-compatible POD types matching C++ structs.

### Tier 3: Real-Time Audio Engine & Phase 0 Multi-Track Set Mixer (C++20 / Apple Frameworks)
- **Build System:** `src-cpp/CMakeLists.txt` targeting C++20 with `-Wall -Wextra -Wpedantic -Werror` and native Apple frameworks (`CoreAudio`, `AudioToolbox`, `Accelerate`, `CoreFoundation`).
- **Headers & Safety:** `AudioEngine.h`, `DeckPlayer.h`, `Mixer.h`, `TransitionPlanner.h`, `TransitionExecutor.h`, `AudioDecoder.h`, `WavWriter.h`, `TimeStretchEngine.h`, `TempoStrategy.h`, and `AudioBridgeTypes.h` documenting real-time thread safety (zero heap allocations, zero blocking locks in audio callbacks).
- **Multi-Track Ping-Pong Audio Loop:** `pulse_cli.cpp` executing continuous multi-track sets across 20+ tracks via dual-deck ping-pong cycling (Deck A $\leftrightarrow$ Deck B), pre-loading incoming tracks into idle decks during playback, automated smoothstep bass swapping, and exporting isolated transition audio snippets (`tests/audio/transitions/transition_XX.wav`).
- **3-Band LR4 EQ DSP Engine:** `DeckPlayer.cpp` implementing 4th-order Linkwitz-Riley crossover filters ($24\text{ dB/octave}$) splitting audio at $f_L = 250\text{ Hz}$ (Low/Mid) and $f_H = 3500\text{ Hz}$ (Mid/High) with 2nd-order allpass phase compensation, achieving flat frequency response ($0\text{ dB}$ across spectrum) and zero phase cancellation at unity EQ bypass.
- **Deterministic Bass Swap Automation:** `TransitionExecutor.cpp` implementing `BassSwapStrategy` executing a sequenced low-frequency handoff where outgoing deck low-EQ drops smoothly $0.0 \rightarrow -1.0$ while incoming deck low-EQ rises $-1.0 \rightarrow 0.0$ via smoothstep interpolation centered at a configurable swap point ($p = 0.50$).
- **Objective Gain Safety:** Energy conservation guarantees that low-frequency sum remains strictly bounded ($\le +0.5\text{ dB}$ vs $> +2.5\text{ dB}$ swelling in naive crossfades), eliminating low-end buildup, phase mud, and limiter clipping.
- **Musical Phrase Boundary Analysis:** `AudioDecoder.cpp` extracting deterministic 4-bar, 8-bar, 16-bar, and 32-bar boundary timestamps and calculating `phraseConfidence` from beatgrid regularity and track bar count.
- **Transition Window Planner:** `TransitionPlanner.cpp` evaluating 4/8/16/32-bar transition windows, calculating outro exit boundaries for outgoing decks and intro entry downbeats for incoming decks, exact phrase duration scaling, and fallback handling.
- **Pitch-Preserving Time-Stretching:** `TimeStretchEngine.cpp` implementing real-time safe, pitch-invariant WSOLA time-stretching (0.0 semitone pitch shift) with SoundTouch dynamic linkage isolation.
- **Bounded Tempo Strategy:** `TempoStrategy.cpp` implementing 4-tier decision policies: Small ($\le \pm 3\%$), Moderate ($\pm 3\%$ to $\pm 6\%$), Octave-Compatible ($0.5\times/2.0\times$), and Excessive ($> \pm 6\%$ rejected/penalized).
- **Golden Fixture Corpus (25 Tracks):** `generate_fixtures.cpp` generating 25 distinct golden tracks (`golden_track_01.wav` .. `golden_track_25.wav`) in `tests/golden-set/` spanning 118–130 BPM, diverse Camelot keys, 8-bar musical phrase pulses, and $60\text{ Hz}$ sub-bass foundations.
- **Test Suite (14 CTest Targets):**
  - `test_audio_bridge` (`AudioBridgeSmokeTest`): C ABI size, alignment, and state mutation tests.
  - `test_audio_decoder` (`AudioDecoderTest`): Steady 120/128 BPM, ambiguous 70/140 BPM, drifting tempo, syncopated rhythm with silence intro, and corrupt/empty file error tests.
  - `test_mixer_dsp` (`MixerDSPTest`): Equal-power crossfader, volume scaling, and peak limiter tests.
  - `test_cli_e2e` (`CliEndToEndTest`): End-to-end beat-aligned transition rendering and JSON artifact emission verification.
  - `test_time_stretch` (`TimeStretchTest`): Pitch invariance (0.0 semitones delta), duration scaling, transient preservation ($\ge 85\%$), octave resolution, excessive stretch rejection, and phase-aligned mixing tests.
  - `test_cli_tempo_match_e2e` (`CliTempoMatchEndToEndTest`): E2E tempo-matched transition mix verification, non-clipping WAV rendering, and JSON report validation.
  - `test_phrase_analysis` (`PhraseAnalysisTest`): Phrase boundary timestamp extraction across 120 BPM, 128 BPM, short audio, and empty/corrupt files.
  - `test_transition_planner` (`TransitionPlannerTest`): 8-bar phrase window selection, stepdown fallback, low-confidence downbeat fallback, and tempo scaling.
  - `test_cli_phrase_transition_e2e` (`CliPhraseTransitionEndToEndTest`): E2E phrase-aware mix rendering, real-time safety, non-clipping WAV validation, and JSON report validation.
  - `test_eq_dsp` (`EqDSPTest`): 3-band LR4 crossover filter tests for unity bypass ($\Delta < 0.015$), low kill ($\ge 24\text{ dB}$ at $60\text{ Hz}$), high kill ($\ge 24\text{ dB}$ at $10\text{ kHz}$), mid isolation passband ($1\text{ kHz}$), and EQ state reset.
  - `test_bass_swap_safety` (`BassSwapSafetyTest`): Sub-bass collision test ($80\text{ Hz}$ at $0.85$ peak), verifying gain boundedness $\le +0.5\text{ dB}$ over single-deck level, zero clipping, and headroom preservation vs naive crossfade.
  - `test_cli_bass_swap_e2e` (`CliBassSwapEndToEndTest`): E2E CLI integration test rendering bass-heavy fixtures with bass swap transition, validating output WAV and JSON report schema (`"strategy": "bass_swap"`).
  - `test_set_mixer_e2e` (`SetMixerEndToEndTest`): End-to-end 20-transition multi-track set rendering test across 21 golden tracks, verifying non-clipping peak amplitude $< 1.0$, 20 transition objects in JSON report with bass swap strategy, and existence of all 20 snippet WAVs.
  - `test_cli_set_flags_e2e` (`CliSetFlagsEndToEndTest`): E2E verification of CLI arguments (`--playlist`, `--track-dir`, `--tracks`, `--auto-sequence`, `--export-snippets`), multi-track mix rendering, and error handling.

---

## 3. Blind Human-Rating Evaluation Harness (`tests/evaluation/`)

- **Rating Rubric (`RATING_RUBRIC.md`):** Formal 5-point Likert scale evaluation protocol across 5 objective dimensions (Beat & Tempo Coherence, Low-End / Bass Management, Phrase & Structural Placement, Energy Continuity, and Overall Human Convincingness) with Phase 0 Feasibility Gate pass criteria ($\ge 4.0 / 5.0$ mean, zero catastrophic failures).
- **Standalone HTML5 Rating Tool (`blind_rating_tool.html`):** Zero-cloud, 100% offline local audio evaluation player featuring blinded track metadata, snippet playback with interactive waveform scrubbing, 5-star scoring, real-time aggregate statistics, and `evaluation_results.json` exporter.
- **Evaluation Template (`evaluation_template.json`):** Structured evaluation template recording ratings and comments for all 20 transitions.

---

## 4. Verification Commands & Quality Gates

| Quality Gate | Command | Scope |
|---|---|---|
| **Licensing & Dependency Audit** | `pnpm audit:licenses` | Validates all manifests against SPDX whitelist & zero-cloud invariants |
| **Frontend Linting** | `pnpm lint` | ESLint across TypeScript & JSX |
| **Frontend Typecheck & Build** | `pnpm build` | `tsc` compilation & Vite bundling |
| **Frontend Unit Tests** | `pnpm test` | Vitest React & Zustand store tests |
| **Rust Formatting** | `cargo fmt --manifest-path src-tauri/Cargo.toml -- --check` | Rustfmt style adherence |
| **Rust Linting** | `cargo clippy --manifest-path src-tauri/Cargo.toml -- -D warnings` | Zero clippy warnings |
| **Rust Core Unit Tests** | `cargo test --manifest-path src-tauri/Cargo.toml` | 23 unit tests (SetPlanner sequencing over 25 tracks, Camelot harmonic distance, Serde roundtrips, Candidate scoring, SQLite DDL) |
| **C++ Build & CTest Suite** | `cmake -B src-cpp/build -S src-cpp && cmake --build src-cpp/build && ctest --test-dir src-cpp/build --output-on-failure` | 14 CTest suites (including SetMixerEndToEndTest and CliSetFlagsEndToEndTest) |
| **Phase 0 Golden Set Mix (20 Transitions)** | `./src-cpp/build/pulse_cli --track-dir tests/golden-set --min-transitions 20 --transition-strategy bass_swap --phrase-aware --phrase-bars 8 --tempo-strategy source --export-snippets --out tests/audio/phase0_master_set.wav --report tests/audio/phase0_set_report.json` | 20-transition automated continuous set mix, master WAV, 20 snippet WAVs, and JSON report |


---

## 4. Invariants & Zero-Cost Compliance Audit

- **Master Dependency Registry:** Formal single-source-of-truth established in [DEPENDENCIES.md](DEPENDENCIES.md) and [Docs/Pulse Dependency Audit.md](Docs/Pulse%20Dependency%20Audit.md) covering all 9 technical domains.
- **Zero Cloud / Local-First:** All dependencies, crate manifests, and configurations run 100% locally with zero telemetry, zero cloud endpoints, and zero external API requirements. Desktop shell enforces `default-src 'self'` CSP.
- **Licensing Audit:** All adopted dependencies (Tauri, React, Zustand, Rusqlite, SoundTouch LGPL, ONNX Runtime MIT) comply with zero-cost open-source distribution rules. SoundTouch is isolated via dynamic `.dylib` linkage.
- **Distribution Cost Segregation:** Apple Developer Program membership ($99/year) is documented strictly as an optional OS distribution / Gatekeeper notarization cost for signed DMGs. Development and local builds run at **$0 cost**.
- **Real-Time Safety:** Audio callback processing in `src-cpp/` strictly isolates DSP from dynamic memory allocation, file I/O, and ML inference. All 3-band LR4 crossover filters, biquad states, transition strategies, and audio buffers are pre-allocated at initialization.
