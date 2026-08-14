# PULSE Phase 0 Feasibility Gate — Blind Human-Rating Rubric

**Version:** 1.0  
**Phase Target:** Phase 0 Feasibility Gate ([PRD Section 18](Docs/Pulse%20PRD.md))  
**Evaluation Scope:** 20 Consecutive Automated DJ Transitions  
**Mandate:** 100% Offline, Zero-Cloud, Blind Human Evaluation  

---

## 1. Objective & Gate Definition

According to the **PULSE Product Requirements Document (Section 18, Phase 0 Gate)**:
> *"Before advancing to Phase 1/2 production architecture, PULSE requires a validated feasibility milestone: executing a continuous automated mix from a corpus yielding **20 consecutive transitions that sound convincing to a human listener**."*

### Phase 0 Pass / Fail Gate Criteria
A set successfully passes the Phase 0 Feasibility Gate if and only if:
1. **Average Human Convincingness Score $\ge 4.0 / 5.0$** across all 20 consecutive transitions.
2. **Zero Catastrophic Failures:** 0 audible tempo trainwrecks (unresolved phase drift), 0 hard-clipping distortion events ($0\text{ dBFS}$ overshoots), and 0 jarring mid-bar structural amputations.
3. **100% Blind Evaluation:** Evaluators must evaluate blinded transition audio snippets without knowing track artist, title, or BPM metadata beforehand.

---

## 2. Five-Dimension Evaluation Criteria

Evaluators rate each transition on a standard **5-point Likert scale (1 to 5)** across five objective dimensions:

| Dimension | Weight | Description |
|---|:---:|---|
| **1. Beat & Tempo Coherence** | 25% | Phase synchronization, pitch invariance, and absence of audible flutter/flanging. |
| **2. Low-End / Bass Management** | 25% | Cleanliness of sub-bass handoff, absence of low-frequency buildup, mud, or volume drops. |
| **3. Phrase & Structural Placement** | 20% | Alignment with 4-bar / 8-bar musical boundaries (intros, drops, breakdowns, outros). |
| **4. Energy Flow & Dynamics** | 15% | Natural perceived energy progression between outgoing and incoming tracks. |
| **5. Overall Human Convincingness** | 15% | Subjective professional judgment: *Would a human listener believe a skilled DJ performed this?* |

---

## 3. Detailed Scoring Anchors

### Dimension 1: Beat & Tempo Coherence
- **5 (Flawless):** Locked in phase and tempo throughout the entire transition window. Transients are razor-sharp with zero audible drift, flanging, or WSOLA artifacts.
- **4 (Good):** Well beat-matched. Minor imperceptible phase micro-deviation that does not distract a casual listener.
- **3 (Acceptable):** Slight audible phase drift or minor transient softening, but tempo remains aligned without losing the dance pulse.
- **2 (Poor):** Noticeable drift / gallop ("horsing"), audible tempo friction between kicks.
- **1 (Catastrophic / Trainwreck):** Complete tempo mismatch or phase collision; kicks clash cacophonously.

### Dimension 2: Low-End & Bass Management
- **5 (Flawless):** Clean, punchy bass swap. Outgoing sub frequencies attenuate precisely as incoming bass establishes foundation, with zero low-end swelling ($< +0.5\text{ dB}$).
- **4 (Good):** Smooth bass handoff with negligible bass collision and clear kick clarity.
- **3 (Acceptable):** Mild low-end buildup or slight hollow dip during crossover, but no limiter pumping or distortion.
- **2 (Poor):** Obvious mud, sub-bass conflict, or noticeable volume dip at transition midpoint.
- **1 (Catastrophic / Distortion):** Severe low-frequency collision causing hard clipping, limiter distortion, or complete bass cancellation.

### Dimension 3: Phrase & Structural Placement
- **5 (Flawless):** Transition starts and resolves exactly on major 8-bar / 16-bar phrase boundaries (downbeats, intro kicks, drop arrivals).
- **4 (Good):** Transition aligns cleanly to a 4-bar phrase boundary with natural musical flow.
- **3 (Acceptable):** Transition snaps to a 2-bar or 1-bar downbeat; musically acceptable but slightly short or abrupt.
- **2 (Poor):** Transition cuts across odd bar boundaries or starts mid-phrase.
- **1 (Catastrophic / Amputation):** Transition cuts abruptly mid-bar (e.g. beat 2 or 3), jarring musical momentum.

### Dimension 4: Energy Flow & Dynamics
- **5 (Flawless):** Seamless energy continuity; energy either builds intentionally or maintains steady momentum.
- **4 (Good):** Smooth volume and frequency trajectory with consistent dancefloor presence.
- **3 (Acceptable):** Noticeable energy delta between tracks, but transition curves soften the change adequately.
- **2 (Poor):** Sudden perceived energy collapse or jarring jump.
- **1 (Catastrophic):** Total dynamic collapse or ear-fatiguing loudness spike.

### Dimension 5: Overall Human Convincingness
- **5 (Professional DJ Performance):** Flawless club-grade execution. Indistinguishable from a live set mixed by a top-tier resident DJ.
- **4 (Convincing / Club-Ready):** Sounds natural and musical. A listener on the dancefloor would enjoy the mix without second thought.
- **3 (Passable / Algorithmic):** Clean transition, but feels slightly mechanical or predictable.
- **2 (Unconvincing):** Audible flaws make it evident that an unguided script performed the mix.
- **1 (Reject):** Unlistenable transition that breaks the mix entirely.

---

## 4. Blind Human-Rating Protocol

1. **Setup:**
   - Open the standalone rating tool: [`tests/evaluation/blind_rating_tool.html`](tests/evaluation/blind_rating_tool.html) in any modern browser (Chrome, Safari, Firefox).
   - Load the set execution telemetry report: `tests/audio/phase0_set_report.json`.
2. **Blinding:**
   - The tool automatically blinds all track filenames, artists, and titles, displaying only "Transition 01" through "Transition 20".
3. **Evaluation Procedure:**
   - Listen to each transition snippet in sequence (`transition_01.wav` through `transition_20.wav`).
   - Replay specific sections using the interactive scrubber or loop feature.
   - Enter ratings (1–5) for all 5 dimensions.
   - Provide optional descriptive notes for any notable anomalies or highlights.
4. **Export & Verification:**
   - Click **"Export Evaluation Results JSON"** to download `evaluation_results.json`.
   - Verify that the aggregate convincingness mean satisfies the Phase 0 Gate ($\ge 4.0 / 5.0$).
