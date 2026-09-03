#!/usr/bin/env bash
set -euo pipefail

# ==============================================================================
# PULSE — Production Dual-Deck Playback & Ahead-of-Time Preparation Verification
# ==============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

echo "========================================================================"
echo "🎛️  PULSE Dual-Deck Playback & Ahead-of-Time Preparation Verification"
echo "========================================================================"
echo "Repository Root: ${REPO_ROOT}"

# 1. Build C++ Engine and Verification Executables
echo -e "\n[1/3] Building C++ Audio Engine and Test Targets..."
cmake -B "${REPO_ROOT}/src-cpp/build" -S "${REPO_ROOT}/src-cpp"
cmake --build "${REPO_ROOT}/src-cpp/build"

# 2. Run Comprehensive Deterministic Dual-Deck Playback Test Suite
echo -e "\n[2/3] Running Deterministic Dual-Deck Playback Test Suite (8 Test Cases)..."
"${REPO_ROOT}/src-cpp/build/test_dual_deck_playback"

# 3. Run Manual Verification Routine with Audio Export & Telemetry Report
echo -e "\n[3/3] Running End-to-End Dual-Deck Playback Verification CLI..."
mkdir -p "${REPO_ROOT}/tests/audio"
"${REPO_ROOT}/src-cpp/build/pulse_cli" \
    --verify-dual-deck \
    --out "${REPO_ROOT}/tests/audio/manual_dual_deck_mix.wav" \
    --report "${REPO_ROOT}/tests/audio/manual_dual_deck_report.json" \
    "$@"

echo -e "\n========================================================================"
echo "🎉 ALL DUAL-DECK PLAYBACK AND PREPARATION QUALITY GATES PASSED!"
echo "========================================================================"
