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

### Tier 3: Real-Time Audio Engine & Phase 1 Production CoreAudio Engine (C++20 / JUCE 8 / Apple Frameworks)
- **Build System:** `src-cpp/CMakeLists.txt` targeting C++20 with `-Wall -Wextra -Wpedantic -Werror` and native Apple frameworks (`CoreAudio`, `AudioToolbox`, `Accelerate`, `CoreFoundation`).
- **CoreAudio Hardware Lifecycle:** `AudioEngine.cpp` implementing deterministic lifecycle (`initialize` $\rightarrow$ `start` $\rightarrow$ `process` $\rightarrow$ `stop` $\rightarrow$ `shutdown`), CoreAudio AudioUnit (`kAudioUnitType_Output`, `kAudioUnitSubType_DefaultOutput`) stream acquisition, sample rate negotiation (44.1k/48k/96k), buffer sizing (64–2048 samples), real-time IOProc callback dispatching to `juce::AudioIODeviceCallback::audioDeviceIOCallbackWithContext`, and clean teardown.
- **Strict Real-Time Safety:** Verified 0 dynamic heap allocations, 0 blocking mutexes, 0 logging statements, and 0 disk I/O across the real-time audio thread callback path (`processAudioBlock`, `audioDeviceIOCallbackWithContext`).
- **Headers & Safety:** `AudioEngine.h`, `DeckPlayer.h`, `Mixer.h`, `TransitionPlanner.h`, `TransitionExecutor.h`, `AudioDecoder.h`, `WavWriter.h`, `TimeStretchEngine.h`, `TempoStrategy.h`, and `AudioBridgeTypes.h` documenting real-time thread safety.
- **Multi-Track Ping-Pong Audio Loop:** `pulse_cli.cpp` executing continuous multi-track sets across 20+ tracks via dual-deck ping-pong cycling (Deck A $\leftrightarrow$ Deck B), pre-loading incoming tracks into idle decks during playback, automated smoothstep bass swapping, and exporting isolated transition audio snippets (`tests/audio/transitions/transition_XX.wav`).
- **Engine-Level Tempo Matching:** `AudioEngine::matchTempo(...)` computes a bounded `TempoStrategyDecision` from both decks' detected BPMs (via `TempoStrategy::evaluate`) and applies matched per-deck tempo ratios through the existing control-plane setters, promoting tempo matching from CLI-only orchestration (`pulse_cli.cpp`) into the JUCE engine. Control-plane only — never invoked on the real-time audio thread — and verified by `test_engine_tempo_match` (Source match, octave match, >6% stretch rejection, and RT-path tempo scaling with no clipping).
- **3-Band LR4 EQ DSP Engine:** `DeckPlayer.cpp` implementing 4th-order Linkwitz-Riley crossover filters ($24\text{ dB/octave}$) splitting audio at $f_L = 250\text{ Hz}$ (Low/Mid) and $f_H = 3500\text{ Hz}$ (Mid/High) with 2nd-order allpass phase compensation, achieving flat frequency response ($0\text{ dB}$ across spectrum) and zero phase cancellation at unity EQ bypass.
- **Deterministic Bass Swap Automation:** `TransitionExecutor.cpp` implementing `BassSwapStrategy` executing a sequenced low-frequency handoff where outgoing deck low-EQ drops smoothly $0.0 \rightarrow -1.0$ while incoming deck low-EQ rises $-1.0 \rightarrow 0.0$ via smoothstep interpolation centered at a configurable swap point ($p = 0.50$).
- **Objective Gain Safety:** Energy conservation guarantees that low-frequency sum remains strictly bounded ($\le +0.5\text{ dB}$ vs $> +2.5\text{ dB}$ swelling in naive crossfades), eliminating low-end buildup, phase mud, and limiter clipping.
- **Pitch-Preserving Time-Stretching:** `TimeStretchEngine.cpp` implementing real-time safe, pitch-invariant WSOLA time-stretching (0.0 semitone pitch shift) with SoundTouch dynamic linkage isolation.
- **Golden Fixture Corpus (25 Tracks):** `generate_fixtures.cpp` generating 25 distinct golden tracks (`golden_track_01.wav` .. `golden_track_25.wav`) in `tests/golden-set/` spanning 118–130 BPM, diverse Camelot keys, 8-bar musical phrase pulses, and $60\text{ Hz}$ sub-bass foundations.
- **Production Transition Executor:** `TransitionExecutor.cpp` executing a fully precomputed, versioned (v2) `TransitionCommandC` plan over the C ABI — parameter sanitization (non-finite → safe defaults, bounds clamp, monotonic phases, structural rejection), the Tech Design §9.1 **Classic EQ Blend** reference transition (silent sync → fader + bass handoff + gain staging → vocal stem handoff → cut + tempo return ramp), seqlock lock-free handoff driven from the engine's real-time block paths, and baseline restore on completion.
- **Test Suite (26 CTest Targets):**
  - `test_audio_bridge` (`AudioBridgeSmokeTest`): C ABI size, alignment, lifecycle, and telemetry tests.
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
  - `test_audio_engine_lifecycle` (`AudioEngineLifecycleTest`): Comprehensive state machine transitions, invalid config rejection, re-initialization under load, and active playback teardown.
  - `test_realtime_safety` (`RealtimeSafetyTest`): Zero heap allocations assertions across 1,000 blocks of simulated and live DSP callbacks.
  - `test_transition_executor` (`TransitionExecutorTest`): Transition executor sanitization matrix (NaN/Inf/out-of-bounds per v2 field), malformed-plan rejection, Classic EQ Blend bit-determinism across engine re-initialization, zero-allocation real-time contract across 1,500 live-callback blocks (no seek/reload/stop), engine-driven live advance, and C FFI accept/reject.
  - `test_production_dsp` (`ProductionDspTest`): Objective real-time DSP tests for the production signal chain — unity bypass, gain bounds, parameter smoothing (no zipper noise), stem-mixer fallback, safe master soft-limiter clipping prevention ($\le 0.999$), and dynamic tempo-adjustment duration scaling.
  - `test_engine_tempo_match` (`EngineTempoMatchTest`): Engine-level tempo matching via `AudioEngine::matchTempo` — Source match ratios (120/125 BPM $\to$ 0.96), octave match (120/60 BPM $\to$ ratio 1.0/1.0), >6% stretch rejection flag, and RT-path tempo scaling through `processAudioBlock` with no master clipping.
  - `test_coreaudio_live` (`CoreAudioLiveTest`): Live hardware callback verification streaming audio to default macOS CoreAudio output with frame progress and zero underruns.
  - `pulse_dsp_comparator` (`DspComparatorSelfTest`): Built-in render-comparison self-test for the DSP regression tool (SNR / peak / clipping metrics against a synthetic reference pair).
  - `test_stress_dsp` (`StressDspTest`): Offline DSP stress matrix — see Three-Tier Audio Quality Harness below.
  - `test_golden_regression` (`GoldenRegressionTest`): Golden-set regression against 5 committed float32 reference renders — see below.
  - `test_soak` (`SoakShortTest`): Continuous 25-track ping-pong soak, 5-minute CI tier — see below.

### Three-Tier Audio Quality Harness (Stress / Golden Regression / Soak)

Three new CTest-registered, fully deterministic, offline harnesses (no CoreAudio render thread, no `start()`, no hardware) per the PRD reliability gate (12-hour soak, zero dropouts):

- **Tier 1 — Stress (`StressDspTest`, `src-cpp/tests/test_stress_dsp.cpp`):** 12-cell matrix of block size ($64/256/512/2048$) $\times$ sample rate ($44.1k/48k/96k$), 5 s of audio per cell, with deterministic LCG-scheduled control-plane churn every 16 blocks (volume/EQ/filter/tempo/seek/play-pause/`executeTransition`/deck-swap reload) and both decks on the pitch-preserving WSOLA path ($1.04\times/0.96\times$). Gating invariants per cell: exact frame accounting, zero underruns, zero event drops, zero heap allocations inside `processAudioBlock` windows (allocation trap), master peak $\le 0.999f$ (engine limiter ceiling). The $\ge10\times$ CPU-headroom bar (mean + p95 block time $\le 10\%$ of block budget) is measured and reported per cell; it is advisory until the engine's per-block fixed cost is optimized (see Follow-up ticket 1 below — the unoptimized CTest build cannot meet it).
- **Tier 2 — Golden Regression (`GoldenRegressionTest`, `src-cpp/tests/test_golden_regression.cpp`):** Five pinned deterministic scenarios (Classic EQ Blend, SCurve + preset deck EQs, Bass Swap, tempo-matched Bass Swap, 24 s 3-track set) rendered fresh and compared against committed float32 reference WAVs under `tests/golden-set/references/` (5 files, $\approx 22$ MB). Bars per profile: $\mathrm{SNR} \ge 80$ dB, max abs sample delta $\le 5\times10^{-3}$, zero clipped samples, exact frame counts. Regenerate references from a known-good build with `./src-cpp/build/test_golden_regression --generate tests/golden-set/references`; never loosen the thresholds (regenerate on a CI-class toolchain instead if CI drifts).
- **Tier 3 — Soak (`SoakShortTest`, `src-cpp/tests/test_soak.cpp`):** Continuous 25-track ping-pong reusing the `pulse_cli` loop structure (preload $\to$ render to mix trigger $\to$ `matchTempo` $\to$ alternating Bass Swap / Classic EQ Blend $\to$ fader handoff, stop + reload), rendering offline until simulated audio time reaches `--duration`. CI tier: 5 minutes (`--duration 300`, `TIMEOUT 1800`). PRD 12-hour local tier (local-only, not scheduled in CI):

  ```bash
  ./src-cpp/build/test_soak --duration 43200 --report tests/audio/soak_12h_report.json
  ```

  Pass bars (both tiers): zero underruns, zero event drops, exact frame ledger, master peak $\le 0.999f$, zero RT-window allocations, every queue track used $\ge 1\times$ when a full pass fits the duration (short sanity runs require $\ge 5$), both decks advanced, final deck states $\in\{$Ready, Paused, Playing$\}$, zero non-finite (NaN/Inf) output samples. Transitions chain back-to-back by design (zero solo per track) to maximize control-plane churn; steady-state coverage comes from Tiers 1–2. A JSON summary (including `cpu_estimate_pct` and `non_finite_samples`) is written to `--report` on every run.

### Follow-up tickets (quality harness)

Open follow-ups surfaced while landing the harness; the advisory statuses above are temporary until these land:

1. **Engine CPU cost (unblocks the CPU-headroom hard bar, plan Risk F2):** reduce the ~50 µs fixed per-block cost and vectorize the WSOLA stretch path so the 64-block cells meet the 10% mean + p95 bar at `-O2` (measured locally: ~50 µs fixed per-block cost dominates small-block budgets). Until then the bar remains advisory in `test_stress_dsp`.
2. **Cross-toolchain golden parity:** if the first CI `GoldenRegressionTest` run drifts, regenerate references on a CI-class toolchain — never loosen the 80 dB bar.

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
| **Rust Core Unit Tests** | `cargo test --manifest-path src-tauri/Cargo.toml` | 31 unit tests (SetPlanner sequencing over 25 tracks, Camelot harmonic distance, Serde roundtrips, Candidate scoring, SQLite DDL, v2 C-ABI layout, TransitionPlan sanitization) |
| **C++ Build & CTest Suite** | `cmake -B src-cpp/build -S src-cpp && cmake --build src-cpp/build && ctest --test-dir src-cpp/build --output-on-failure` | 26 CTest suites (including StressDspTest, GoldenRegressionTest, SoakShortTest, TransitionExecutorTest, CoreAudioLiveTest) |
| **Live CoreAudio Probe** | `./src-cpp/build/test_coreaudio_live` | Validates real-time audio output callback execution on macOS hardware |
| **Phase 0 Golden Set Mix (20 Transitions)** | `./src-cpp/build/pulse_cli --track-dir tests/golden-set --min-transitions 20 --transition-strategy bass_swap --phrase-aware --phrase-bars 8 --tempo-strategy source --export-snippets --out tests/audio/phase0_master_set.wav --report tests/audio/phase0_set_report.json` | 20-transition automated continuous set mix, master WAV, 20 snippet WAVs, and JSON report |



---

## 4. Invariants & Zero-Cost Compliance Audit

- **Master Dependency Registry:** Formal single-source-of-truth established in [DEPENDENCIES.md](DEPENDENCIES.md) and [Docs/Pulse Dependency Audit.md](Docs/Pulse%20Dependency%20Audit.md) covering all 9 technical domains.
- **Zero Cloud / Local-First:** All dependencies, crate manifests, and configurations run 100% locally with zero telemetry, zero cloud endpoints, and zero external API requirements. Desktop shell enforces `default-src 'self'` CSP.
- **Licensing Audit:** All adopted dependencies (Tauri, React, Zustand, Rusqlite, SoundTouch LGPL, ONNX Runtime MIT) comply with zero-cost open-source distribution rules. SoundTouch is isolated via dynamic `.dylib` linkage.
- **Distribution Cost Segregation:** Apple Developer Program membership ($99/year) is documented strictly as an optional OS distribution / Gatekeeper notarization cost for signed DMGs. Development and local builds run at **$0 cost**.
- **Real-Time Safety:** Audio callback processing in `src-cpp/` strictly isolates DSP from dynamic memory allocation, file I/O, and ML inference. All 3-band LR4 crossover filters, biquad states, transition strategies, and audio buffers are pre-allocated at initialization.
