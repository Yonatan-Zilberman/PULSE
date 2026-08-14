# PULSE
## Autonomous Local DJ Application

**Product Requirements Document**

| | |
|---|---|
| **Target platform (V1)** | macOS, Apple Silicon (M1–M5) |
| **Target platform (V2+)** | Cross-platform — Windows, Linux (Section 18) |
| **Architecture** | Local-first, fully offline-capable — Tauri 2 + React (Rust core) / JUCE (C++ audio engine) |
| **Cost model** | Free to acquire, free to run, forever — no subscription, no per-track or per-session cloud cost (Section 7) |
| **Status** | Draft for engineering kickoff |
| **Supersedes** | PULSE-DJ PRD v3.0 (platform/legal-verified draft) and PULSE PRD v3.0 (DJ-brain deep-design draft) |

**Merge notes:** This document combines two independently developed PRDs for the same product. One emphasized verified platform/legal facts (streaming rights, competitive landscape, realistic ML performance targets) and a compact MVP staging plan. The other developed a deeper DJ decision-making model (transition taxonomy, bass/vocal collision handling, "do-not-mix" logic, per-subsystem recovery, explainability). This draft keeps the strongest material from both and adds an explicit cost/licensing framework, since **the product must be free to use and free to build — no paid cloud APIs, no subscriptions, no recurring costs of any kind.** Where source documents disagreed, the resolution is marked **[Merge decision]**.

---

## 1. Product Vision & Core Principle

PULSE turns an ordinary local music library into a continuous, professionally mixed DJ set with minimal user interaction. The user selects music, a mood, or an energy level; PULSE handles track selection, ordering, beat sync, tempo matching, phrase alignment, harmonic compatibility, transition strategy, EQ/filter automation, vocal and bass management, and energy progression.

**Core principle: "The user chooses the music. PULSE DJs it."** The user should never need to understand BPM, beatgrids, Camelot notation, EQ, phrase structure, or transition types to get the core experience. Those concepts can be exposed in advanced controls, never required.

The objective is not to crossfade songs — it's to make the listener believe a competent human DJ is running the set. An automatic mixer asks *"how can I transition these two songs?"* PULSE asks *"how should I perform this entire set?"*

**Why this is buildable:** the individual technical bets — BPM/key detection, structural segmentation, on-device stem separation, automated beatmatched transitions — are proven in shipping products today (djay Pro/Neural Mix, Serato Stems, VirtualDJ). What's novel about PULSE is combining them into a **fully autonomous** experience running **entirely on the user's own machine at zero ongoing cost**, rather than a manual mixing tool with cloud-assisted features layered on top.

---

## 2. Product Principles

1. **The user chooses the music. PULSE DJs it.** No DJ vocabulary should ever be a prerequisite for the core experience.
2. **A DJ is more than a crossfade.** PULSE optimizes for timing, phrasing, energy, harmony, vocal/bass interaction, structure, and set-level progression — not just tempo matching.
3. **Do less when doing more would sound worse.** PULSE must be able to decide *"do not mix these songs"* and let a track finish cleanly rather than force a bad transition (Section 12.3).
4. **Safety beats cleverness.** If confidence is low, PULSE simplifies, delays, or falls back to a plain crossfade rather than risk an audible failure. The listener must never hear the system "break."
5. **Intelligence is separated from real-time audio.** All ML inference and planning happen ahead of time on background threads. The real-time audio engine only executes already-prepared decisions — it never blocks, allocates unpredictably, or waits on ML.
6. **Local-first and zero-cost are product features, not implementation details.** Audio, analysis, and library data never leave the device by default. There is no cloud inference, no metered API, and no subscription — for the user or for the people building it (Section 7).
7. **Explainable by default.** Every automated decision can show a plain-language reason. This builds user trust and makes the system debuggable during development.

---

## 3. Problem Statement

Current music applications optimize for playback, playlists, recommendations, or shuffle — none of them beatmatch or manage musical energy. DJ applications provide powerful mixing tools but assume a human operator. Cloud "AI DJ" features (e.g., Spotify AI DJ) add commentary or shuffle, not beatmatched mixing, and require an ongoing subscription plus a live network connection.

PULSE addresses a different problem: **how can someone have the experience of listening to a DJ set without personally being the DJ, using only music they already own, at no ongoing cost?**

---

## 4. Target Users & Personas

| Persona | Need | Why existing tools fall short |
|---|---|---|
| **Home party host** (primary) | Press play, mix runs itself all night, doesn't know how to DJ | Shuffle doesn't beatmatch or manage energy; djay/Serato assume active manual mixing |
| **Casual music fan** (primary) | "I have a playlist, but I want it to feel like a mix" | Streaming auto-DJ features require a subscription and a live connection |
| **Small venue / bar owner** (secondary) | Background music with no jarring track changes, no added subscription cost | Manual playlists have volume/tempo jumps; commercial auto-DJ radio is a subscription with per-venue licensing overhead |
| **Fitness / long-form listener** (secondary) | Continuous, energy-curve-aware background mix (warm-up → build → peak → cooldown) from their own library | Full control over source files needed for a coherent curve; cloud auto-DJ adds latency and cost |
| **Amateur DJ** (secondary) | Automated set construction, transition assistance, live backup | No mainstream tool offers full-set automation *and* a manual fallback in one free app |
| **Wedding / mobile DJ** (tertiary) | Reliable "autopilot" for breaks, rapid set prep | Existing auto-mix features (djay Automix, rekordbox AI) are secondary features inside paid performance-first tools |
| **Professional DJ** (tertiary) | Intelligent prep, automated warm-up, emergency automation | No tool treats automation as a first-class prep/performance aid rather than a gimmick |

*Recommend validating against 8–10 target-user interviews before Phase 4 UI work — no primary research has been done yet.*

---

## 5. Product Modes

| Mode | Description |
|---|---|
| **Full Auto DJ** | Default experience. User selects music, presses Start, PULSE handles everything. |
| **Guided DJ** | PULSE recommends next track, transition point, and strategy; user approves or modifies. |
| **Auto-Assist** | User DJs manually; PULSE handles selected sub-tasks (beatmatching, beatgrid correction, harmonic recommendations, vocal-collision warnings). |
| **Manual DJ** | Traditional dual decks, waveforms, beatgrids, EQ, filters, crossfader, cueing, loops, stems. |

**[Merge decision]** All four modes are the right long-term shape, but they don't all ship in V1. Manual DJ mode alone is a full DJ-software UI — building it well is its own multi-month project and would dilute focus on the thing nobody else does (full autonomy). **V1 ships Full Auto DJ only.** Guided, Manual, and Auto-Assist are V2 scope (Section 18), gated on Full Auto DJ working well first.

---

## 6. Competitive Landscape & Differentiation

| Product | Fully autonomous auto-DJ | Local-first / no subscription | Real-time on-device stems |
|---|---|---|---|
| **djay Pro (Neural Mix)** | Partial — Automix exists, core UX is manual | No | Yes (AudioShake-powered) |
| **Serato DJ Pro (Stems)** | No — manual performance tool | Subscription for Stems/DJ features | Yes |
| **rekordbox** | AI-assisted, not autonomous | Subscription tiers | Partial |
| **VirtualDJ** | Automix feature exists | Free tier available | Yes |
| **Spotify AI DJ** | Commentary + shuffle, not beatmatched mixing | Requires subscription | No |
| **DJ.Studio** | No — offline mix-prep DAW, not live | Desktop app | AI stems inside a timeline editor |
| **PULSE** | **Yes — core differentiator** | **Yes — zero subscription, zero cloud cost, ever** | Target: yes, phased in (Section 12.7) |

**Positioning:** the bar for stem-separation *quality* is set by AudioShake/Neural Mix — hard to beat as an indie build. PULSE should not lead with "better stems than djay." It should lead with **full autonomy** (nobody ships "never touch it" as the primary UX) and **genuinely free, fully local, no data leaving the device** as the wedge. Competitive claims should be re-validated against current products before launch rather than treated as permanently established facts.

---

## 7. Cost, Licensing & the "Completely Free" Commitment

This is a hard product constraint, not an aspiration: **PULSE must cost nothing to run, indefinitely, on hardware the user already owns.** This shapes engineering decisions throughout the document:

- **No cloud inference, no metered APIs.** All BPM/key/structure/energy analysis and all stem separation run on-device (CPU/Neural Engine). Nothing about the core experience requires a network connection.
- **No metadata API calls either.** Artwork, genre, and tag enrichment come from the audio file's own embedded tags (ID3/Vorbis/MP4 metadata), not from a third-party lookup service — this keeps the "zero outside API calls" rule absolute, not just applied to the ML pipeline.
- **No subscription, no license key, no account requirement** for the core local-file experience, ever.
- **Dependency licensing must not create a hidden bill.** Third-party libraries are selected for permissive or free licensing so the project never owes a per-seat or revenue-based license fee (see the Technical Design Document, Section 4, for the specific library-by-library analysis — this includes explicit decisions on JUCE's licensing tier, the time-stretch library, and the stem-separation model's license).
- **One unavoidable real-world cost exists outside the software itself:** Apple's $99/year Developer Program is required only for notarized distribution outside of local/dev builds. Running PULSE from source or as a locally-signed dev build carries no such cost. This is called out explicitly so it's never a surprise late in the project (Technical Design Document, Section 16).
- **Telemetry is opt-in only**, and off by default — there is no business model that depends on collecting usage data.

---

## 8. Goals & Success Metrics

**North Star: Autonomous Listening Time** — average uninterrupted minutes of playback without manual intervention. Not songs played — a session where the user never has to skip, correct, or manually intervene is the product working.

| Supporting metric | V1 Target |
|---|---|
| Skip rate | Track and minimize |
| Transition failure / fallback-mode rate | < 10% of transitions use simplified fallback |
| % of transitions rated positively by blind human evaluators | ≥ 70% indistinguishable from human-DJ mixes (Section 16) |
| Time from track import to "ready to mix" | < 2s/track local file, p95 < 5s |
| Continuous playback reliability | 12-hour soak test, zero audio dropouts/xruns |
| Resource footprint | < 250 MB RAM idle, < 15% CPU sustained on base M1 |
| % of supported tracks that analyze successfully | ≥ 99% |
| Session completion | % of sessions running 30 min / 1 hr / 2 hr / 4+ hr unattended |

---

## 9. Scope & MVP Staging

Do not build the full vision at once. Stage it so each MVP answers one specific question before investing further.

| Stage | Adds | Question it answers |
|---|---|---|
| **MVP 1** | Local files, two decks, BPM/beatgrid detection, basic structure detection, tempo matching, phrase-aware crossfades, EQ + bass-aware automation, loudness normalization, queue generation, energy scoring, basic transition selection. **No key detection, no stems.** | Can PULSE automatically mix 50 carefully selected songs convincingly? |
| **MVP 2** | Key detection, harmonic compatibility, vocal detection, better structure analysis, full transition-type library, do-not-mix logic, transition previews, confidence scoring, fallback logic | Can PULSE handle diverse playlists without sounding mechanical? |
| **MVP 3** | Stem separation, vocal handoffs, advanced transition strategies, energy-curve planning, multi-track lookahead, adaptive set planning | Can PULSE produce a set a competent DJ would plausibly have performed? |
| **V2** | Guided/Manual/Auto-Assist modes, user taste learning, embeddings, live adaptation, professional controls, recording/export, set history | Can PULSE grow from an autonomous appliance into a full DJ platform? |
| **V3 (platform expansion)** | Cross-platform build (Windows/Linux), external controller support | Can PULSE run free on hardware beyond Apple Silicon? |

This staging deliberately defers stem separation — the hardest and least predictable engineering piece — until after the core sequencing/transition logic is validated without it.

**MVP1 must support:** local audio, dual decks, BPM detection, beatgrid, phrase detection, loudness normalization, basic structure detection, tempo matching, EQ automation, basic energy scoring, intelligent queue, several transition strategies, fallback behavior, fully automatic playback.

**MVP1 does not require:** advanced stems, streaming integrations of any kind, cloud services, user taste learning, sophisticated embeddings, professional hardware integration.

---

## 10. Core User Experience

```
Open PULSE
    ↓
Select music sources (folder, library)
    ↓
Import/scan library — playback can start before analysis finishes
    ↓
Optional: choose energy / mood
    ↓
Press AUTO DJ
    ↓
PULSE analyzes in the background, prioritized (current → next candidates → queue → rest of library)
    ↓
PULSE builds an initial set plan and playback begins
    ↓
PULSE continuously plans future tracks and performs transitions
    ↓
PULSE adapts when the user intervenes (skip, lock, mood change)
```

---

## 11. Music Ingestion & Rights Strategy

**Architectural principle:** the system must resolve **playlist metadata → local or licensed audio**, never assume **streaming API → raw audio buffer.** Raw audio analysis only happens where PULSE has legitimate, permitted access to the actual audio data. This is also the only ingestion model consistent with the zero-cost, zero-outside-API-call requirement in Section 7.

| Phase | Capability | Basis |
|---|---|---|
| **V1 (ship)** | Local DRM-free files the user owns: MP3, WAV, FLAC, AAC, AIFF, M4A | User's own files — no third-party terms involved, no network dependency |
| **V1.x** | Playlist import as track-matching convenience only: read a streaming playlist's track/artist/album list, fuzzy-match against the user's local library, queue the matched local files. **No audio is ever streamed or extracted from a third-party service.** | Reads metadata the user already has access to; plays files the user already owns |
| **V2 (conditional, explicitly opt-in cost/effort)** | Apply to a licensed DJ-streaming partner program (e.g., Apple's "DJ with Apple Music") for non-owned catalog | Contingent on approval; treated as a business-development track with an unknown timeline, never an engineering deliverable with a fixed date. Any such integration that requires a paid API tier is out of scope for the "free" product and would need to be a clearly-labeled optional feature. |
| **Not planned** | Third-party streaming playback (e.g., Spotify) inside PULSE | No compliant, free path exists |

Any user-facing copy describing "matched local files" must mean the user's own already-owned files — never third-party ripped substitutes. State this explicitly in terms/support docs.

---

## 12. Feature Requirements

### 12.1 DJ Brain — Set Planning & Track Selection

The DJ Brain is the central differentiating component, responsible for: set planning, track selection, transition candidate generation and selection, energy management, real-time adaptation, and failure recovery. It is a decision system with multiple constraints and objectives — not a single weighted formula.

**Candidate scoring (MVP2/MVP3 target, full dimensionality):**

```
CandidateScore = tempo compatibility + harmonic compatibility + phrase compatibility
                + energy compatibility + structural compatibility + vocal compatibility
                + intro/outro compatibility + genre compatibility + mood compatibility
                + novelty + transition confidence
```

Weights are context-sensitive — e.g., vocal-collision risk weighs far more heavily when both candidate tracks have prominent vocals.

**[Merge decision — MVP1 baseline weights]** Full-dimension scoring is the MVP2/MVP3 target once phrase, structural, vocal, and mood features exist. MVP1 ships a concrete four-dimension baseline so there's something to engineer against from day one:

| Variable | Definition | MVP1 weight |
|---|---|---|
| `BPM_Diff_norm` | `\|BPM_A − BPM_B\| / BPM_A`, capped — beyond ±6% requires time-stretch, penalize sharply past that | 0.35 |
| `KeyDistance` | 0 = same/adjacent Camelot step, 1 = one step off, 2+ = clash *(MVP2, once key detection ships)* | 0.30 |
| `EnergyDelta` | Difference in computed track energy | 0.25 |
| `RepeatPenalty` | Penalizes replaying the same artist/track within a configurable window | 0.10 |

**Lookahead planning:** avoid greedy next-track selection. A track with the highest immediate compatibility score is not necessarily the best choice — PULSE should prefer a slightly-lower-scoring next track if it opens a much stronger path several tracks out. Evaluate multiple forward paths and choose the one that produces the strongest overall set. Minimum lookahead: 2 tracks. Preferred: 4–8 tracks.

**Track selection objectives the planner balances:** transition quality, energy curve, BPM progression, harmonic compatibility, structural compatibility, genre continuity, mood continuity, novelty, artist/track repetition, user preferences, future transition potential.

**Energy-curve mode:** the user can pick a target arc for the session (warm-up → peak → cooldown, or flat background energy); the sequencer optimizes track order against that curve using per-section energy profiles, not just pairwise compatibility.

### 12.2 Track Analysis / Music Intelligence

Every track is analyzed asynchronously and produces a persisted `TrackProfile` (full schema in the Technical Design Document, Section 5) covering: metadata, tempo & beatgrid (with confidence and support for non-constant tempo), harmonic analysis (key, Camelot, key-change points), structural segmentation (intro/verse/chorus/breakdown/build/drop/bridge/outro, each with confidence), phrase analysis (4/8/16/32/64-bar boundaries), a per-section energy profile (not a single number per track), loudness (integrated LUFS, short-term, peak, true peak, dynamic range), and low-level features (vocal/drum/bass density, danceability, mood, genre probability).

**Structural segmentation is explicitly the least mature piece of the pipeline** — automatic structure detection is an active research area and even strong models are imperfect on structurally unusual tracks. Every boundary carries a confidence score; low confidence falls back to simple energy-based phrase detection (every 8/16/32 bars) rather than silently mistiming a transition.

**Key compatibility is an input to the DJ Brain, not a hard constraint** — a musically strong transition with a key change can beat a boring same-key transition. Priority order: (1) strong harmonic match, (2) compatible adjacent Camelot key, (3) compatible relative key, (4) controlled pitch adjustment, (5) deliberate key change, (6) avoid.

**Mixability score:** a composite per-track score (intro quality, outro quality, phrase stability, vocal isolation feasibility, beat stability, tempo stability, transition-option count) letting the sequencer prefer easier-to-mix tracks at the right moments.

### 12.3 Transition Planning & Type Library

A transition is a structured `TransitionPlan`, computed ahead of the crossover point and then executed — not improvised live. For every candidate track pair, PULSE generates **multiple** possible strategies and scores each one, rather than assuming one algorithm fits every pair:

```
A → B candidates:
32-bar EQ blend          91%
16-bar vocal handoff     86%
Drop transition          94%
Instrumental bridge      89%
Hard cut                 61%
Do not mix                88%
```

| Type | When used |
|---|---|
| Classic EQ Blend | Default — bass swap with gradual fader movement |
| Long Blend | 32–64 bars, compatible tracks with room to breathe |
| Short Blend | 8–16 bars, tighter transitions |
| Filter Transition | HPF/LPF automation instead of a straight EQ swap |
| Vocal Handoff | End one vocal phrase before introducing the next, preventing overlap |
| Instrumental Bridge | Use an instrumental section to connect two vocal-heavy tracks |
| Percussion Blend | Introduce next track's drums while preserving current track's musical content |
| Drop Transition | Use a drop / beat-one impact as the handoff moment |
| Breakdown Transition | Use the breakdown of one track to introduce the next |
| Stem Transition | Selectively use vocal/drum/bass/other stems (Section 12.7) |
| Hard Cut | Intentional clean cut, used when a blend would be worse |
| **Finish-and-Start** | Let the outgoing track finish naturally and start the next cleanly, with no forced mixing at all |

**The "Do Not Mix" decision** is a first-class output of transition scoring, not an error state. Every candidate pair must be allowed to resolve to `DO_NOT_MIX`, with reasons such as poor beatgrid, excessive BPM difference, incompatible structure, vocal collision, insufficient intro/outro, or low overall confidence. This directly implements Principle 3 (Section 2): a clean Finish-and-Start is a valid, sometimes-correct output — not a failure of the system.

**Transition timing:** don't default to a fixed bar count. Generate candidate transition windows from song structure, available outro, next track's intro, phrase boundaries, vocal activity, energy, and confidence; score each window; pick the best.

### 12.4 Vocal Collision Prevention

Core requirement, not an enhancement. Unintelligible vocal overlap is one of the strongest indicators of a bad automatic mix and must be strongly penalized in the transition-confidence score. When both tracks have significant vocal activity, PULSE considers, in order of preference: (1) delaying the destination vocal entry, (2) reducing the outgoing vocal level, (3) using an instrumental section, (4) using stem isolation, (5) shortening the transition, (6) selecting a different transition type, (7) selecting a different destination track entirely.

### 12.5 Bass Collision Prevention

The system must prevent excessive simultaneous low-frequency energy from both tracks — a common cause of muddy, unprofessional-sounding automatic mixes even when BPM and beat alignment are correct. Bass handoff should be an intentional, sequenced swap (outgoing bass fades as incoming bass enters, never both at full energy simultaneously), not simply lowering both tracks' overall volume.

### 12.6 Loudness Management

Every track is analyzed for integrated LUFS, short-term loudness, peak, true peak, and dynamic range. Gain staging occurs before and during every transition so perceived loudness stays consistent without unnecessary compression. A final safety limiter prevents clipping. This exists because BPM/key matching alone cannot prevent audible level discontinuities between differently-mastered tracks.

### 12.7 Stem Processing (Optional Enhancement, MVP3)

Stem separation is an optional enhancement, not a requirement for every track — and it's the highest-engineering-risk component in the whole system, which is why MVP staging (Section 9) defers it. **Decision engine:** before generating stems, ask *"will stems materially improve this transition?"* If no, don't generate them. If yes, generate only what's required (vocal-suppression need → vocal stem only; bass-swap need → bass stem only). Stem generation runs entirely on-device and is precomputed/cached ahead of the transition window — never computed live inside it. If stem generation is unavailable or disabled, PULSE falls back to standard EQ-based crossfades without vocal isolation — never fails outright. Technical implementation, model licensing, and execution-provider verification are covered in the Technical Design Document, Section 10.

### 12.8 Confidence & Failure Recovery

Every automated decision carries a numeric confidence — beatgrid, key, structure, transition, vocal/bass-collision risk. Low-confidence decisions automatically trigger safer strategies. **Failure recovery is a first-class subsystem, not an afterthought.** If a transition becomes unsafe mid-playback, PULSE aborts gracefully via a defined fallback hierarchy:

1. Continue current track.
2. Simplify the transition (drop to a lighter transition type).
3. Use a standard crossfade.
4. Skip the destination track.
5. Select a backup destination track.
6. Emergency fade.

The listener should never hear the application "break." Per-subsystem fallback detail (beatgrid failure, key failure, structure failure, stem failure, audio-decode failure, etc.) is specified in the Technical Design Document, Section 11.

### 12.9 Queue, User Controls & DJ Style Presets

**Queue architecture** — current track, active transition, next track, backup track, future candidates — visible to the user:

```
NOW          Track A
TRANSITION   Track A → Track B — Classic EQ Blend — 92% confidence
NEXT         Track B
AFTER        Track C, Track D
```

**Simple mode controls:** Auto DJ on/off, energy, mood, genre, pace, transition intensity.

**Advanced mode controls:** BPM range, key compatibility strictness, transition length, stem usage, vocal-overlap tolerance, energy curve, genre crossover, novelty, track-repetition tolerance.

**DJ Style presets** (map to scoring-weight adjustments and transition-type preferences): Smooth (long transitions, minimal effects), Club (long, phrase-aligned, aggressive energy management), Radio (shorter transitions, strong vocal management), House (long blends, harmonic emphasis), Pop (short transitions, vocal-aware), Hip-Hop (beat-aware cuts, instrumental-bridge preference), Eclectic (higher tolerance for BPM/key/genre variance).

At any time the user can skip, go back, choose another track, lock a track/transition, or change energy/mood; PULSE rebuilds the future queue without disrupting current playback.

### 12.10 Explainability & Transition Preview

Every automated transition can show a plain-language reason, e.g. *"PULSE chose this transition because Track B has a 32-bar instrumental intro, matching tempo, compatible key, and lower vocal density."* This matters most during development, where it makes algorithmic behavior debuggable — a wrong decision with a visible reason is far cheaper to fix than a silent one. Users can also preview Track A → Track B without playing the full set, with **"Use this transition"** or **"Find a better one"** actions.

### 12.11 Library Analysis & Caching

Playback must not wait for the full library to finish analyzing. On import of, say, 1,000 tracks, playback begins immediately while analysis proceeds in priority order: (1) current track, (2) next queue candidates, (3) future queue, (4) remainder of library. Analysis results are persisted per track — a track should not require full re-analysis on every app launch.

### 12.12 User Taste Learning & Live Adaptation *(V2)*

Over time, learn from skips, replays, rejected/approved transitions, and preferred energy/genre/BPM/transition styles — improving without manual configuration, and entirely from on-device data (Section 7). Continuously evaluate listener interaction during playback and adapt the forward queue — e.g., if the user repeatedly skips high-energy tracks, the DJ Brain adjusts.

---

## 13. UI/UX

- Top bar: master volume/tempo, Auto DJ toggle, hardware acceleration status indicator.
- Dual waveform visualizer with beatgrid, cue points, transition-zone overlays.
- Central mixer: crossfader, 3-band EQ, filters, per-stem toggles (V2/Manual mode).
- Smart queue view (Section 12.9) with Camelot badges, BPM, match score, confidence indicators, transition-reason explainability.
- Energy-curve view: visual arc of planned session intensity, editable by dragging.
- Manual override controls: skip/lock/reorder without breaking the auto-sequencer.
- DJ style preset picker.

---

## 14. Non-Functional Requirements

- **Reliability:** 8-hour minimum / 12-hour target continuous playback soak test, zero audio-engine failures; defined recovery if a single track's analysis or stem cache fails (skip with notification, don't crash the session).
- **Performance:** audio thread strictly real-time priority, no ML/allocation on that thread. UI target 60 FPS, smooth waveform scrolling, low interaction latency. Inference benchmarked independently across M1 through M5 and multiple RAM configurations before locking specs.
- **Hardware:** minimum 8 GB unified memory, recommended 16 GB; graceful degradation on lower-memory systems.
- **Privacy:** local-first is a genuine selling point — analysis, audio, and library data stay on-device by default; no telemetry on libraries or listening habits without explicit opt-in; network access is unnecessary for local playback.
- **Cost:** no feature required for the core local-file experience may depend on a paid API, subscription, or account (Section 7).
- **Accessibility:** VoiceOver support, keyboard navigation, adequate contrast, accessible control labels, non-color-only status indicators.
- **Public performance licensing:** if marketed for commercial/venue use, documentation must clearly state that public performance licensing (PRO fees, etc.) is the venue's responsibility, independent of the software.

---

## 15. Risk Register

| Risk | Impact | Mitigation |
|---|---|---|
| Streaming-ingestion assumptions don't hold | High — was core to an earlier pitch | Ship V1 fully on local files; streaming partnerships are a BD track, not an engineering deliverable with a fixed date (Section 11) |
| Structural segmentation accuracy is inconsistent | Medium — bad transitions undermine the "sounds professional" promise | Confidence scoring + fallback to phrase-based timing (Section 12.2) |
| On-device ML export/quantization pitfalls silently fall back to CPU | Medium — could blow performance targets unnoticed | Explicit execution-provider verification in QA (Technical Design Document, Section 10) |
| Competing directly on stem-separation quality vs. djay/Serato | Medium — well-funded incumbents with a dedicated ML partner | Differentiate on full autonomy + zero-cost/local-first, not raw stem fidelity (Section 6) |
| A dependency's license quietly requires payment or GPL-style disclosure incompatible with distribution plans | High — directly violates the "completely free" mandate | Explicit license audit before adoption of any library (Section 7; Technical Design Document, Section 4) |
| Apple notarization cost ($99/yr) is discovered late and treated as a surprise | Low-Medium — real but small, one-time-per-year cost outside the app itself | Documented up front (Section 7); source/dev builds remain entirely free |
| Scope creep across the full DJ Brain vision before MVP1 validates the core loop | High — largest single risk to shipping at all | Hold the line on MVP staging (Section 9); no MVP2/3 feature starts before MVP1's 50-song test passes |

---

## 16. QA: Human Evaluation & Golden Test Set

**Human evaluation:** build a blind evaluation dataset. For each transition, collect ratings for timing, beat alignment, musicality, energy continuity, vocal interaction, and artifact quality. Evaluate entire sets for coherence, pacing, variety, and overall DJ quality. The system should be tuned against this dataset, not just automated confidence metrics.

**Golden test set:** a fixed collection of deliberately hard tracks/transitions every audio-engine release must pass — large BPM difference, incorrect beatgrid, live drums, tempo changes mid-track, vocal-heavy tracks, instrumental tracks, long/short/no intros, hard endings, long outros, key changes, genre changes, half-time and double-time tracks, quiet tracks, heavily compressed tracks, unusual structures.

---

## 17. Definition of Done

**Technical DoD (ready for public beta):**
- 8-hour continuous playback with zero audio-engine failures.
- ≥99% of supported tracks analyze successfully.
- Beatgrid confidence sufficient for reliable sync across the golden test set.
- No systematic vocal or bass collisions, no clipping.
- Automatic fallback handles failed transitions without user-visible breakage.
- Users can start playback without understanding any DJ terminology.
- Majority of transitions rated positively by blind human evaluators.
- No feature in the default build requires a network connection or incurs any cost.

**Product DoD:** a user can select a playlist, press Auto DJ, walk away, return 30–60 minutes later, hear a coherent continuous mix, experience transitions that feel intentional rather than algorithmic, and have no need to manually correct the DJ — having paid nothing to do so.

---

## 18. Release Roadmap

| Phase | Deliverables | Timeline |
|---|---|---|
| **0. Feasibility** | Command-line audio-engine prototype: decode two tracks, detect BPM, align beats, time-stretch, phrase-aware transition, EQ automation. Produce a 60-minute automated mix from 100–500 tracks. **Success gate: 20 consecutive transitions sound convincing to a human listener before proceeding.** | 4–6 weeks |
| **1. Audio Engine** | JUCE dual-deck playback, CoreAudio, time-stretch, EQ, filters, crossfade, master processing, stress tests | 4–6 weeks |
| **2. Music Intelligence** | BPM, beatgrid, key, phrase, structural segmentation, energy model, vocal/bass density, mixability score, loudness analysis, confidence scoring, caching | 6–8 weeks |
| **3. DJ Brain** (most important phase) | Candidate scoring, lookahead, transition-type selection, do-not-mix logic, transition confidence, failure-recovery hierarchy — this is MVP1/MVP2 territory | 6–10 weeks |
| **4. Product UX** | Tauri UI: Auto DJ, queue view, waveform, transition preview/explainability, settings, library, onboarding | 4–6 weeks |
| **5. Advanced Intelligence** | Stem separation (on-device model conversion + hardware EP verification + benchmarking), vocal handoff via stems, advanced transition strategies, adaptive sequencing — this is MVP3, deliberately last | 6–10 weeks, budget extra time for model-conversion risk |
| **6. V2 Platform** | Guided/Manual/Auto-Assist modes, user taste learning, live adaptation, professional controls, recording/export, set history | Begins after MVP3 validates |
| **7. Cross-Platform Expansion** | Port to Windows and Linux — swap CoreML for a portable inference backend, swap CoreAudio for WASAPI/ALSA. Tauri and JUCE were chosen in Phase 1 specifically because both already support this target, minimizing rework. | Begins once V1 (Mac) product is validated and stable, per product direction |

MVP1 ships at roughly Phase 0 + Phase 1 + partial Phase 2/3; MVP2 completes Phase 3; MVP3 completes Phase 5; V2 begins after; cross-platform expansion is an explicit later phase, not a V1 requirement.

---

## 19. Future Features (Post-V1)

Automatic set recording, generated/shareable DJ mixes, social sharing, collaborative sets, party mode, microphone/room-aware behavior, external DJ controller support, hardware integrations, live performance mode, cross-device queue control. None of these should block the initial product, and none should be built in a way that reintroduces a recurring cost.

---

## 20. North Star

The goal of PULSE is not *"automatically crossfade songs."* It is **"automatically perform a DJ set, for free, entirely on the user's own machine."** That distinction should drive every architecture and scope decision above — including which corners are acceptable to cut in V1 (Manual/Guided modes, stem separation, streaming catalogs) and which are not (autonomy, failure recovery, transition quality, and zero ongoing cost).

---

## Appendix A — Camelot Wheel Quick Reference

Standard Mixed In Key Camelot notation: keys are numbered 1–12 with an A (minor) or B (major) suffix. Compatible transitions: same number (relative major/minor), ±1 number same letter (adjacent fifth), or same number opposite letter. This is industry-standard notation, not proprietary to PULSE.

## Appendix B — Related Document

Implementation details (architecture, data schemas, algorithms, licensing analysis, benchmarking plan, and repository layout) live in the companion **PULSE Technical Design Document**.
