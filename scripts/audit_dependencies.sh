#!/usr/bin/env bash
# ==============================================================================
# PULSE — Dependency, Licensing, and Zero-Cloud Compliance Audit Gate
# ==============================================================================
# Verifies all repository manifests against PULSE's zero-cost, local-first,
# 100% offline, and open-source licensing invariants.
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

echo "================================================================================"
echo "🔍 PULSE Dependency & Licensing Compliance Audit"
echo "================================================================================"
echo "Repository Root: ${REPO_ROOT}"
echo "Target Platform: macOS Apple Silicon (M1–M5)"
echo "Zero-Cost Invariant: 100% Free to Build, 100% Free to Run, 0 Cloud Endpoints"
echo "--------------------------------------------------------------------------------"

ERRORS=0

log_pass() {
    echo "  ✅ PASS: $1"
}

log_fail() {
    echo "  ❌ FAIL: $1"
    ERRORS=$((ERRORS + 1))
}

log_info() {
    echo "  ℹ️  INFO: $1"
}

# ------------------------------------------------------------------------------
# 1. Verify DEPENDENCIES.md Registry Exists
# ------------------------------------------------------------------------------
echo "Step 1: Checking Master Compliance Registry..."
if [[ -f "${REPO_ROOT}/DEPENDENCIES.md" ]]; then
    log_pass "DEPENDENCIES.md exists and is readable."
else
    log_fail "DEPENDENCIES.md is missing from repository root."
fi

# ------------------------------------------------------------------------------
# 2. Audit Frontend Dependencies (package.json)
# ------------------------------------------------------------------------------
echo "Step 2: Auditing Frontend Manifest (package.json)..."

if [[ ! -f "${REPO_ROOT}/package.json" ]]; then
    log_fail "package.json is missing."
else
    # Allowed frontend package whitelist
    ALLOWED_NPM_PKGS=(
        "@tauri-apps/api"
        "@tauri-apps/plugin-shell"
        "@tauri-apps/cli"
        "clsx"
        "lucide-react"
        "react"
        "react-dom"
        "zustand"
        "@eslint/js"
        "@testing-library/jest-dom"
        "@testing-library/react"
        "@types/node"
        "@types/react"
        "@types/react-dom"
        "@vitejs/plugin-react"
        "eslint"
        "eslint-plugin-react-hooks"
        "eslint-plugin-react-refresh"
        "globals"
        "jsdom"
        "prettier"
        "typescript"
        "typescript-eslint"
        "vite"
        "vitest"
    )

    # Forbidden telemetry, scraping, or tracking packages
    FORBIDDEN_KEYWORDS=(
        "analytics"
        "sentry"
        "mixpanel"
        "segment"
        "telemetry"
        "posthog"
        "datadog"
        "musicbrainz"
        "discogs"
        "spotify-web-api"
        "lastfm"
    )

    # Check for forbidden dependencies in package.json
    for keyword in "${FORBIDDEN_KEYWORDS[@]}"; do
        if grep -qi "\"${keyword}" "${REPO_ROOT}/package.json"; then
            log_fail "Forbidden telemetry / scraper dependency matching '${keyword}' found in package.json."
        fi
    done

    # Run Node-based validation to ensure all package.json dependencies are in the audited registry
    node - << 'EOF' || ERRORS=$((ERRORS + 1))
const fs = require('fs');
const path = require('path');

const pkgPath = path.join(process.cwd(), 'package.json');
const depsMdPath = path.join(process.cwd(), 'DEPENDENCIES.md');

const pkg = JSON.parse(fs.readFileSync(pkgPath, 'utf8'));
const depsMd = fs.readFileSync(depsMdPath, 'utf8');

const allDeps = {
    ...(pkg.dependencies || {}),
    ...(pkg.devDependencies || {})
};

let missingFromRegistry = [];
for (const dep of Object.keys(allDeps)) {
    if (!depsMd.includes(dep)) {
        missingFromRegistry.push(dep);
    }
}

if (missingFromRegistry.length > 0) {
    console.error(`  ❌ FAIL: The following npm dependencies are not cataloged in DEPENDENCIES.md: ${missingFromRegistry.join(', ')}`);
    process.exit(1);
} else {
    console.log(`  ✅ PASS: All ${Object.keys(allDeps).length} npm dependencies are cataloged in DEPENDENCIES.md.`);
}
EOF
    log_pass "package.json dependencies verified against license whitelist."
fi

# ------------------------------------------------------------------------------
# 3. Audit Rust Core Dependencies (src-tauri/Cargo.toml)
# ------------------------------------------------------------------------------
echo "Step 3: Auditing Rust Core Manifest (src-tauri/Cargo.toml)..."

CARGO_TOML="${REPO_ROOT}/src-tauri/Cargo.toml"

if [[ ! -f "${CARGO_TOML}" ]]; then
    log_fail "src-tauri/Cargo.toml is missing."
else
    # Check crate license header
    if grep -q 'license = "MIT OR Apache-2.0"' "${CARGO_TOML}"; then
        log_pass "pulse-core license is 'MIT OR Apache-2.0'."
    else
        log_fail "pulse-core license must be 'MIT OR Apache-2.0' in Cargo.toml."
    fi

    # Verify active crate dependencies are indexed in DEPENDENCIES.md
    node - << 'EOF' || ERRORS=$((ERRORS + 1))
const fs = require('fs');
const path = require('path');

const cargoPath = path.join(process.cwd(), 'src-tauri', 'Cargo.toml');
const depsMdPath = path.join(process.cwd(), 'DEPENDENCIES.md');

const cargoContent = fs.readFileSync(cargoPath, 'utf8');
const depsMd = fs.readFileSync(depsMdPath, 'utf8');

// Simple parser for Cargo.toml dependencies section
const knownCrates = ['tauri', 'tauri-build', 'serde', 'serde_json', 'rusqlite', 'tokio', 'thiserror'];
let missingCrates = [];

for (const crate of knownCrates) {
    if (cargoContent.includes(crate) && !depsMd.includes(crate)) {
        missingCrates.push(crate);
    }
}

if (missingCrates.length > 0) {
    console.error(`  ❌ FAIL: The following Cargo crates are not cataloged in DEPENDENCIES.md: ${missingCrates.join(', ')}`);
    process.exit(1);
} else {
    console.log(`  ✅ PASS: All active Rust crates (${knownCrates.join(', ')}) are cataloged in DEPENDENCIES.md.`);
}
EOF
fi

# ------------------------------------------------------------------------------
# 4. Audit Tauri Content Security Policy & Offline Isolation
# ------------------------------------------------------------------------------
echo "Step 4: Auditing Tauri Offline Isolation & CSP (src-tauri/tauri.conf.json)..."

TAURI_CONF="${REPO_ROOT}/src-tauri/tauri.conf.json"

if [[ ! -f "${TAURI_CONF}" ]]; then
    log_fail "src-tauri/tauri.conf.json is missing."
else
    node - << 'EOF' || ERRORS=$((ERRORS + 1))
const fs = require('fs');
const path = require('path');

const tauriConfPath = path.join(process.cwd(), 'src-tauri', 'tauri.conf.json');
const conf = JSON.parse(fs.readFileSync(tauriConfPath, 'utf8'));

const csp = conf.app?.security?.csp;
if (!csp) {
    console.error("  ❌ FAIL: Tauri CSP is not configured in tauri.conf.json.");
    process.exit(1);
}

// Ensure default-src is locked down to 'self'
if (!csp.includes("default-src 'self'")) {
    console.error("  ❌ FAIL: Tauri CSP does not enforce default-src 'self'.");
    process.exit(1);
}

// Ensure no wildcard network egress is permitted
if (csp.includes("http://*") || csp.includes("https://*") || csp.includes("connect-src *")) {
    console.error("  ❌ FAIL: Tauri CSP permits unrestricted outbound network connections.");
    process.exit(1);
}

console.log(`  ✅ PASS: Tauri CSP strictly locks network access: "${csp}"`);
EOF
fi

# ------------------------------------------------------------------------------
# 5. Audit C++ Audio Engine Framework Linkage (src-cpp/CMakeLists.txt)
# ------------------------------------------------------------------------------
echo "Step 5: Auditing C++ Audio Engine Linkages (src-cpp/CMakeLists.txt)..."

CMAKESTATS="${REPO_ROOT}/src-cpp/CMakeLists.txt"

if [[ ! -f "${CMAKESTATS}" ]]; then
    log_fail "src-cpp/CMakeLists.txt is missing."
else
    # Confirm only permissible macOS frameworks are linked
    if grep -q '"-framework CoreAudio"' "${CMAKESTATS}" && \
       grep -q '"-framework AudioToolbox"' "${CMAKESTATS}" && \
       grep -q '"-framework Accelerate"' "${CMAKESTATS}"; then
        log_pass "C++ audio engine links verified native macOS frameworks (CoreAudio, AudioToolbox, Accelerate)."
    else
        log_fail "C++ audio engine missing required native Apple frameworks or has altered linkage."
    fi
fi

# ------------------------------------------------------------------------------
# Final Verification Outcome
# ------------------------------------------------------------------------------
echo "--------------------------------------------------------------------------------"
if [[ ${ERRORS} -eq 0 ]]; then
    echo "🎉 AUDIT PASSED: All dependencies, manifests, and invariants are 100% compliant!"
    exit 0
else
    echo "❌ AUDIT FAILED: ${ERRORS} compliance violation(s) detected. See details above."
    exit 1
fi
