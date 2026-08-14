# PULSE — Dependency & Licensing Compliance Registry

**Document Version:** 1.0.0  
**Target Platform:** macOS Apple Silicon (M1–M5)  
**Architecture Conformance:** [PRD v4.0](Docs/Pulse%20PRD.md) & [Technical Design v1.0](Docs/Pulse%20Technical%20Design.md)  
**Zero-Cost Invariant:** 100% Free to Build, 100% Free to Run, Zero Cloud Telemetry, Zero Remote API Subscriptions.

---

## 1. Executive Summary & Zero-Cost Licensing Policy

PULSE is an autonomous, local-first DJ application engineered to execute seamlessly on macOS Apple Silicon hardware without recurring cloud costs, metered APIs, or proprietary license fees. To guarantee long-term maintainability and prevent intellectual property conflicts, all software dependencies incorporated into or planned for PULSE must satisfy three non-negotiable criteria:

1. **Zero Recurring Runtime Cost:** The dependency must not require paid API subscriptions, per-seat licenses, metered cloud tokens, or runtime telemetry tracking.
2. **Local-First / 100% Offline Execution:** The dependency must operate fully on-device without requiring network connectivity, outbound HTTP/REST calls, or third-party authentication servers.
3. **Open-Source & Distribution License Compatibility:** Permissible licenses include **MIT**, **Apache-2.0**, **ISC**, **BSD-2-Clause**, **BSD-3-Clause**, **0BSD**, **CC0-1.0**, **Unlicense**, **Apple-SDK** (system frameworks), **JUCE Personal Tier** (for indie builds under revenue threshold), and **LGPL-2.1** (strictly isolated via dynamic linking `.dylib` to preserve relinking rights without forcing open-sourcing of application core or UI logic).

---

## 2. Master Dependency Registry (9 Technical Domains)

| Domain / Subsystem | Dependency / Upstream Source | Target / Active Version | SPDX License | Integration & Linking Method | Runtime Network Activity | Cost Tier & Distribution Constraints |
|---|---|---|---|---|---|---|
| **1. Audio Decoding** | macOS `AudioToolbox.framework` & `CoreAudio.framework` (Apple) | macOS 14.0+ SDK | `Apple-SDK` / Proprietary System Runtime | Dynamic OS Framework (`-framework AudioToolbox -framework CoreAudio`) | `None (100% Offline)` | $0. Built into macOS OS; hardware-accelerated decoding (MP3, AAC/M4A, ALAC, FLAC, WAV, AIFF). |
| **1. Audio Decoding** | `symphonia` / `lofty` (Rust Ecosystem) | `0.5.x` / `0.21.x` | `MPL-2.0` / `MIT OR Apache-2.0` | Rust Cargo Crate (`src-tauri`) | `None (100% Offline)` | $0. Local pure-Rust decoding and fallback audio metadata extraction. |
| **2. DSP & Filters** | macOS `Accelerate.framework` (`vDSP` / `vForce`) | macOS 14.0+ SDK | `Apple-SDK` / Proprietary System Runtime | Dynamic OS Framework (`-framework Accelerate`) | `None (100% Offline)` | $0. Apple Silicon SIMD vector mathematics and FFT acceleration. |
| **2. DSP & Filters** | PULSE Native C++20 DSP (Biquad EQ, Limiters, Crossovers) | `0.1.0` (In-House) | `MIT OR Apache-2.0` | Static C++ Compilation (`src-cpp/`) | `None (100% Offline)` | $0. Custom real-time thread-safe DSP routines. |
| **2. DSP & Filters** | PULSE Rust DSP Math (Chroma correlation, Autocorrelation) | `0.1.0` (In-House) | `MIT OR Apache-2.0` | Rust Cargo Library (`pulse-core`) | `None (100% Offline)` | $0. Analysis heuristics (Krumhansl-Schmuckler, beat tracking). |
| **3. Time-Stretching** | `SoundTouch` Library | `2.3.x` | `LGPL-2.1-only` / `LGPL-2.1-or-later` | Dynamic Library (`.dylib` on macOS via C ABI / `dlopen`) | `None (100% Offline)` | $0. Default reference time-stretcher. Relinking requirement satisfied via dynamic `.dylib` isolation. |
| **3. Time-Stretching** | `Rubber Band Library` (Breakfast Quay) | `3.3.x` (Evaluated) | `GPL-2.0-or-later` / `Commercial` | Dynamic Library / Isolated Process | `None (100% Offline)` | $0 under GPL (requires open-source release); paid commercial license required for closed-source. SoundTouch preferred. |
| **4. Audio Engine** | `JUCE 8` Audio Framework (Raw Material Software) | `8.0.x` | `JUCE-Personal-Tier` / `AGPL-3.0-only` | C++ Static / Module Linkage (`src-cpp/`) | `None (100% Offline)` | $0 under JUCE Personal tier (<$50k/yr revenue) or AGPLv3 open-source. Revisit commercial only if revenue exceeds threshold. |
| **5. Rust Core / FFI** | `tauri` | `2.1.1` | `MIT OR Apache-2.0` | Rust Cargo Crate | `None (100% Offline)` | $0. Desktop runtime shell and IPC bridge. |
| **5. Rust Core / FFI** | `tauri-build` | `2.0.5` | `MIT OR Apache-2.0` | Rust Cargo Build Crate | `None (Build-time only)` | $0. Build-time helper for Tauri 2 application bundles. |
| **5. Rust Core / FFI** | `serde` | `1.0.216` | `MIT OR Apache-2.0` | Rust Cargo Crate | `None (100% Offline)` | $0. Zero-cost data serialization. |
| **5. Rust Core / FFI** | `serde_json` | `1.0.133` | `MIT OR Apache-2.0` | Rust Cargo Crate | `None (100% Offline)` | $0. JSON serialization/deserialization. |
| **5. Rust Core / FFI** | `rusqlite` (bundled SQLite 3) | `0.32.1` | `MIT` (SQLite: `Blessing` / Public Domain) | Rust Cargo Crate (Static bundled C SQLite) | `None (100% Offline)` | $0. Local embedded database for track index and analysis caching. |
| **5. Rust Core / FFI** | `tokio` | `1.42.0` | `MIT` | Rust Cargo Crate | `None (100% Offline)` | $0. Asynchronous executor for background library analysis orchestration. |
| **5. Rust Core / FFI** | `thiserror` | `2.0.9` | `MIT OR Apache-2.0` | Rust Cargo Crate | `None (100% Offline)` | $0. Compile-time error derivation. |
| **6. ML Inference** | `ONNX Runtime` (`ort`) | `1.20.x` | `MIT` | Dynamic / Static C++ Library & Rust C ABI | `None (100% Offline)` | $0. Hardware-accelerated local inference via CoreML Execution Provider (`CoreMLExecutionProvider`). |
| **7. Stem & Structure Models** | `Demucs / htdemucs` (Meta AI Research) | `v4` (ONNX Export) | `MIT` | Local ONNX Model File (`models/`) | `None (100% Offline)` | $0. Local 4-stem separation (vocals, drums, bass, other) executed on Apple Neural Engine / GPU. |
| **7. Stem & Structure Models** | In-House Structural Segmentation Model | `v1` (ONNX) | `MIT OR Apache-2.0` | Local ONNX Model File (`models/`) | `None (100% Offline)` | $0. Local musical structure identification (intro, build, drop, verse, outro). |
| **8. Metadata Parsing** | Embedded Tag Extractors (`ID3v2`, `Vorbis`, `MP4 Atoms`) | Native / `lofty` | `MIT OR Apache-2.0` / `Apple-SDK` | Rust Crate / C++ Reader | `None (100% Offline)` | $0. Reads embedded tags directly from audio file headers. Third-party scraping web APIs are strictly prohibited. |
| **9. UI & Desktop Shell** | `react` | `18.3.1` | `MIT` | npm Package | `None (100% Offline)` | $0. UI component rendering. |
| **9. UI & Desktop Shell** | `react-dom` | `18.3.1` | `MIT` | npm Package | `None (100% Offline)` | $0. React DOM bindings for desktop browser engine. |
| **9. UI & Desktop Shell** | `zustand` | `4.5.5` | `MIT` | npm Package | `None (100% Offline)` | $0. Lightweight reactive client-side state store. |
| **9. UI & Desktop Shell** | `lucide-react` | `0.468.0` | `ISC` | npm Package | `None (100% Offline)` | $0. Tree-shakeable SVG UI icons. |
| **9. UI & Desktop Shell** | `clsx` | `2.1.1` | `MIT` | npm Package | `None (100% Offline)` | $0. Conditional CSS class utilities. |
| **9. UI & Desktop Shell** | `@tauri-apps/api` | `^2.0.0` | `MIT OR Apache-2.0` | npm Package | `None (100% Offline)` | $0. Tauri 2 frontend TypeScript IPC interface. |
| **9. UI & Desktop Shell** | `@tauri-apps/plugin-shell` | `^2.0.0` | `MIT OR Apache-2.0` | npm Package | `None (100% Offline)` | $0. Tauri 2 safe OS shell plugin bindings. |
| **9. Build & Tooling** | `@tauri-apps/cli` | `^2.0.0` | `MIT OR Apache-2.0` | npm DevDependency | `None (Build-time only)` | $0. Desktop packaging CLI tooling. |
| **9. Build & Tooling** | `vite` | `^6.0.3` | `MIT` | npm DevDependency | `None (Build-time only)` | $0. Frontend build and HMR dev server. |
| **9. Build & Tooling** | `@vitejs/plugin-react` | `^4.3.4` | `MIT` | npm DevDependency | `None (Build-time only)` | $0. React Fast Refresh plugin for Vite. |
| **9. Build & Tooling** | `vitest` | `^2.1.8` | `MIT` | npm DevDependency | `None (Build-time only)` | $0. Fast unit test runner. |
| **9. Build & Tooling** | `jsdom` | `^25.0.1` | `MIT` | npm DevDependency | `None (Build-time only)` | $0. Headless DOM environment for unit testing. |
| **9. Build & Tooling** | `@testing-library/react` | `^16.1.0` | `MIT` | npm DevDependency | `None (Build-time only)` | $0. React component test utilities. |
| **9. Build & Tooling** | `@testing-library/jest-dom` | `^6.6.3` | `MIT` | npm DevDependency | `None (Build-time only)` | $0. Custom jest-dom matchers for Vitest. |
| **9. Build & Tooling** | `typescript` | `^5.7.2` | `Apache-2.0` | npm DevDependency | `None (Build-time only)` | $0. Static type checking. |
| **9. Build & Tooling** | `typescript-eslint` | `^8.17.0` | `MIT` | npm DevDependency | `None (Build-time only)` | $0. TypeScript AST tooling for ESLint. |
| **9. Build & Tooling** | `eslint` | `^9.16.0` | `MIT` | npm DevDependency | `None (Build-time only)` | $0. Static code linting. |
| **9. Build & Tooling** | `@eslint/js` | `^9.16.0` | `MIT` | npm DevDependency | `None (Build-time only)` | $0. ESLint JavaScript configuration utilities. |
| **9. Build & Tooling** | `eslint-plugin-react-hooks` | `^5.0.0` | `MIT` | npm DevDependency | `None (Build-time only)` | $0. React hook linting rules. |
| **9. Build & Tooling** | `eslint-plugin-react-refresh` | `^0.4.16` | `MIT` | npm DevDependency | `None (Build-time only)` | $0. React refresh linting rules. |
| **9. Build & Tooling** | `globals` | `^15.13.0` | `MIT` | npm DevDependency | `None (Build-time only)` | $0. Global variable definitions for ESLint. |
| **9. Build & Tooling** | `@types/node` | `^22.10.1` | `MIT` | npm DevDependency | `None (Build-time only)` | $0. Node.js ambient type definitions. |
| **9. Build & Tooling** | `@types/react` | `^18.3.12` | `MIT` | npm DevDependency | `None (Build-time only)` | $0. React ambient type definitions. |
| **9. Build & Tooling** | `@types/react-dom` | `^18.3.1` | `MIT` | npm DevDependency | `None (Build-time only)` | $0. React DOM ambient type definitions. |
| **9. Build & Tooling** | `prettier` | `^3.4.2` | `MIT` | npm DevDependency | `None (Build-time only)` | $0. Code formatting engine. |
| **9. Build & Tooling** | Apple Clang (`clang++`) & CMake | `Apple Clang 15+` / `CMake 3.20+` | `Apple-SDK` / `BSD-3-Clause` | Native Host Toolchain | `None (Build-time only)` | $0. Host compilation system for C++ audio engine. |

---

## 3. Subsystem Integration, Obligations & Architectural Analysis

### 3.1 Time-Stretching: SoundTouch (LGPLv2.1) vs. Rubber Band (GPLv2/v3 / Commercial)
- **Reference Decision:** Default to **SoundTouch** under `LGPL-2.1-only` / `LGPL-2.1-or-later`.
- **Obligation Mechanics:** LGPLv2.1 Section 6 requires that the application allow relinking against user-modified versions of the LGPL library. In PULSE, SoundTouch must be built as a separate dynamic library (`libsoundtouch.dylib`) and loaded at runtime via dynamic linker or C ABI (`dlopen`). This completely isolates the LGPL code and guarantees full compliance without requiring PULSE’s proprietary UI or Rust Core code to be open-sourced.
- **Rubber Band Evaluation:** Rubber Band Library provides high audio quality but is licensed under `GPL-2.0-or-later` or a paid commercial license. Under GPL, linking Rubber Band directly would trigger copyleft obligations on the entire application binary. Rubber Band is maintained solely as an optional alternative for fully open-source builds or if isolated across a dedicated IPC process boundary.

### 3.2 Audio Engine Framework: JUCE 8 (Personal Tier vs. AGPLv3)
- **Reference Decision:** Adopt **JUCE 8** under the free **JUCE Personal Tier** (or AGPLv3 for open source).
- **Obligation Mechanics:** JUCE Personal is free for individuals and businesses with revenue below $50,000 USD/year. No upfront license fees are required. Reevaluating commercial tier licensing is treated as a future commercial milestone rather than a technical prerequisite.

### 3.3 ML Inference: ONNX Runtime & Apple Neural Engine (ANE)
- **Reference Decision:** **ONNX Runtime (MIT)** with `CoreMLExecutionProvider`.
- **Execution Strategy:** All model inference (Demucs stem separation, musical structure segmentation) executes strictly on-device using Apple Silicon's Neural Engine and Unified Memory GPU. Zero network calls or cloud endpoints exist.
- **CoreML Fallback Guard:** Certain INT8 quantized operations can silently fall back to CPU. The ML engine subsystem must programmatically log and verify the active execution provider at runtime to ensure GPU/ANE acceleration is active.

### 3.4 Local Metadata & Zero Cloud Invariant
- **Reference Decision:** Local embedded tags only (`ID3v2.3/2.4`, `Vorbis Comments`, `MP4/M4A atoms`, `FLAC metadata`).
- **Network Invariant:** Zero remote calls to third-party scraping APIs (e.g., MusicBrainz, Discogs, Spotify Web API, Last.fm) are permitted in the core playback path. Album artwork and track tags are parsed directly from local audio file buffers.

### 3.5 External Distribution Cost: Apple Developer Program ($99/year)
- **Classification:** **Distribution & OS Gatekeeper Cost ONLY.**
- **Cost Reality:** Ad-hoc signed development builds run locally on macOS Apple Silicon at **$0 cost**. The Apple Developer Program fee ($99/year) is strictly required for official Apple Notarization and Gatekeeper distribution of packaged `.dmg` / `.app` binaries to untrusted third-party machines. It is not an engineering, development, or runtime dependency.

---

## 4. Network Ingress / Egress & Security Policy

PULSE strictly isolates its desktop shell using Tauri's Content Security Policy (`CSP`):

```json
"security": {
  "csp": "default-src 'self'; img-src 'self' asset: https://asset.localhost; style-src 'self' 'unsafe-inline'"
}
```

- **0 Outbound Sockets:** Neither the C++ real-time audio thread nor the Rust `pulse-core` process binds to external network sockets or executes background telemetry pings.
- **Local Asset Protocol:** Local track album artwork is served securely through Tauri’s custom `asset://` protocol directly from disk.

---

## 5. Automated Verification & Maintenance Rules

To prevent introducing unauthorized dependencies:
1. **Automated Audit Script:** Execute `./scripts/audit_dependencies.sh` (or `pnpm audit:licenses`) to verify that all dependencies in `package.json` and `src-tauri/Cargo.toml` conform to allowed SPDX categories and are cataloged in this document.
2. **Standing Review Gate:** Before adding any crate, npm package, or C++ library, confirm:
   - It runs 100% offline with zero cloud telemetry.
   - It has no per-call, per-seat, or metered fees.
   - Its license is compatible with the PULSE license matrix.
   - It is explicitly added to Section 2 of this file.
