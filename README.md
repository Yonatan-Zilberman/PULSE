# PULSE

> **Autonomous, Local-First, Zero-Cloud DJ Application for macOS Apple Silicon (M1–M5)**

PULSE is an intelligent DJ application that executes seamless, harmonically aligned, and energy-aware transitions entirely on local hardware. Designed with a strict zero-cloud invariant, PULSE makes zero network calls, requires zero API keys, and has zero recurring software costs.

---

## Architecture Overview

PULSE is architected into three strictly isolated tiers:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│ 1. FRONTEND LAYER: Tauri 2 + React 18 + TypeScript + Zustand                │
│    - High-density dark DJ console, WebGL waveform renderers, queue manager  │
└─────────────────────────────────────────────────────────────────────────────┘
                                      │ IPC (Rust FFI)
┌─────────────────────────────────────────────────────────────────────────────┐
│ 2. APPLICATION CORE: Rust (pulse-core)                                      │
│    - SQLite library index, ML analysis scheduler, DJ Brain set planner      │
└─────────────────────────────────────────────────────────────────────────────┘
                                      │ Direct C ABI (extern "C")
┌─────────────────────────────────────────────────────────────────────────────┐
│ 3. AUDIO ENGINE LAYER: C++20 / JUCE 8                                       │
│    - Real-time CoreAudio dual-deck DSP, time-stretching, 3-band EQ, limiter │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## Prerequisites & System Requirements

- **Operating System:** macOS Apple Silicon (M1, M2, M3, M4, M5) running macOS Sonoma (14+) or Sequoia (15+).
- **Rust Toolchain:** `rustc` / `cargo` (1.80+ / 1.95+ recommended).
- **Node.js Environment:** `node` (v20+) and `pnpm` (v9+ / v11+).
- **C++ Compiler:** Apple Clang / Xcode Command Line Tools (`clang++` supporting C++20).
- **Build System:** `cmake` (3.20+ recommended, installable via `brew install cmake`).

---

## Quickstart & Local Setup

### 1. Install Frontend Dependencies
```bash
pnpm install
```

### 2. Verify Quality Gates & Test Suites

#### Licensing & Dependency Compliance Gate:
```bash
pnpm audit:licenses   # Or directly: ./scripts/audit_dependencies.sh
```

#### Frontend Linting, Build & Tests:
```bash
pnpm lint          # Run ESLint across TypeScript / React
pnpm build         # Run TypeScript typechecker and Vite bundler
pnpm test          # Run Vitest unit tests
```

#### Rust Core Formatting, Clippy & Unit Tests:
```bash
cargo fmt --manifest-path src-tauri/Cargo.toml -- --check
cargo clippy --manifest-path src-tauri/Cargo.toml -- -D warnings
cargo test --manifest-path src-tauri/Cargo.toml
```

#### C++ Audio Engine Build & Test Suite:
```bash
# Configure, build, and run CTest test suite:
cmake -B src-cpp/build -S src-cpp
cmake --build src-cpp/build
ctest --test-dir src-cpp/build --output-on-failure
```

#### Phase 0 CLI Audio-Engine Prototype:
```bash
# Generate deterministic test fixtures (steady, ambiguous, drifting, syncopated, multi-phrase):
./src-cpp/build/generate_fixtures tests/audio

# Run prototype CLI offline phrase-aware transition mix with pitch-preserving tempo matching:
./src-cpp/build/pulse_cli \
  --deck-a tests/audio/fixture_a.wav \
  --deck-b tests/audio/fixture_tempo_small_122bpm.wav \
  --phrase-aware \
  --phrase-bars 4 \
  --tempo-strategy source \
  --transition-strategy phrase_crossfade \
  --out tests/audio/phrase_mix_out.wav \
  --report tests/audio/phrase_mix_report.json
```


---

## Repository Structure

```
Pulse/
├── .github/workflows/ci.yml   # Multi-language CI pipeline (includes audit gate)
├── .gitignore                 # Artifact & cache ignore rules
├── BASELINE.md                # Engineering baseline & environment report
├── DEPENDENCIES.md            # Master dependency & licensing compliance registry
├── README.md                  # Project overview & quickstart
├── package.json               # Node & Tauri frontend scripts
├── tsconfig.json              # TypeScript root configuration
├── vite.config.ts             # Vite bundler & test harness
├── scripts/
│   └── audit_dependencies.sh  # Automated zero-cost & license verification script
├── src/                       # React 18 UI layer
│   ├── components/            # AutoDJ, Waveform, Queue, TransitionPreview, Mixer
│   ├── state/                 # Zustand store (useAppStore.ts)
│   └── App.tsx
├── src-tauri/                 # Rust core application tier
│   ├── Cargo.toml             # Rust dependencies (Tauri 2, Rusqlite, Tokio)
│   ├── tauri.conf.json        # Tauri desktop bundle configuration & offline CSP
│   └── src/
│       ├── dj_brain/          # Set planner, candidate generator, transition graph
│       ├── analysis/          # BPM, beatgrid, musical key, structure detectors
│       ├── library/           # SQLite track index & metadata cache
│       ├── models/            # TrackProfile, TransitionPlan, SetPlan schemas
│       └── audio_bridge/      # C ABI FFI bindings
├── src-cpp/                   # C++20 / JUCE Real-Time Audio Engine
│   ├── CMakeLists.txt         # Audio engine CMake build definition
│   ├── include/               # AudioEngine.h, DeckPlayer.h, AudioBridgeTypes.h
│   ├── src/                   # DSP implementations and C FFI bridge
│   └── tests/                 # C++ FFI smoke test harness
├── models/                    # Local ONNX model weights (htdemucs, structure)
├── analysis-cache/            # Local SQLite analysis caches (ignored in Git)
└── tests/                     # Golden set audio & sequencing benchmarks
```

---

## Zero-Cost Licensing & Invariants

PULSE strictly mandates zero recurring runtime costs, 100% offline execution, and full open-source license compatibility. All adopted and planned dependencies are formally cataloged and audited in [DEPENDENCIES.md](file:///Users/yonatanzilberman/Documents/Pulse/DEPENDENCIES.md):

- **Tauri 2 & React 18:** `MIT / Apache-2.0`.
- **Rust Core & Crates:** `MIT / Apache-2.0` (`tauri`, `serde`, `rusqlite`, `tokio`, `thiserror`).
- **JUCE 8:** Free under `JUCE Personal Tier` (for revenue <$50k/year) or `AGPL-3.0-only` for open-source builds.
- **SoundTouch:** `LGPL-2.1` dynamically linked (`.dylib`) to isolate relinking rights without forcing PULSE proprietary source release.
- **ONNX Runtime:** `MIT` with `CoreMLExecutionProvider` for local Apple Silicon Neural Engine / GPU acceleration.
- **Metadata & Artwork:** Embedded audio file tags only (`ID3v2`, `Vorbis`, `MP4 atoms`). Third-party scraping web APIs are strictly prohibited.
- **Zero Cloud Invariant:** 0 external network sockets, 0 telemetry pings, 0 cloud endpoints. Content Security Policy (`CSP`) enforces `default-src 'self'`.
- **Distribution Cost Segregation:** Apple Developer Program membership ($99/year) is documented strictly as an optional OS distribution / Gatekeeper notarization cost for signed DMGs. Development and local builds run at **$0 cost**.

