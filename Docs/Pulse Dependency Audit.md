# PULSE — Dependency Audit & Licensing Specification

**Companion to:** [Pulse Technical Design.md](file:///Users/yonatanzilberman/Documents/Pulse/Docs/Pulse%20Technical%20Design.md) and [Pulse PRD.md](file:///Users/yonatanzilberman/Documents/Pulse/Docs/Pulse%20PRD.md)  
**Root Compliance Document:** [DEPENDENCIES.md](file:///Users/yonatanzilberman/Documents/Pulse/DEPENDENCIES.md)  

This document codifies the mandatory licensing and zero-cost audit defined in Section 4 of the PULSE Technical Design Document.

---

## 1. Domain Coverage

PULSE audits all 9 core technical domains against its zero-cost, local-first, and licensing invariants:

1. **Audio Decoding:** macOS `AudioToolbox` / `CoreAudio` (`Apple-SDK`), pure Rust `symphonia` (`MPL-2.0`) / `lofty` (`MIT OR Apache-2.0`).
2. **DSP & Filters:** macOS `Accelerate.framework` (`vDSP`), C++20 biquad filters, Rust DSP math (`MIT OR Apache-2.0`).
3. **Time-Stretching & Pitch-Shifting:** `SoundTouch` (`LGPL-2.1`, dynamic `.dylib` linkage) vs. `Rubber Band Library` (`GPL-2.0-or-later` / commercial).
4. **Audio Engine Framework:** `JUCE 8` (`JUCE Personal Tier` <$50k/yr revenue / `AGPL-3.0-only`).
5. **Rust Audio / FFI & Storage:** `tauri` 2.1, `rusqlite` 0.32 bundled SQLite, `tokio` 1.42, `serde` 1.0, `thiserror` 2.0 (`MIT` / `Apache-2.0` / `Blessing`).
6. **ML Inference Engine:** `ONNX Runtime` (`MIT`) with `CoreMLExecutionProvider` for Apple Neural Engine / GPU acceleration.
7. **Stem Separation & Structural Models:** `Demucs / htdemucs` (`MIT`, Meta AI Research ONNX export), in-house structural segmentation (`MIT OR Apache-2.0`).
8. **Metadata Parsing:** Local embedded ID3v2, Vorbis, MP4/AAC tags (zero remote scraping APIs).
9. **UI, Desktop Shell & Build Tooling:** React 18, Zustand 4, Lucide React, Vite, Vitest, ESLint, TypeScript, CMake, Apple Clang (`MIT` / `Apache-2.0` / `ISC` / `BSD-3-Clause` / `Apple-SDK`).

---

## 2. Automated Compliance Gate

All dependencies in the repository are verified at build and CI time via:
```bash
./scripts/audit_dependencies.sh
```

Refer to the primary registry at [DEPENDENCIES.md](file:///Users/yonatanzilberman/Documents/Pulse/DEPENDENCIES.md) for the complete version, SPDX, and linking matrix.
