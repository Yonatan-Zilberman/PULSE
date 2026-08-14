# PULSE — Technical Design Document

**Companion to:** PULSE PRD v4.0
**Scope:** V1 (macOS, Apple Silicon M1–M5), with explicit notes on what changes for the planned cross-platform phase.

---

## 1. Purpose & Relationship to the PRD

The PRD defines *what* PULSE must do and *why*. This document defines *how* it is built: architecture, data models, algorithms, on-device ML/DSP choices, licensing decisions, performance targets, failure handling, and repository structure. Section numbers here are independent of the PRD; cross-references are called out explicitly.

The single hardest constraint driving this document is the one from PRD Section 7: **PULSE must run entirely on local hardware, make zero outside API calls, and cost nothing — to the user, and to whoever builds it — indefinitely.** Every architectural and dependency decision below is filtered through that constraint first, performance second.

---



## 2. System Architecture Overview

PULSE is three layers connected by two hard boundaries: a UI/IPC boundary and a real-time-safety boundary.

```
┌─────────────────────────────────────────────────────────────────────────────┐
│ FRONTEND LAYER: Tauri 2 + React 18 + TypeScript + Zustand                   │
│ - Waveform rendering (WebGL/Canvas), queue UI, deck controls,               │
│   transition preview, energy-curve editor                                   │
└─────────────────────────────────────────────────────────────────────────────┘
                          │ IPC via Rust FFI — no UI operation may block audio
┌─────────────────────────────────────────────────────────────────────────────┐
│ APPLICATION LAYER: Rust Core                                                │
│ - Library index, analysis orchestration & caching, DJ Brain, ML scheduling  │
└─────────────────────────────────────────────────────────────────────────────┘
                          │ Direct C FFI / shared memory
┌─────────────────────────────────────────────────────────────────────────────┐
│ AUDIO ENGINE LAYER: C++ / JUCE                                              │
│ - Dual-deck playback (CoreAudio), time-stretch, EQ, filters, mixer          │
└─────────────────────────────────────────────────────────────────────────────┘
```

**Per-deck signal chain:**

```
Decoder → Tempo Engine → Pitch/Time-Stretch → EQ → Filter → Stem Mixer → Gain
                                                                              ↘
Deck A ─────────────────────────────────────────────────────────────────────→ Mixer → Master DSP → Output
Deck B ─────────────────────────────────────────────────────────────────────↗
```

**Decision flow (ahead of real time):**

```
ML inference → Track metadata → DJ Brain (decision engine) → Transition plan → Real-time DSP engine
```

**Long-term architecture (target shape once V2 lands):**

```
                    USER INTENT
                         │
             ┌───────────▼───────────┐
             │       SET BRAIN       │   mood, energy, genre, session progression
             └───────────┬───────────┘
             ┌───────────▼───────────┐
             │     SET PLANNER       │   lookahead, track selection, future optimization
             └───────────┬───────────┘
             ┌───────────▼───────────┐
             │ TRANSITION PLANNER    │   candidate generation, strategy selection, do-not-mix
             └───────────┬───────────┘
             ┌───────────▼───────────┐
             │ MUSIC INTELLIGENCE    │   BPM/key/structure, energy/vocals, mixability/stems
             └───────────┬───────────┘
             ┌───────────▼───────────┐
             │ TRANSITION EXECUTOR   │   EQ/filters/stems, sync/DSP/effects
             └───────────┬───────────┘
             ┌───────────▼───────────┐
             │      AUDIO OUTPUT     │
             └───────────────────────┘
```

---



## 3. Technology Stack


| Layer                  | Choice                                                                                                        | Why                                                                                                                     |
| ---------------------- | ------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------- |
| Shell / packaging      | Tauri 2                                                                                                       | Cross-platform by design (eases the planned V3 port), tiny footprint vs. Electron, no bundled Chromium license concerns |
| UI                     | React 18 + TypeScript + Zustand                                                                               | Standard, free, permissively licensed (MIT)                                                                             |
| Application core       | Rust                                                                                                          | Memory safety without a GC, first-class FFI to C++, strong async story for the analysis scheduler                       |
| Real-time audio engine | C++ / JUCE                                                                                                    | Industry-standard real-time audio framework with mature CoreAudio (and, later, WASAPI/ALSA) backends                    |
| ML inference           | ONNX Runtime with the CoreML Execution Provider (V1); CPU/DirectML/CUDA EPs added in the cross-platform phase | One model format, multiple free hardware backends — avoids re-authoring models per platform                             |


---



## 4. Zero-Cost Engineering Constraints & Licensing Analysis

This section exists specifically because the PRD requires the product to be free to build, not just free to use. Every dependency below is chosen (or flagged) with that in mind.


| Dependency                                    | Purpose                                              | License / cost reality                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              | Decision                                                                                                                                                                                                                                                                                                                                 |
| --------------------------------------------- | ---------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **JUCE 8**                                    | Real-time audio engine framework                     | Free under the "JUCE Personal" tier for individuals/small teams under JUCE's revenue threshold, or under AGPLv3 if the app is released as open source. A paid commercial license is only required above that revenue threshold or to ship closed-source without AGPL obligations.                                                                                                                                                                                                                                   | Build and ship under the free tier. Revisit licensing only if/when PULSE generates revenue past JUCE's published threshold — treat that as a future business decision, not a V1 engineering cost.                                                                                                                                        |
| **Time-stretch / pitch-shift engine**         | Tempo matching without pitch artifacts               | Rubber Band Library is GPLv2/v3 (free) or commercial (paid). SoundTouch is LGPLv2.1 (free, and usable in closed-source builds via dynamic linking without releasing PULSE's own source).                                                                                                                                                                                                                                                                                                                            | Default to **SoundTouch (LGPL)** for the reference build to keep licensing friction minimal regardless of how PULSE is eventually distributed. Rubber Band under GPL remains an option if PULSE ships fully open-source and prefers its audio quality — decide in Phase 1 based on A/B listening tests, not licensing convenience alone. |
| **Stem separation model (Demucs / htdemucs)** | Vocal/drum/bass/other stem isolation                 | MIT license (Meta AI Research)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                      | Free to use, modify, and ship, including commercially. No cost.                                                                                                                                                                                                                                                                          |
| **ONNX Runtime**                              | Cross-hardware ML inference                          | MIT license                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         | Free.                                                                                                                                                                                                                                                                                                                                    |
| **BPM/onset/beat tracking**                   | Tempo & beatgrid detection                           | Implement using well-documented, license-clear DSP techniques (onset-strength envelopes + autocorrelation/comb-filter tempo estimation) rather than depending on a single GPL C library where avoidable, so the core analysis pipeline stays license-flexible regardless of future distribution model. Where an existing free library is used for prototyping (e.g., aubio, GPLv3), keep it isolated behind a swappable trait/interface in Rust so it can be replaced without a redesign if licensing needs change. | Free either way; isolate behind an interface.                                                                                                                                                                                                                                                                                            |
| **Key detection**                             | Musical key / Camelot mapping                        | Krumhansl-Schmuckler-style chroma/key-profile correlation is a published, license-free algorithm (not a product or API) — implement directly rather than calling any third-party key-detection service.                                                                                                                                                                                                                                                                                                             | Free, no licensing question at all.                                                                                                                                                                                                                                                                                                      |
| **Structural segmentation model**             | Intro/verse/chorus/drop/etc. detection               | Train or fine-tune a small local model (or adapt a permissively-licensed open research model) and export to ONNX. Confirm license terms per candidate model before adoption — reject any option requiring a paid API tier or non-commercial-only license.                                                                                                                                                                                                                                                           | Requires a per-model license check before adoption (tracked as a Phase 2 spike); default to training a lightweight in-house model on permissively-licensed/royalty-free audio if no suitable free model is found.                                                                                                                        |
| **Metadata / artwork**                        | Track title, artist, album art                       | Read exclusively from embedded file tags (ID3/Vorbis/MP4). No MusicBrainz/Discogs/streaming metadata API calls in the default build.                                                                                                                                                                                                                                                                                                                                                                                | Keeps the "zero outside API calls" rule absolute, not just applied to ML.                                                                                                                                                                                                                                                                |
| **Apple code signing & notarization**         | Distributing a signed build outside of local/dev use | Apple Developer Program: **$99/year.** This is the one real, unavoidable cost in the entire stack — and it is a distribution cost, not a development or runtime cost.                                                                                                                                                                                                                                                                                                                                               | Call this out explicitly wherever "completely free" is stated publicly. Local/dev builds (ad-hoc signed, run on the developer's own machine) incur no cost at all. Treat notarization as a deliberate later decision tied to public distribution, not a Phase 0–5 requirement.                                                           |


**Master Compliance Registry & Automated Gate:** All 9 technical domains, exact SPDX licenses, dynamic linking isolation mechanics (e.g. SoundTouch LGPLv2.1 `.dylib` isolation), and runtime network constraints are formally audited and maintained in [DEPENDENCIES.md](file:///Users/yonatanzilberman/Documents/Pulse/DEPENDENCIES.md) and verified via `./scripts/audit_dependencies.sh`.

**Standing rule for future dependencies:** before adding any new library, model, or service, confirm (1) it runs fully offline, (2) it has no per-call/per-seat/revenue-tied fee, (3) its license is compatible with however PULSE is ultimately distributed, and (4) it is registered in `DEPENDENCIES.md`. Verify compliance via `pnpm audit:licenses`.

---



## 5. Core Data Models



### TrackProfile

```
TrackProfile {
    id
    metadata { title, artist, album, genre, duration, artwork, source, sample_rate, channels }

    tempo {
        bpm, bpm_confidence, alternative_bpm_hypotheses,
        beat_positions, downbeat_positions, bar_positions,
        tempo_change_points, grid_offset
    }

    key {
        key, camelot, key_confidence, chroma_profile, key_changes
    }

    structure {
        segments: [{ type, start, end, confidence, energy, vocal_density, instrumental_density }]
        // types: intro, verse, pre-chorus, chorus, breakdown, build, drop,
        //        instrumental, bridge, outro, silence, hard_ending
    }

    phrases {
        boundaries_4bar, boundaries_8bar, boundaries_16bar, boundaries_32bar, boundaries_64bar
    }

    energy_curve            // per-section energy profile, not a single number
    loudness_profile        // integrated LUFS, short-term, peak, true peak, dynamic range
    vocal_profile           // density, prominence, phrase boundaries
    instrumentation         // drum density, bass intensity, melodic density, harmonic complexity

    mixability {
        intro_quality, outro_quality, phrase_stability,
        vocal_isolation_feasibility, beat_stability, tempo_stability, transition_option_count
    }

    embeddings               // V2 — music similarity vector
    stem_cache_status
    confidence               // rollup across all of the above
}
```



### TransitionCandidate

```
TransitionCandidate {
    source_track, destination_track
    start_position, end_position, duration

    transition_type          // see Section 8 type library

    bpm_score, harmonic_score, phrase_score,
    energy_score, vocal_score, bass_score, structural_score

    artifact_risk
    confidence

    required_stems           // [] if none needed
}
```



### TransitionPlan

```
TransitionPlan {
    source_track, destination_track
    start_position, end_position, duration

    tempo_strategy, phrase_alignment
    eq_automation, filter_automation
    vocal_strategy, bass_strategy, stem_strategy
    crossfader_curve, effect_strategy

    confidence
}
```



### SetPlan

```
SetPlan {
    energy_curve
    tracks[]
    transitions[]
    backup_tracks[]
    confidence
}
```

---



## 6. Real-Time Audio Contract

The real-time audio thread must:

- process audio and read **precomputed** parameters only
- perform deterministic DSP
- never block, never perform network I/O, never perform ML inference
- avoid unpredictable memory allocation
- never depend on UI state directly
- run at high real-time thread priority

The application layer (Rust core / DJ Brain) prepares **all** transition parameters ahead of time. The audio engine executes a `TransitionPlan` deterministically — it never decides *"maybe we should choose another song"*; that decision belongs entirely to the DJ Brain, upstream, off the real-time thread.

**Stem separation for a transition must be precomputed and cached** the moment a track enters the queue (or, at the latest, while the destination track is already playing silently ahead of the handoff) — never computed live inside the transition window. Inference timing has variance; audio continuity cannot be gated on an ML call finishing in time.

---



## 7. Track Analysis Pipeline

Every track runs through this pipeline asynchronously, off the real-time thread, writing results into the persisted `TrackProfile` cache (Section 12).

1. **Decode & normalize** — sample-rate/channel normalization for analysis (playback path stays at native format).
2. **Tempo & beatgrid** — onset-strength detection → autocorrelation/comb-filter tempo estimation → beat/downbeat/bar tracking. Must support non-constant-tempo recordings (live drums, DJ edits) rather than assuming one fixed BPM; carries a confidence score and alternative BPM hypotheses (e.g., half-time/double-time ambiguity).
3. **Harmonic analysis** — chroma feature extraction → Krumhansl-Schmuckler-style key-profile correlation → Camelot mapping, plus key-change detection.
4. **Structural segmentation** — local ONNX model (Section 4) producing per-segment type + confidence. Low-confidence segments fall back to energy-based phrase boundaries (every 8/16/32 bars) rather than mistiming a transition silently.
5. **Phrase analysis** — 4/8/16/32/64-bar boundary detection, used preferentially as transition start/end points unless a strategy deliberately breaks alignment (e.g., a hard cut on a drop).
6. **Energy model** — per-section energy, not a single track-level number, e.g.:
  ```
   Intro       3.2      Verse       6.7
   Build       5.1      Chorus      8.1
   Drop        8.9      Breakdown   4.8
                         Outro       6.0
  ```
7. **Loudness analysis** — integrated/short-term LUFS, peak, true peak, dynamic range.
8. **Low-level features** — vocal density, drum density, bass intensity, melodic density, harmonic complexity, spectral brightness, danceability, mood/genre probability.
9. **Mixability score** — composite of the above, letting the sequencer prefer easier-to-mix tracks at the right moments.

Every stage writes a confidence value into the `TrackProfile`, surfaced in the UI (advanced mode) so low-confidence tracks can be spot-checked before they cause a bad live transition.

---



## 8. DJ Brain Algorithm



### 8.1 Candidate scoring

MVP1 baseline (concrete, four-dimension — see PRD Section 12.1 for the rationale and weights):

```
CandidateScore_MVP1 = 0.35 * (1 - BPM_Diff_norm)
                     + 0.30 * (1 - KeyDistance_norm)     // added once key detection ships (MVP2)
                     + 0.25 * (1 - EnergyDelta_norm)
                     + 0.10 * (1 - RepeatPenalty)
```

Full MVP2/MVP3 target:

```
CandidateScore = tempo compatibility + harmonic compatibility + phrase compatibility
                + energy compatibility + structural compatibility + vocal compatibility
                + intro/outro compatibility + genre compatibility + mood compatibility
                + novelty + transition confidence
```

Weights are context-sensitive at runtime — e.g., vocal-collision risk is weighted far more heavily when both candidate tracks have prominent vocals (per their `vocal_profile`).

### 8.2 Transition graph & lookahead

Model the forward queue as a graph, not a chain:

```
                  Track B
                 /       \
                /         \
Track A ────────            Track D
                \         /
                 \       /
                  Track C
```

Each edge carries one or more `TransitionCandidate`s. The planner evaluates multiple forward **paths**, not just the next edge — a track with the highest immediate score is not necessarily correct if it dead-ends into weak follow-on options:

```
A → B = 96%        A → D = 91%
B → C = 41%         D → E = 93%
```

PULSE prefers `A → D → E` over `A → B → C` because the overall path is stronger. Minimum lookahead: 2 tracks. Preferred: 4–8 tracks.

### 8.3 The "Do Not Mix" outcome

Every `TransitionCandidate` set must include the possibility of resolving to `DO_NOT_MIX`, with a logged reason (poor beatgrid, excessive BPM difference, incompatible structure, vocal collision, insufficient intro/outro, low confidence, excessive predicted artifacts, poor musicality). When this wins, PULSE either lets the current track finish (Finish-and-Start), uses a plain crossfade, or selects a different destination track entirely. This is a normal, expected output — not an error path.

---



## 9. Transition Execution Model



### 9.1 Reference phase breakdown — Classic EQ Blend

Other transition types adapt or simplify from this reference model:


| Phase                 | Timing         | Actions                                                                                                                                                                                                                                 |
| --------------------- | -------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1. Sync & Phase Align | Bar −32 to −16 | Time-stretch destination to source's tempo (0% pitch deviation). Begin destination silently at 0dB. Trigger stem-cache warm-up if not already cached.                                                                                   |
| 2. EQ Blend & Swap    | Bar −16 to −4  | HPF on destination's bass, gradually open fader. Apply LUFS-matched gain staging so perceived loudness stays constant through the blend. Bass handoff is sequenced (Section 9.3), never simultaneous full-energy bass from both tracks. |
| 3. Vocal/Stem Isolate | Bar −8 to 0    | If both tracks have prominent vocals, mute source's *precomputed* vocal stem while destination's vocal enters.                                                                                                                          |
| 4. Cut & Tempo Ramp   | Bar 0+         | Cut source. Ramp destination back to native BPM over 16 bars if it was stretched.                                                                                                                                                       |


Other types simplify this: a **Hard Cut** skips phases 1–3 entirely; a **Drop Transition** compresses phases 2–3 around the impact moment; a **Percussion Blend** runs an extended phase 2 without a full vocal handoff; **Finish-and-Start** skips the entire phase model and simply sequences track end → track start with a minimal, safe gap/fade.

### 9.2 Candidate transition windows

Don't default to a fixed bar count. Generate candidate windows from song structure, available outro, next track's intro, phrase boundaries, vocal activity, energy, and confidence; score each; pick the best:

```
Candidate windows: A: 64 bars before end   B: 32 bars before end
                    C: 16 bars before end   D: 8 bars before end
```



### 9.3 Bass collision prevention

Bass handoff is sequenced, not simultaneous:

```
Outgoing bass         Incoming bass
██████████                   ███
████████                       █████
██████                           ███████
```



### 9.4 Tempo matching

Prefer small tempo adjustments over large audible time-stretch artifacts. Large tempo changes incur an increasing penalty in candidate scoring. If a transition would require excessive stretching, prefer picking another track, a transition strategy tolerant of the difference, a deliberate tempo change, or Finish-and-Start over forcing an unnatural match.

---



## 10. Stem Separation Subsystem

- **Model:** htdemucs (Demucs), MIT-licensed, exported to ONNX.
- **Execution:** ONNX Runtime with the CoreML Execution Provider on Apple Silicon; runs entirely on-device, no network call.
- **Engineering reality check:** converting htdemucs to ONNX is genuinely nontrivial — it involves complex-valued STFT tensors and custom fused-attention kernels that standard exporters historically didn't handle cleanly. Budget real R&D time for this in Phase 5, not a checkbox integration task.
- **Known CoreML pitfall:** certain INT8-quantized ops (e.g., `ConvInteger`, `DynamicQuantizeLinear`) are not supported by the CoreML execution provider and silently fall back to CPU with no warning — a build can *think* it's using the Neural Engine while actually being CPU-bound. Add an explicit runtime check that logs which execution provider actually ran each inference; QA must alert if CPU fallback is detected on a machine with a working Neural Engine.
- **Decision engine:** before generating stems, ask *"will stems materially improve this transition?"* Generate only the stems required (vocal-suppression need → vocal stem only; bass-swap need → bass stem only; drum-only transition → drums only).
- **Caching:** results are cached to disk/memory ahead of need. Generation never happens inside the transition window itself (Section 6).
- **Performance target:** don't commit to a specific number until benchmarked on target hardware. Publicly reported Apple Silicon HTDemucs benchmarks cluster around 30–35x realtime on high-end M-series chips using optimized native frameworks — treat that as a ceiling, not a floor, for a base M1 target. V1 target: "full stem separation completes well before the track needs to play, verified empirically," rather than a hardcoded second count. Benchmark independently across M1, M1 Pro/Max, M2, M2 Pro/Max, M3, M3 Pro/Max, M4, and M5, across multiple memory configurations, before locking a spec number.
- **Graceful degradation:** if the Neural Engine is unavailable or the user disables ML features, fall back to a standard EQ-based crossfade without vocal isolation — never fail outright.

---



## 11. Confidence Model & Failure Recovery Architecture



### 11.1 Confidence model

Every analysis stage and every DJ Brain decision carries a numeric confidence:

```
BPM confidence:             0.98
Beatgrid confidence:        0.95
Key confidence:             0.81
Structure confidence:       0.76
Vocal confidence:           0.93
Transition confidence:      0.91
```

Low-confidence decisions automatically select safer strategies. Confidence is visible in advanced UI but never overwhelms the default/simple UI.

### 11.2 Per-subsystem recovery


| Subsystem                  | Failure mode                       | Fallback                                     |
| -------------------------- | ---------------------------------- | -------------------------------------------- |
| Analysis                   | Any stage fails/times out          | Use simplified analysis for that stage       |
| Beatgrid                   | Low confidence / detection failure | Use conservative crossfade timing            |
| Key                        | Detection failure                  | Ignore harmonic constraint for that pair     |
| Structure                  | Low confidence / unusual structure | Use phrase/energy boundaries instead         |
| Stem generation            | Model or hardware failure          | Use normal (non-stem) DSP path               |
| Transition (mid-execution) | Confidence collapses live          | Abort advanced transition, use safe fallback |
| Destination track          | Fails to load/decode               | Skip to backup track                         |
| Audio decoding             | Corrupt/unsupported file           | Skip track, notify user, continue session    |




### 11.3 Top-level fallback hierarchy

PULSE always prefers the simplest successful strategy:

```
Advanced transition
       ↓ if unsafe
Simplified transition
       ↓ if unsafe
Standard crossfade
       ↓ if unsafe
Finish current track
       ↓
Start backup track
```

This hierarchy is a core reliability requirement, not an edge case. The listener must never hear the application "break."

---



## 12. Caching & Incremental Analysis

- **Incremental analysis:** playback must not wait for the full library to finish analyzing. On import of e.g. 1,000 tracks, playback begins immediately while analysis proceeds in priority order: (1) current track, (2) next queue candidates, (3) future queue, (4) remainder of library.
- **Analysis cache:** persisted per track — beatgrid, BPM, key, structure, energy, vocal/bass density, mixability, embeddings, stem availability. A track should not require full re-analysis on every app launch.
- **Model versioning:** cached analysis is tagged with the model version that produced it, so a model update can selectively invalidate only the affected cache entries rather than forcing a full re-scan.
- **Storage:** local database (e.g., embedded SQLite) plus a stem/analysis blob cache on disk — no cloud sync in V1, consistent with Section 4.

---



## 13. Performance & Benchmarking Plan


| Target                      | V1 Spec                                                                                |
| --------------------------- | -------------------------------------------------------------------------------------- |
| Audio thread                | Strict real-time priority; zero allocation, zero ML, zero blocking I/O in the callback |
| UI                          | 60 FPS, smooth waveform scrolling, low interaction latency                             |
| Import → ready-to-mix       | < 2s/track local file, p95 < 5s                                                        |
| Idle resource footprint     | < 250 MB RAM, < 15% CPU sustained on base M1                                           |
| Continuous playback         | 12-hour soak test, zero dropouts/xruns                                                 |
| Track analysis success rate | ≥ 99% of supported formats                                                             |


**Benchmarking matrix (required before locking any hard number in the PRD):** M1, M1 Pro/Max, M2, M2 Pro/Max, M3, M3 Pro/Max, M4, M5 — each across at least two RAM configurations. Every benchmark run must log which ONNX execution provider actually executed (Section 10) so a silent CPU fallback never masquerades as a Neural Engine result.

---



## 14. Testing Strategy

- **Unit tests:** DSP primitives, scoring functions, cache correctness.
- **Golden test set (PRD Section 16):** fixed hard-transition corpus (large BPM gaps, bad beatgrids, live drums, mid-track tempo changes, vocal-heavy tracks, unusual structures, half/double-time, key changes, genre changes) — every audio-engine release must pass it before merge.
- **Human evaluation pipeline:** blind-rated transitions (timing, beat alignment, musicality, energy continuity, vocal interaction, artifact quality) and blind-rated full sets (coherence, pacing, variety, overall quality). Tune the system against this dataset, not only automated confidence metrics.
- **Soak tests:** 8–12 hour continuous playback runs, automated dropout/xrun detection.
- **Fallback-path tests:** deliberately induce every failure mode in Section 11.2 and assert the correct fallback fires and playback never audibly breaks.

---



## 15. Repository Structure

```
pulse/
├── src/                        # React frontend
│   ├── components/
│   │   ├── AutoDJ/
│   │   ├── Waveform/
│   │   ├── Queue/
│   │   ├── TransitionPreview/
│   │   ├── EnergyCurve/
│   │   └── Mixer/
│   ├── state/
│   ├── hooks/
│   └── App.tsx
│
├── src-tauri/                  # Rust application core
│   ├── src/
│   │   ├── dj_brain/
│   │   │   ├── set_planner.rs
│   │   │   ├── transition_graph.rs
│   │   │   ├── candidate_generator.rs
│   │   │   ├── energy_planner.rs
│   │   │   └── recovery.rs
│   │   ├── analysis/
│   │   │   ├── bpm.rs
│   │   │   ├── beatgrid.rs
│   │   │   ├── key.rs
│   │   │   ├── structure.rs
│   │   │   └── loudness.rs
│   │   ├── library/
│   │   ├── ml_engine.rs
│   │   ├── audio_bridge.rs
│   │   └── main.rs
│   └── Cargo.toml
│
├── src-cpp/                     # C++/JUCE audio engine
│   ├── include/
│   │   ├── DeckPlayer.h
│   │   ├── AudioEngine.h
│   │   ├── Mixer.h
│   │   └── TransitionExecutor.h
│   ├── AudioEngine.cpp
│   ├── DeckPlayer.cpp
│   ├── Mixer.cpp
│   └── CMakeLists.txt
│
├── models/                      # ONNX models (structure, stem separation)
├── analysis-cache/
└── tests/
    ├── audio/
    ├── transitions/
    ├── sequencing/
    └── golden-set/
```

---



## 16. Build, Packaging & Distribution

- **Local development:** ad-hoc signed builds run on the developer's own Mac at zero cost. This covers Phases 0–5 entirely.
- **Public distribution (notarized .app / DMG):** requires an Apple Developer Program membership — **$99/year.** This is the one real recurring cost in the entire project, and it is a *distribution* cost, not a *build* cost. It should be budgeted separately from engineering and explicitly disclosed anywhere PULSE is described as "completely free" (users pay nothing; the one entity that ever pays is whoever notarizes and distributes signed builds, once, per year).
- **Alternative:** ship as source with a build script, letting technically capable users build and run unsigned/self-signed locally at zero cost — consistent with the project's open, zero-cost ethos, and a reasonable V1 distribution path before committing to the $99/year notarization cost.
- **Cross-platform builds (Phase 7):** Tauri and JUCE both already support Windows/Linux, which is why they were selected in Phase 1 over macOS-only alternatives. The main porting work is (a) swapping the CoreML execution provider for CPU/DirectML/CUDA depending on target hardware, and (b) swapping CoreAudio for WASAPI (Windows) / ALSA or PipeWire (Linux) in the JUCE audio backend. No Windows/macOS code-signing-equivalent cost is required for Linux; Windows has its own (optional) code-signing certificate cost if a fully trusted installer is desired, which should be evaluated at that phase rather than assumed now.

---



## 17. Roadmap-to-Code Mapping


| PRD Phase                | Primary code areas touched                                                            |
| ------------------------ | ------------------------------------------------------------------------------------- |
| 0. Feasibility           | `src-cpp/` prototype CLI (no Tauri/UI yet), `analysis/bpm.rs`, `analysis/beatgrid.rs` |
| 1. Audio Engine          | `src-cpp/` full JUCE engine, `audio_bridge.rs`                                        |
| 2. Music Intelligence    | `analysis/*.rs`, `models/` (structure model), `library/`                              |
| 3. DJ Brain              | `dj_brain/*.rs`                                                                       |
| 4. Product UX            | `src/` (all React components), Tauri IPC layer                                        |
| 5. Advanced Intelligence | `models/` (stem model), `ml_engine.rs`, stem cache in `analysis-cache/`               |
| 6. V2 Platform           | `dj_brain/` extensions (taste model, embeddings), new UI modes                        |
| 7. Cross-Platform        | `src-cpp/` backend swap, `ml_engine.rs` execution-provider abstraction                |


---



## 18. Definition of "Professional" (Engineering Target)

A professional-feeling transition should satisfy as many of the following as appropriate — this is the qualitative bar every algorithm in Sections 8–9 is engineered against:

- beats remain aligned
- transition occurs at a meaningful musical point
- phrase boundaries make sense
- bass interaction is controlled (Section 9.3)
- vocals do not clash (Section 11 / PRD Section 12.4)
- loudness remains stable
- energy change feels intentional
- no obvious DSP artifacts occur
- incoming track is introduced naturally, outgoing track is removed naturally
- the transition supports the larger set, not just the immediate pair (Section 8.2)

**Product quality bar:** if the user did not know PULSE was performing the mix, would they believe a competent DJ had selected and mixed these tracks? That is the standard every section of this document optimizes toward — at zero cost, on hardware the user already owns.