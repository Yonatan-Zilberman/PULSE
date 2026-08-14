# PULSE 🎛️

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
│    - SQLite library index, ML analysis scheduler, DJ Brain set planner     │
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

#### C++ Audio Engine Build & Smoke Test:
```bash
# Via CMake & CTest:
cmake -B src-cpp/build -S src-cpp
cmake --build src-cpp/build
ctest --test-dir src-cpp/build --output-on-failure

# Or direct Clang compile:
clang++ -std=c++20 -Wall -Wextra -Wpedantic -Isrc-cpp/include \
  src-cpp/src/AudioBridge.cpp src-cpp/src/AudioEngine.cpp \
  src-cpp/src/DeckPlayer.cpp src-cpp/src/Mixer.cpp \
  src-cpp/tests/test_audio_bridge.cpp -o test_audio_bridge && ./test_audio_bridge && rm test_audio_bridge
```

---

## Repository Structure

```
Pulse/
├── .github/workflows/ci.yml   # Multi-language CI pipeline
├── .gitignore                 # Artifact & cache ignore rules
├── BASELINE.md                # Engineering baseline & environment report
├── README.md                  # Project overview & quickstart
├── package.json               # Node & Tauri frontend scripts
├── tsconfig.json              # TypeScript root configuration
├── vite.config.ts             # Vite bundler & test harness
├── src/                       # React 18 UI layer
│   ├── components/            # AutoDJ, Waveform, Queue, TransitionPreview, Mixer
│   ├── state/                 # Zustand store (useAppStore.ts)
│   └── App.tsx
├── src-tauri/                 # Rust core application tier
│   ├── Cargo.toml             # Rust dependencies (Tauri 2, Rusqlite, Tokio)
│   ├── tauri.conf.json        # Tauri desktop bundle configuration
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

All dependencies in PULSE are strictly free and permissively licensed:
- **Tauri 2 & React 18:** MIT / Apache-2.0.
- **Rust Core & Crates:** MIT / Apache-2.0.
- **JUCE 8:** Free under JUCE Personal tier / AGPLv3.
- **SoundTouch:** LGPLv2.1 (dynamically linked).
- **ONNX Runtime:** MIT.
- **Zero Cloud:** No network calls, telemetry, or remote streaming APIs are configured or permitted.
