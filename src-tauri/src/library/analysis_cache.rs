// The public methods unwrap the lock guard; a poisoned mutex means the
// application is already unwinding, so panicking is the only sane option.
#![allow(clippy::missing_panics_doc)]
//! Persistent analysis cache: per-track, model-versioned analysis results.
//!
//! One wide `analysis_results` row per `track_id` (schema v2, same
//! `pulse-library.db` as the library index). Each of the five fixed stages
//! ([`AnalysisStage`]) has a `(state, version, payload)` column triplet;
//! payloads are serialized [`AnalysisStagePayload`] JSON. This cache holds
//! its **own** `Mutex<Connection>` on the database file, so the
//! single-writer-per-table invariant is structural: `AnalysisCache` can only
//! reach `analysis_results` — ingest (`LibraryCache`) never sees it.
//!
//! `rusqlite::Connection` is `!Sync`, so it is wrapped in a
//! `std::sync::Mutex` to make [`AnalysisCache`] `Send + Sync`.
//!
//! **Lock-splitting rule:** every public method locks the connection exactly
//! once and delegates to a private `*_locked(conn, …)` helper that assumes
//! the lock is already held. No public method may call another public method
//! on the same receiver (the `std` mutex is non-reentrant). One lock scope
//! per public call (no explicit `rusqlite::Transaction`: each helper runs
//! under autocommit, and its single write is idempotent and safe to re-run
//! after a crash).
//!
//! **Freshness at read time.** `lookup` derives each stage's
//! [`StageFreshness`] from the stored row plus two caller parameters
//! (`current_hash`, `availability`) with this precedence:
//!
//! 1. `availability == Missing` ⇒ every stage `MissingSource` (stored
//!    payloads are still returned; re-availability restores validity with no
//!    write).
//! 2. Else, stored hash and `current_hash` are both `Some` and differ ⇒
//!    every stage `Invalid` (payloads still returned, flagged).
//! 3. Else, per stored stage state: `not_started` ⇒ `NotStarted`,
//!    `in_progress` ⇒ `InProgress`, `failed` ⇒ `Failed` (no payload),
//!    `complete` + `NULL` payload ⇒ `Invalid`, `complete` + payload that
//!    fails to parse (or whose `"stage"` tag does not match the column) ⇒
//!    `Invalid`, `complete` + payload + stored version ==
//!    [`AnalysisStage::model_version`] ⇒ `Current`, and version mismatch
//!    (or `NULL`) ⇒ `Stale` (served, flagged).
//!
//! **Invalidation is a read-time computation, never a destructive write.**
//! There is no `DELETE` and no `mark_invalid` API: a content change or a
//! model-version bump invalidates at the next `lookup`, and re-hashing to
//! the original content (or a version re-rollout) restores validity without
//! any write.
//!
//! **Fail-soft corruption:** an unparseable or mis-tagged payload degrades
//! to that single stage's `Invalid` — it never fails the whole record and
//! never panics.

use std::fmt::Write;

use rusqlite::{params, Connection, OptionalExtension, Result};
use serde::{Deserialize, Serialize};

use super::errors::LibraryError;
use super::schema::migrate;
use super::types::{AnalysisState, Availability, UpsertOutcome};
use crate::models::track::{AnalysisStage, AnalysisStagePayload};

/// How fresh a stored stage result is, computed at [`AnalysisCache::lookup`]
/// time (see the module docs for the precedence rules).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum StageFreshness {
    /// The stage has not run yet.
    NotStarted,
    /// A run is recorded as in progress (a crashed run lands here).
    InProgress,
    /// The payload was produced by the current model version and the source
    /// file matches.
    Current,
    /// Served, but produced by a different model version (or `NULL`
    /// version) — the orchestrator should re-run.
    Stale,
    /// The stage's last run failed.
    Failed,
    /// The stored payload is corrupt, mis-tagged, or missing with a
    /// `complete` state.
    Invalid,
    /// The source file is missing; the stored payload is retained but must
    /// not be trusted until the file returns.
    MissingSource,
}

/// One stage of an [`AnalysisRecord`].
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct AnalysisStageEntry {
    pub stage: AnalysisStage,
    pub freshness: StageFreshness,
    /// The stored payload (deserialized fail-soft; `None` when absent or
    /// unusable). Served even when `freshness` is `Stale`/`Invalid`/
    /// `MissingSource`, so the orchestrator can fall back to it.
    pub payload: Option<AnalysisStagePayload>,
}

/// The full analysis state for one track, as seen by the orchestrator.
/// `current_hash` and `availability` come from the `tracks` row (caller
/// parameters) — this cache never reads `tracks`.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct AnalysisRecord {
    pub track_id: String,
    pub file_path: String,
    /// The hash the cached results were computed for.
    pub file_hash: Option<String>,
    /// The caller-provided filesystem presence for this lookup.
    pub availability: Availability,
    /// Always 5 entries, in [`AnalysisStage`] declaration order.
    pub stages: Vec<AnalysisStageEntry>,
}

pub struct AnalysisCache {
    conn: std::sync::Mutex<Connection>,
}

impl AnalysisCache {
    /// Open (or create) the database at `path` and migrate it to the current
    /// schema. Shares the file with the library index; a `busy_timeout`
    /// guards the two-connection file-DB case (in-memory paths are
    /// unaffected).
    pub fn open<P: AsRef<std::path::Path>>(path: P) -> Result<Self, LibraryError> {
        let conn = Connection::open(path)?;
        Self::from_connection(conn)
    }

    /// An in-memory database with the current schema (test path).
    pub fn in_memory() -> Result<Self, LibraryError> {
        Self::from_connection(Connection::open_in_memory()?)
    }

    fn from_connection(conn: Connection) -> Result<Self, LibraryError> {
        // The library index and this cache hold separate connections on the
        // same file DB; the timeout must be set *before* migrating: the
        // migration's write statements need SQLite's write lock, and with the
        // default timeout (0) they fail immediately if the other connection
        // is mid-transaction (e.g. a scan in progress).
        conn.pragma_update(None, "busy_timeout", 5000)?;
        migrate(&conn)?;
        Ok(Self {
            conn: std::sync::Mutex::new(conn),
        })
    }

    /// Test hook: access to the underlying connection (tests use it to
    /// simulate model-version bumps and payload corruption directly).
    #[cfg(test)]
    #[doc(hidden)]
    pub(crate) fn conn(&self) -> &std::sync::Mutex<Connection> {
        &self.conn
    }

    /// Store (or update) one stage's result for a track.
    ///
    /// `track_id` must reference an existing `tracks` row (the caller is
    /// expected to run ingest first). This is a caller expectation only —
    /// there is no foreign key (`PRAGMA foreign_keys` is set nowhere, so an
    /// FK would not even enforce across the two connections), and nothing in
    /// this cache deletes `analysis_results` rows: when ingest removes or
    /// re-ingests a track, the row orphans and accumulates unboundedly.
    /// The orchestrator milestone needs a deletion/GC policy for that. The stored model version is always
    /// [`AnalysisStage::model_version`] — callers cannot supply one, so a
    /// rollout bump happens in exactly one place. The outcome compares the
    /// incoming `(state, version, payload, file_path, file_hash)` against
    /// the stored row; `Unchanged` short-circuits with no write.
    ///
    /// State/payload consistency: `Complete` requires `Some(payload)`;
    /// `Failed`/`InProgress`/`NotStarted` require `None`.
    pub fn upsert_stage(
        &self,
        track_id: &str,
        file_path: &str,
        file_hash: Option<&str>,
        stage: AnalysisStage,
        state: AnalysisState,
        payload: Option<&AnalysisStagePayload>,
    ) -> Result<UpsertOutcome, LibraryError> {
        let payload_json = match (state, payload) {
            (AnalysisState::Complete, Some(p)) => Some(
                serde_json::to_string(p)
                    .map_err(|e| LibraryError::Db(format!("stage payload serialization: {e}")))?,
            ),
            (AnalysisState::Complete, None) => {
                return Err(LibraryError::Db(format!(
                    "complete {} results require a payload",
                    stage.as_str()
                )));
            }
            (_, Some(_)) => {
                return Err(LibraryError::Db(format!(
                    "{} {} results cannot carry a payload",
                    state.as_str(),
                    stage.as_str()
                )));
            }
            (_, None) => None,
        };
        let guard = self.conn.lock().unwrap();
        upsert_stage_locked(
            &guard,
            track_id,
            file_path,
            file_hash,
            stage,
            state,
            payload_json.as_deref(),
        )
    }

    /// Read one track's full analysis state with per-stage freshness.
    ///
    /// Unknown `track_id` ⇒ `Ok(None)` (a true miss). `current_hash` and
    /// `availability` are caller parameters from the `tracks` row (see the
    /// module docs for the precedence rules); the stored row is never
    /// mutated.
    pub fn lookup(
        &self,
        track_id: &str,
        current_hash: Option<&str>,
        availability: Availability,
    ) -> Result<Option<AnalysisRecord>, LibraryError> {
        let guard = self.conn.lock().unwrap();
        lookup_locked(&guard, track_id, current_hash, availability)
    }
}

// ----------------------------------------------------------------------
// Locked (lock already held) helpers
// ----------------------------------------------------------------------

/// One stored stage triplet plus the file-identity columns.
struct StoredStageState {
    file_path: String,
    file_hash: Option<String>,
    state: String,
    version: Option<String>,
    payload: Option<String>,
}

/// A stored `analysis_results` row; the stage triplets are in
/// [`AnalysisStage`] declaration order.
struct StoredAnalysisRow {
    track_id: String,
    file_path: String,
    file_hash: Option<String>,
    stages: Vec<(String, Option<String>, Option<String>)>,
}

fn upsert_stage_locked(
    conn: &Connection,
    track_id: &str,
    file_path: &str,
    file_hash: Option<&str>,
    stage: AnalysisStage,
    state: AnalysisState,
    payload: Option<&str>,
) -> Result<UpsertOutcome, LibraryError> {
    let s = stage.as_str();
    let state_col = format!("{s}_state");
    let version_col = format!("{s}_version");
    let payload_col = format!("{s}_payload");

    let existing: Option<StoredStageState> = conn
        .query_row(
            &format!(
                "SELECT file_path, file_hash, {state_col}, {version_col}, {payload_col}
                 FROM analysis_results WHERE track_id = ?1"
            ),
            [track_id],
            |row| {
                Ok(StoredStageState {
                    file_path: row.get(0)?,
                    file_hash: row.get(1)?,
                    state: row.get(2)?,
                    version: row.get(3)?,
                    payload: row.get(4)?,
                })
            },
        )
        .optional()?;

    let outcome = match &existing {
        None => UpsertOutcome::Inserted,
        Some(stored) => {
            if stored.file_path == file_path
                && stored.file_hash.as_deref() == file_hash
                && stored.state == state.as_str()
                && stored.version.as_deref() == Some(stage.model_version())
                && stored.payload.as_deref() == payload
            {
                UpsertOutcome::Unchanged
            } else {
                UpsertOutcome::Changed
            }
        }
    };
    if outcome == UpsertOutcome::Unchanged {
        return Ok(outcome);
    }

    // `track_id` is UNIQUE and the lock is held, so a conflict is always
    // this track's own row; the update touches file identity + exactly the
    // one stage triplet, never other stages' columns.
    conn.execute(
        &format!(
            "INSERT INTO analysis_results
                (id, track_id, file_path, file_hash, {state_col}, {version_col}, {payload_col})
             VALUES (lower(hex(randomblob(16))), ?1, ?2, ?3, ?4, ?5, ?6)
             ON CONFLICT(track_id) DO UPDATE SET
                file_path = excluded.file_path,
                file_hash = excluded.file_hash,
                {state_col} = excluded.{state_col},
                {version_col} = excluded.{version_col},
                {payload_col} = excluded.{payload_col},
                updated_at = CURRENT_TIMESTAMP"
        ),
        params![
            track_id,
            file_path,
            file_hash,
            state.as_str(),
            stage.model_version(),
            payload
        ],
    )?;
    Ok(outcome)
}

fn lookup_locked(
    conn: &Connection,
    track_id: &str,
    current_hash: Option<&str>,
    availability: Availability,
) -> Result<Option<AnalysisRecord>, LibraryError> {
    let mut columns = String::from("track_id, file_path, file_hash");
    for stage in AnalysisStage::all() {
        let s = stage.as_str();
        let _ = write!(columns, ", {s}_state, {s}_version, {s}_payload");
    }
    let sql = format!("SELECT {columns} FROM analysis_results WHERE track_id = ?1");

    let row: Option<StoredAnalysisRow> = conn
        .query_row(&sql, [track_id], |row| {
            let mut stages = Vec::with_capacity(AnalysisStage::all().len());
            for (offset, _) in AnalysisStage::all().iter().enumerate() {
                stages.push((
                    row.get(3 + 3 * offset)?,
                    row.get(4 + 3 * offset)?,
                    row.get(5 + 3 * offset)?,
                ));
            }
            Ok(StoredAnalysisRow {
                track_id: row.get(0)?,
                file_path: row.get(1)?,
                file_hash: row.get(2)?,
                stages,
            })
        })
        .optional()?;

    let Some(row) = row else {
        return Ok(None);
    };

    // Rule 2: content changed since the results were computed.
    let hash_invalid = matches!(
        (row.file_hash.as_deref(), current_hash),
        (Some(a), Some(b)) if a != b
    );
    let missing = availability == Availability::Missing;

    let stages = row
        .stages
        .iter()
        .zip(AnalysisStage::all())
        .map(|((state, version, payload), stage)| {
            let (freshness, deserialized) = stage_freshness(
                stage,
                state,
                version.as_deref(),
                payload.as_deref(),
                missing,
                hash_invalid,
            );
            AnalysisStageEntry {
                stage,
                freshness,
                payload: deserialized,
            }
        })
        .collect();

    Ok(Some(AnalysisRecord {
        track_id: row.track_id,
        file_path: row.file_path,
        file_hash: row.file_hash,
        availability,
        stages,
    }))
}

/// Apply the module-doc precedence rules to one stored stage.
fn stage_freshness(
    stage: AnalysisStage,
    state: &str,
    version: Option<&str>,
    payload_json: Option<&str>,
    missing: bool,
    hash_invalid: bool,
) -> (StageFreshness, Option<AnalysisStagePayload>) {
    let stored = deserialize_payload(stage, payload_json);
    if missing {
        return (StageFreshness::MissingSource, stored);
    }
    if hash_invalid {
        return (StageFreshness::Invalid, stored);
    }
    match state {
        "not_started" => (StageFreshness::NotStarted, None),
        "in_progress" => (StageFreshness::InProgress, None),
        "failed" => (StageFreshness::Failed, None),
        "complete" => {
            let Some(payload) = stored else {
                // `complete` with an absent/unparseable/mis-tagged payload.
                return (StageFreshness::Invalid, None);
            };
            if version == Some(stage.model_version()) {
                (StageFreshness::Current, Some(payload))
            } else {
                (StageFreshness::Stale, Some(payload))
            }
        }
        // The CHECK constraint makes this unreachable; fail soft regardless.
        _ => (StageFreshness::Invalid, None),
    }
}

/// Parse a stored payload fail-soft: garbage JSON or a `"stage"` tag that
/// does not match the column degrades to `None` (stage-local, never a
/// record failure, never a panic).
fn deserialize_payload(stage: AnalysisStage, json: Option<&str>) -> Option<AnalysisStagePayload> {
    let json = json?;
    let payload: AnalysisStagePayload = serde_json::from_str(json).ok()?;
    (payload.stage() == stage).then_some(payload)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::models::track::{
        KeyProfile, LoudnessProfile, MixabilityProfile, PhraseBoundaries, SegmentType,
        StructureSegment, TempoProfile,
    };

    const TRACK_ID: &str = "trk-1";
    const PATH: &str = "/music/test-track.flac";
    const HASH_A: &str = "hash-a";

    fn fixture_payload(stage: AnalysisStage) -> AnalysisStagePayload {
        match stage {
            AnalysisStage::Tempo => AnalysisStagePayload::Tempo(TempoProfile {
                bpm: 120.0,
                bpm_confidence: 0.95,
                alternative_bpm_hypotheses: vec![60.0],
                beat_positions: vec![0.0, 0.5],
                downbeat_positions: vec![0.0],
                bar_positions: vec![0.0],
                grid_offset_seconds: 0.0,
                is_variable_tempo: false,
                tempo_drift_min_bpm: 120.0,
                tempo_drift_max_bpm: 120.0,
            }),
            AnalysisStage::Key => AnalysisStagePayload::Key(KeyProfile {
                key: "A Minor".to_string(),
                camelot: "8A".to_string(),
                key_confidence: 0.9,
                chroma_profile: vec![0.1; 12],
            }),
            AnalysisStage::Structure => {
                AnalysisStagePayload::Structure(crate::models::track::StructureStageResult {
                    segments: vec![StructureSegment {
                        segment_type: SegmentType::Drop,
                        start_seconds: 30.0,
                        end_seconds: 90.0,
                        confidence: 0.9,
                        energy: 0.9,
                        vocal_density: 0.4,
                        instrumental_density: 0.8,
                    }],
                    phrases: PhraseBoundaries {
                        boundaries_4bar: vec![0.0, 8.0],
                        boundaries_8bar: vec![0.0, 16.0],
                        boundaries_16bar: vec![0.0, 32.0],
                        boundaries_32bar: vec![0.0, 64.0],
                    },
                    energy_curve: vec![0.3, 0.9, 0.5],
                })
            }
            AnalysisStage::Loudness => AnalysisStagePayload::Loudness(LoudnessProfile {
                integrated_lufs: -11.0,
                short_term_lufs_max: -8.0,
                true_peak_db: -1.0,
                dynamic_range_lu: 5.0,
            }),
            AnalysisStage::Mixability => AnalysisStagePayload::Mixability(MixabilityProfile {
                intro_quality: 0.9,
                outro_quality: 0.8,
                phrase_stability: 0.95,
                vocal_isolation_feasibility: 0.85,
                beat_stability: 0.99,
                tempo_stability: 0.99,
                transition_option_count: 10,
            }),
        }
    }

    /// Store all five stages complete with the fixed fixtures.
    fn store_all(cache: &AnalysisCache, hash: Option<&str>) {
        // One row per track: the first stage inserts it, the rest change it.
        for (index, stage) in AnalysisStage::all().iter().enumerate() {
            let payload = fixture_payload(*stage);
            let expected = if index == 0 {
                UpsertOutcome::Inserted
            } else {
                UpsertOutcome::Changed
            };
            assert_eq!(
                cache
                    .upsert_stage(
                        TRACK_ID,
                        PATH,
                        hash,
                        *stage,
                        AnalysisState::Complete,
                        Some(&payload)
                    )
                    .unwrap(),
                expected
            );
        }
    }

    fn entry(record: &AnalysisRecord, stage: AnalysisStage) -> &AnalysisStageEntry {
        record
            .stages
            .iter()
            .find(|e| e.stage == stage)
            .expect("every stage must be present")
    }

    #[test]
    fn test_insert_read_roundtrip() {
        let cache = AnalysisCache::in_memory().unwrap();
        store_all(&cache, Some(HASH_A));
        let saved = fixture_payload(AnalysisStage::Tempo);

        let record = cache
            .lookup(TRACK_ID, Some(HASH_A), Availability::Available)
            .unwrap()
            .expect("a stored track must be found");
        assert_eq!(record.track_id, TRACK_ID);
        assert_eq!(record.file_path, PATH);
        assert_eq!(record.file_hash.as_deref(), Some(HASH_A));
        assert_eq!(record.stages.len(), 5, "always 5 entries");
        for stage in AnalysisStage::all() {
            let e = entry(&record, stage);
            assert_eq!(
                e.freshness,
                StageFreshness::Current,
                "{} must be current",
                stage.as_str()
            );
            assert_eq!(
                e.payload,
                Some(fixture_payload(stage)),
                "payload must round-trip for {}",
                stage.as_str()
            );
        }
        assert_eq!(entry(&record, AnalysisStage::Tempo).payload, Some(saved));
    }

    #[test]
    fn test_update_and_idempotent_reshape() {
        let cache = AnalysisCache::in_memory().unwrap();
        let tempo = TempoProfile {
            bpm: 120.0,
            bpm_confidence: 0.95,
            alternative_bpm_hypotheses: vec![60.0],
            beat_positions: vec![0.0, 0.5],
            downbeat_positions: vec![0.0],
            bar_positions: vec![0.0],
            grid_offset_seconds: 0.0,
            is_variable_tempo: false,
            tempo_drift_min_bpm: 120.0,
            tempo_drift_max_bpm: 120.0,
        };
        let payload = AnalysisStagePayload::Tempo(tempo.clone());
        cache
            .upsert_stage(
                TRACK_ID,
                PATH,
                Some(HASH_A),
                AnalysisStage::Tempo,
                AnalysisState::Complete,
                Some(&payload),
            )
            .unwrap();

        // A changed payload is a `Changed` write and is read back.
        let mut new_tempo = tempo.clone();
        new_tempo.bpm = 128.0;
        let new_payload = AnalysisStagePayload::Tempo(new_tempo.clone());
        assert_eq!(
            cache
                .upsert_stage(
                    TRACK_ID,
                    PATH,
                    Some(HASH_A),
                    AnalysisStage::Tempo,
                    AnalysisState::Complete,
                    Some(&new_payload)
                )
                .unwrap(),
            UpsertOutcome::Changed
        );
        let record = cache
            .lookup(TRACK_ID, Some(HASH_A), Availability::Available)
            .unwrap()
            .unwrap();
        match entry(&record, AnalysisStage::Tempo)
            .payload
            .as_ref()
            .unwrap()
        {
            AnalysisStagePayload::Tempo(t) => assert!(t.bpm > 127.9 && t.bpm < 128.1),
            _ => panic!("wrong payload variant"),
        }

        // Identical repeat: `Unchanged`, no write, stored bytes untouched.
        let stored_before: Option<String> = cache
            .conn()
            .lock()
            .unwrap()
            .query_row(
                "SELECT tempo_payload FROM analysis_results WHERE track_id = ?1",
                [TRACK_ID],
                |r| r.get(0),
            )
            .unwrap();
        assert_eq!(
            cache
                .upsert_stage(
                    TRACK_ID,
                    PATH,
                    Some(HASH_A),
                    AnalysisStage::Tempo,
                    AnalysisState::Complete,
                    Some(&new_payload)
                )
                .unwrap(),
            UpsertOutcome::Unchanged
        );
        let stored_after: Option<String> = cache
            .conn()
            .lock()
            .unwrap()
            .query_row(
                "SELECT tempo_payload FROM analysis_results WHERE track_id = ?1",
                [TRACK_ID],
                |r| r.get(0),
            )
            .unwrap();
        assert_eq!(stored_before, stored_after, "Unchanged must not write");

        // A file-hash change with an identical payload is still a change.
        assert_eq!(
            cache
                .upsert_stage(
                    TRACK_ID,
                    PATH,
                    Some("hash-b"),
                    AnalysisStage::Tempo,
                    AnalysisState::Complete,
                    Some(&new_payload)
                )
                .unwrap(),
            UpsertOutcome::Changed
        );
    }

    #[test]
    fn test_hash_invalidation_and_restore() {
        let cache = AnalysisCache::in_memory().unwrap();
        store_all(&cache, Some(HASH_A));

        let current = |cache: &AnalysisCache, hash: Option<&str>| {
            let record = cache
                .lookup(TRACK_ID, hash, Availability::Available)
                .unwrap()
                .unwrap();
            record
                .stages
                .iter()
                .map(|e| e.freshness)
                .collect::<Vec<_>>()
        };
        assert!(current(&cache, Some(HASH_A))
            .iter()
            .all(|f| *f == StageFreshness::Current));
        // Content changed: every stage invalid.
        assert!(current(&cache, Some("hash-b"))
            .iter()
            .all(|f| *f == StageFreshness::Invalid));
        // Re-hash to the original content: validity restored, no write
        // happened, the row is retained.
        assert!(current(&cache, Some(HASH_A))
            .iter()
            .all(|f| *f == StageFreshness::Current));

        // A `NULL` stored hash never invalidates (rule 2 needs both `Some`).
        let cache2 = AnalysisCache::in_memory().unwrap();
        let payload = fixture_payload(AnalysisStage::Tempo);
        cache2
            .upsert_stage(
                TRACK_ID,
                PATH,
                None,
                AnalysisStage::Tempo,
                AnalysisState::Complete,
                Some(&payload),
            )
            .unwrap();
        let record = cache2
            .lookup(TRACK_ID, Some("anything"), Availability::Available)
            .unwrap()
            .unwrap();
        assert_eq!(
            entry(&record, AnalysisStage::Tempo).freshness,
            StageFreshness::Current
        );
    }

    #[test]
    fn test_version_bump_is_per_stage() {
        let cache = AnalysisCache::in_memory().unwrap();
        store_all(&cache, Some(HASH_A));
        cache
            .conn()
            .lock()
            .unwrap()
            .execute(
                "UPDATE analysis_results SET key_version = '9.9.9' WHERE track_id = ?1",
                [TRACK_ID],
            )
            .unwrap();

        let record = cache
            .lookup(TRACK_ID, Some(HASH_A), Availability::Available)
            .unwrap()
            .unwrap();
        assert_eq!(
            entry(&record, AnalysisStage::Key).freshness,
            StageFreshness::Stale,
            "a version bump must flip only that stage"
        );
        for stage in [
            AnalysisStage::Tempo,
            AnalysisStage::Structure,
            AnalysisStage::Loudness,
            AnalysisStage::Mixability,
        ] {
            assert_eq!(
                entry(&record, stage).freshness,
                StageFreshness::Current,
                "siblings must stay current"
            );
        }
    }

    #[test]
    fn test_stale_serving() {
        let cache = AnalysisCache::in_memory().unwrap();
        let loudness = fixture_payload(AnalysisStage::Loudness);
        cache
            .upsert_stage(
                TRACK_ID,
                PATH,
                Some(HASH_A),
                AnalysisStage::Loudness,
                AnalysisState::Complete,
                Some(&loudness),
            )
            .unwrap();
        // `NULL` stored version ⇒ `Stale` (same serving behavior).
        cache
            .conn()
            .lock()
            .unwrap()
            .execute(
                "UPDATE analysis_results SET loudness_version = NULL WHERE track_id = ?1",
                [TRACK_ID],
            )
            .unwrap();

        let record = cache
            .lookup(TRACK_ID, Some(HASH_A), Availability::Available)
            .unwrap()
            .unwrap();
        let stale = entry(&record, AnalysisStage::Loudness);
        assert_eq!(stale.freshness, StageFreshness::Stale);
        assert_eq!(
            stale.payload,
            Some(loudness),
            "a stale entry must still serve its stored payload"
        );
    }

    #[test]
    fn test_partial_results_mixed_status() {
        let cache = AnalysisCache::in_memory().unwrap();
        let tempo = fixture_payload(AnalysisStage::Tempo);
        cache
            .upsert_stage(
                TRACK_ID,
                PATH,
                Some(HASH_A),
                AnalysisStage::Tempo,
                AnalysisState::Complete,
                Some(&tempo),
            )
            .unwrap();
        // The row already exists (the tempo upsert created it): `Changed`.
        assert_eq!(
            cache
                .upsert_stage(
                    TRACK_ID,
                    PATH,
                    Some(HASH_A),
                    AnalysisStage::Key,
                    AnalysisState::Failed,
                    None
                )
                .unwrap(),
            UpsertOutcome::Changed
        );

        let record = cache
            .lookup(TRACK_ID, Some(HASH_A), Availability::Available)
            .unwrap()
            .unwrap();
        let key = entry(&record, AnalysisStage::Key);
        assert_eq!(key.freshness, StageFreshness::Failed);
        assert_eq!(key.payload, None, "a failed stage carries no payload");
        assert_eq!(
            entry(&record, AnalysisStage::Tempo).freshness,
            StageFreshness::Current
        );
        // Siblings that never ran stay `NotStarted`.
        assert_eq!(
            entry(&record, AnalysisStage::Loudness).freshness,
            StageFreshness::NotStarted
        );

        // State/payload consistency rule: `failed` + `Some` is rejected
        // (and `complete` + `None` is symmetrically rejected).
        assert!(cache
            .upsert_stage(
                TRACK_ID,
                PATH,
                Some(HASH_A),
                AnalysisStage::Key,
                AnalysisState::Failed,
                Some(&tempo)
            )
            .is_err());
        assert!(cache
            .upsert_stage(
                TRACK_ID,
                PATH,
                Some(HASH_A),
                AnalysisStage::Key,
                AnalysisState::Complete,
                None
            )
            .is_err());
    }

    #[test]
    fn test_missing_source() {
        let cache = AnalysisCache::in_memory().unwrap();
        store_all(&cache, Some(HASH_A));

        let record = cache
            .lookup(TRACK_ID, Some(HASH_A), Availability::Missing)
            .unwrap()
            .unwrap();
        assert!(record
            .stages
            .iter()
            .all(|e| e.freshness == StageFreshness::MissingSource));

        // The row is retained: re-availability restores `Current` with no
        // write, payloads included.
        let record = cache
            .lookup(TRACK_ID, Some(HASH_A), Availability::Available)
            .unwrap()
            .unwrap();
        assert!(record
            .stages
            .iter()
            .all(|e| e.freshness == StageFreshness::Current));
        let count: i64 = cache
            .conn()
            .lock()
            .unwrap()
            .query_row(
                "SELECT COUNT(*) FROM analysis_results WHERE track_id = ?1",
                [TRACK_ID],
                |r| r.get(0),
            )
            .unwrap();
        assert_eq!(count, 1, "the row must never be deleted");
    }

    #[test]
    fn test_corruption_is_stage_local() {
        let cache = AnalysisCache::in_memory().unwrap();
        store_all(&cache, Some(HASH_A));

        // Garbage in one stage's payload: that stage is `Invalid` with no
        // payload; the record and every sibling stage survive.
        cache
            .conn()
            .lock()
            .unwrap()
            .execute(
                "UPDATE analysis_results SET tempo_payload = 'garbage' WHERE track_id = ?1",
                [TRACK_ID],
            )
            .unwrap();
        let record = cache
            .lookup(TRACK_ID, Some(HASH_A), Availability::Available)
            .unwrap()
            .unwrap();
        let tempo = entry(&record, AnalysisStage::Tempo);
        assert_eq!(tempo.freshness, StageFreshness::Invalid);
        assert_eq!(tempo.payload, None);
        for stage in [
            AnalysisStage::Key,
            AnalysisStage::Structure,
            AnalysisStage::Loudness,
            AnalysisStage::Mixability,
        ] {
            assert_eq!(
                entry(&record, stage).freshness,
                StageFreshness::Current,
                "corruption must be stage-local"
            );
        }

        // Valid JSON with the wrong `"stage"` tag is `Invalid` too.
        let other_tag = fixture_payload(AnalysisStage::Key);
        let json = serde_json::to_string(&other_tag).unwrap();
        cache
            .conn()
            .lock()
            .unwrap()
            .execute(
                "UPDATE analysis_results SET tempo_payload = ?1 WHERE track_id = ?2",
                (json, TRACK_ID),
            )
            .unwrap();
        let record = cache
            .lookup(TRACK_ID, Some(HASH_A), Availability::Available)
            .unwrap()
            .unwrap();
        let tempo = entry(&record, AnalysisStage::Tempo);
        assert_eq!(tempo.freshness, StageFreshness::Invalid);
        assert_eq!(tempo.payload, None, "a mis-tagged payload is unusable");

        // `complete` with a `NULL` payload is `Invalid`.
        cache
            .conn()
            .lock()
            .unwrap()
            .execute(
                "UPDATE analysis_results SET tempo_payload = NULL WHERE track_id = ?1",
                [TRACK_ID],
            )
            .unwrap();
        let record = cache
            .lookup(TRACK_ID, Some(HASH_A), Availability::Available)
            .unwrap()
            .unwrap();
        let tempo = entry(&record, AnalysisStage::Tempo);
        assert_eq!(tempo.freshness, StageFreshness::Invalid);
        assert_eq!(tempo.payload, None);
    }

    #[test]
    fn test_lookup_unknown_track() {
        let cache = AnalysisCache::in_memory().unwrap();
        assert_eq!(
            cache
                .lookup("ghost", Some(HASH_A), Availability::Available)
                .unwrap(),
            None
        );
    }

    /// A payload containing non-finite floats survives the write (serde
    /// renders them as `null` JSON) but must fail soft at read time: the
    /// stage is `Invalid` with no payload, and never a panic or a record
    /// failure.
    #[test]
    fn test_nonfinite_payload_degrades_to_invalid() {
        let cache = AnalysisCache::in_memory().unwrap();
        let mut tempo = fixture_payload(AnalysisStage::Tempo);
        if let AnalysisStagePayload::Tempo(t) = &mut tempo {
            t.bpm = f64::NAN;
            t.tempo_drift_max_bpm = f64::INFINITY;
            t.tempo_drift_min_bpm = f64::NEG_INFINITY;
        }
        assert!(
            cache
                .upsert_stage(
                    TRACK_ID,
                    PATH,
                    Some(HASH_A),
                    AnalysisStage::Tempo,
                    AnalysisState::Complete,
                    Some(&tempo)
                )
                .is_ok(),
            "a non-finite payload must not hard-fail the write (serde renders NaN/Inf as null)"
        );

        let record = cache
            .lookup(TRACK_ID, Some(HASH_A), Availability::Available)
            .unwrap()
            .unwrap();
        let tempo = entry(&record, AnalysisStage::Tempo);
        assert_eq!(
            tempo.freshness,
            StageFreshness::Invalid,
            "null-able fields fail to parse back into f32 ⇒ Invalid"
        );
        assert_eq!(tempo.payload, None);
    }

    /// Truncated, mistyped, and non-object JSON in a stored payload degrades
    /// that single stage to `Invalid` — never a lookup error, never a panic,
    /// siblings untouched.
    #[test]
    fn test_malformed_payload_shapes_fail_soft() {
        let shapes = [
            r#"{"stage":"tempo","bpm":"not-a-number"}"#,
            r#"{"stage":"tempo","bpm":0.5,"bpm_confid"#,
            r#""just a string""#,
            "42",
            "[1,2,3]",
            "null",
            "\u{0}\u{0}\u{0}",
        ];
        for shape in shapes {
            let cache = AnalysisCache::in_memory().unwrap();
            store_all(&cache, Some(HASH_A));
            cache
                .conn()
                .lock()
                .unwrap()
                .execute(
                    "UPDATE analysis_results SET tempo_payload = ?1 WHERE track_id = ?2",
                    (shape, TRACK_ID),
                )
                .unwrap();

            let record = cache
                .lookup(TRACK_ID, Some(HASH_A), Availability::Available)
                .unwrap()
                .unwrap();
            let tempo = entry(&record, AnalysisStage::Tempo);
            assert_eq!(
                tempo.freshness,
                StageFreshness::Invalid,
                "shape {shape:?} must be Invalid"
            );
            assert_eq!(tempo.payload, None, "shape {shape:?}");
            for stage in [
                AnalysisStage::Key,
                AnalysisStage::Structure,
                AnalysisStage::Loudness,
                AnalysisStage::Mixability,
            ] {
                assert_eq!(
                    entry(&record, stage).freshness,
                    StageFreshness::Current,
                    "corruption must stay stage-local (shape {shape:?})"
                );
            }
        }
    }

    /// Hostile identifiers — empty ids, embedded NUL bytes, path traversal —
    /// are opaque TEXT blobs to this cache and must round-trip verbatim.
    #[test]
    fn test_hostile_identifiers_roundtrip() {
        let cache = AnalysisCache::in_memory().unwrap();
        let payload = fixture_payload(AnalysisStage::Loudness);
        let cases = [
            "",
            "trk\u{0}nul\u{0}",
            "../../etc/passwd",
            "trk-1/\u{1F4B6} 🎵 unicode",
        ];
        for (i, id) in cases.iter().enumerate() {
            cache
                .upsert_stage(
                    id,
                    "../../escape/../track.flac",
                    None,
                    AnalysisStage::Loudness,
                    AnalysisState::Complete,
                    Some(&payload),
                )
                .unwrap();
            let record = cache
                .lookup(id, None, Availability::Available)
                .unwrap()
                .unwrap_or_else(|| panic!("track {i} must be found"));
            assert_eq!(record.track_id, *id);
            assert_eq!(
                entry(&record, AnalysisStage::Loudness).freshness,
                StageFreshness::Current
            );
        }
        // Each hostile id is its own row: 4 rows total.
        let count: i64 = cache
            .conn()
            .lock()
            .unwrap()
            .query_row("SELECT COUNT(*) FROM analysis_results", [], |r| r.get(0))
            .unwrap();
        assert_eq!(count, 4, "each identifier must own exactly one row");

        // Empty-string domain fields inside a payload must also round-trip.
        let key = AnalysisStagePayload::Key(KeyProfile {
            key: String::new(),
            camelot: String::new(),
            key_confidence: 0.0,
            chroma_profile: Vec::new(),
        });
        cache
            .upsert_stage(
                TRACK_ID,
                PATH,
                None,
                AnalysisStage::Key,
                AnalysisState::Complete,
                Some(&key),
            )
            .unwrap();
        let record = cache
            .lookup(TRACK_ID, None, Availability::Available)
            .unwrap()
            .unwrap();
        assert_eq!(
            entry(&record, AnalysisStage::Key).payload,
            Some(key),
            "empty strings and zero values must round-trip"
        );
    }

    /// A multi-megabyte payload (a 250k-point energy curve) round-trips
    /// byte-exactly through the cache.
    #[test]
    fn test_huge_payload_roundtrip() {
        let cache = AnalysisCache::in_memory().unwrap();
        let structure = crate::models::track::StructureStageResult {
            segments: Vec::new(),
            phrases: PhraseBoundaries {
                boundaries_4bar: Vec::new(),
                boundaries_8bar: Vec::new(),
                boundaries_16bar: Vec::new(),
                boundaries_32bar: Vec::new(),
            },
            energy_curve: vec![0.5; 250_000],
        };
        let payload = AnalysisStagePayload::Structure(structure.clone());
        let json_len = serde_json::to_string(&payload).unwrap().len();
        assert!(
            json_len > 1_000_000,
            "fixture should be multi-MB, was {json_len}"
        );

        cache
            .upsert_stage(
                TRACK_ID,
                PATH,
                Some(HASH_A),
                AnalysisStage::Structure,
                AnalysisState::Complete,
                Some(&payload),
            )
            .unwrap();
        let record = cache
            .lookup(TRACK_ID, Some(HASH_A), Availability::Available)
            .unwrap()
            .unwrap();
        let s = entry(&record, AnalysisStage::Structure);
        assert_eq!(s.freshness, StageFreshness::Current);
        assert_eq!(
            s.payload,
            Some(payload),
            "the {json_len}-byte payload must round-trip exactly"
        );
    }

    /// Freshness precedence: `Missing` beats a hash mismatch, which beats
    /// the per-stage state. Payloads are served (flagged) under hash
    /// mismatch but never under a missing source that the stage never ran.
    #[test]
    fn test_freshness_precedence_missing_beats_hash_beats_state() {
        let cache = AnalysisCache::in_memory().unwrap();
        store_all(&cache, Some(HASH_A));
        // Flip key to `Failed` so the per-stage state differs from the rest.
        cache
            .upsert_stage(
                TRACK_ID,
                PATH,
                Some(HASH_A),
                AnalysisStage::Key,
                AnalysisState::Failed,
                None,
            )
            .unwrap();

        // Hash mismatch + available ⇒ every stage `Invalid`, even `Failed`.
        let record = cache
            .lookup(TRACK_ID, Some("hash-b"), Availability::Available)
            .unwrap()
            .unwrap();
        assert!(
            record
                .stages
                .iter()
                .all(|e| e.freshness == StageFreshness::Invalid),
            "a content change must invalidate every stage, including a failed one"
        );

        // Missing beats the hash mismatch: every stage `MissingSource`.
        let record = cache
            .lookup(TRACK_ID, Some("hash-b"), Availability::Missing)
            .unwrap()
            .unwrap();
        assert!(
            record
                .stages
                .iter()
                .all(|e| e.freshness == StageFreshness::MissingSource),
            "a missing file must dominate a hash mismatch"
        );

        // Matching hash + available ⇒ per-stage state: `Failed` stays
        // `Failed`, the rest `Current`.
        let record = cache
            .lookup(TRACK_ID, Some(HASH_A), Availability::Available)
            .unwrap()
            .unwrap();
        assert_eq!(
            entry(&record, AnalysisStage::Key).freshness,
            StageFreshness::Failed
        );
        for stage in [
            AnalysisStage::Tempo,
            AnalysisStage::Structure,
            AnalysisStage::Loudness,
            AnalysisStage::Mixability,
        ] {
            assert_eq!(entry(&record, stage).freshness, StageFreshness::Current);
        }
    }

    /// `NULL` current hash (the caller never hashed the file) must never
    /// trigger rule 2: a stored result stays `Current`.
    #[test]
    fn test_null_current_hash_never_invalidates() {
        let cache = AnalysisCache::in_memory().unwrap();
        let payload = fixture_payload(AnalysisStage::Tempo);
        cache
            .upsert_stage(
                TRACK_ID,
                PATH,
                Some(HASH_A),
                AnalysisStage::Tempo,
                AnalysisState::Complete,
                Some(&payload),
            )
            .unwrap();
        let record = cache
            .lookup(TRACK_ID, None, Availability::Available)
            .unwrap()
            .unwrap();
        assert_eq!(
            entry(&record, AnalysisStage::Tempo).freshness,
            StageFreshness::Current,
            "rule 2 requires BOTH hashes to be `Some`"
        );
    }

    /// A state string the CHECK constraint should have rejected (reachable
    /// only via direct SQL) degrades to `Invalid`, and a `complete` row with
    /// a valid version/payload maps to `Current`.
    #[test]
    fn test_stage_freshness_unknown_state_fails_soft() {
        let json = serde_json::to_string(&fixture_payload(AnalysisStage::Tempo)).unwrap();
        assert_eq!(
            stage_freshness(
                AnalysisStage::Tempo,
                "weird_state",
                Some("1.0.0"),
                Some(&json),
                false,
                false
            ),
            (StageFreshness::Invalid, None)
        );
        assert_eq!(
            stage_freshness(
                AnalysisStage::Tempo,
                "COMPLETE",
                Some("1.0.0"),
                Some(&json),
                false,
                false
            ),
            (StageFreshness::Invalid, None),
            "state strings are lowercase wire forms"
        );
        assert_eq!(
            stage_freshness(
                AnalysisStage::Tempo,
                "complete",
                Some("1.0.0"),
                Some(&json),
                false,
                false
            ),
            (
                StageFreshness::Current,
                Some(AnalysisStagePayload::Tempo(
                    crate::models::track::TempoProfile {
                        bpm: 120.0,
                        bpm_confidence: 0.95,
                        alternative_bpm_hypotheses: vec![60.0],
                        beat_positions: vec![0.0, 0.5],
                        downbeat_positions: vec![0.0],
                        bar_positions: vec![0.0],
                        grid_offset_seconds: 0.0,
                        is_variable_tempo: false,
                        tempo_drift_min_bpm: 120.0,
                        tempo_drift_max_bpm: 120.0,
                    }
                ))
            )
        );
    }

    /// `lookup` always returns exactly five stage entries, in declaration
    /// order, even when only one stage was ever written.
    #[test]
    fn test_stage_vector_always_five_in_declaration_order() {
        let cache = AnalysisCache::in_memory().unwrap();
        let payload = fixture_payload(AnalysisStage::Loudness);
        cache
            .upsert_stage(
                TRACK_ID,
                PATH,
                Some(HASH_A),
                AnalysisStage::Loudness,
                AnalysisState::Complete,
                Some(&payload),
            )
            .unwrap();

        let record = cache
            .lookup(TRACK_ID, Some(HASH_A), Availability::Available)
            .unwrap()
            .unwrap();
        let order: Vec<AnalysisStage> = record.stages.iter().map(|e| e.stage).collect();
        assert_eq!(order, AnalysisStage::all().to_vec());
        for (stage, e) in order.iter().zip(record.stages.iter()) {
            assert_eq!(
                e.freshness,
                if *stage == AnalysisStage::Loudness {
                    StageFreshness::Current
                } else {
                    StageFreshness::NotStarted
                }
            );
        }
    }

    /// A version re-rollout (bump the stored version, then roll it back)
    /// flips `Stale` → `Current` with no write: the stored payload and all
    /// sibling stages are untouched.
    #[test]
    fn test_version_rerollout_restores_without_write() {
        let cache = AnalysisCache::in_memory().unwrap();
        store_all(&cache, Some(HASH_A));
        let key_payload_before = entry(
            &cache
                .lookup(TRACK_ID, Some(HASH_A), Availability::Available)
                .unwrap()
                .unwrap(),
            AnalysisStage::Key,
        )
        .payload
        .clone();

        // Bump: stale. Roll back: current, payload bytes identical.
        cache
            .conn()
            .lock()
            .unwrap()
            .execute(
                "UPDATE analysis_results SET key_version = '9.9.9' WHERE track_id = ?1",
                [TRACK_ID],
            )
            .unwrap();
        let record = cache
            .lookup(TRACK_ID, Some(HASH_A), Availability::Available)
            .unwrap()
            .unwrap();
        assert_eq!(
            entry(&record, AnalysisStage::Key).freshness,
            StageFreshness::Stale
        );

        cache
            .conn()
            .lock()
            .unwrap()
            .execute(
                "UPDATE analysis_results SET key_version = '1.0.0' WHERE track_id = ?1",
                [TRACK_ID],
            )
            .unwrap();
        let record = cache
            .lookup(TRACK_ID, Some(HASH_A), Availability::Available)
            .unwrap()
            .unwrap();
        let key = entry(&record, AnalysisStage::Key);
        assert_eq!(key.freshness, StageFreshness::Current);
        assert_eq!(key.payload, key_payload_before, "no payload write happened");
        assert_eq!(
            entry(&record, AnalysisStage::Tempo).freshness,
            StageFreshness::Current
        );
    }

    /// The state/payload consistency matrix: exactly one of the two payload
    /// shapes is accepted per state, and a repeated identical non-complete
    /// write is `Unchanged`.
    #[test]
    fn test_upsert_state_payload_matrix() {
        let states = [
            AnalysisState::NotStarted,
            AnalysisState::InProgress,
            AnalysisState::Failed,
            AnalysisState::Complete,
        ];
        for state in states {
            let cache = AnalysisCache::in_memory().unwrap();
            let payload = fixture_payload(AnalysisStage::Tempo);
            let with_payload = cache
                .upsert_stage(
                    TRACK_ID,
                    PATH,
                    Some(HASH_A),
                    AnalysisStage::Tempo,
                    state,
                    Some(&payload),
                )
                .is_ok();
            let without_payload = cache
                .upsert_stage(
                    TRACK_ID,
                    PATH,
                    Some(HASH_A),
                    AnalysisStage::Tempo,
                    state,
                    None,
                )
                .is_ok();
            assert_eq!(
                with_payload,
                state == AnalysisState::Complete,
                "{} + payload must be accepted iff complete",
                state.as_str()
            );
            assert_eq!(
                without_payload,
                state != AnalysisState::Complete,
                "{} + no payload must be accepted iff not complete",
                state.as_str()
            );
        }

        // A repeated identical `Failed` upsert is `Unchanged` (no write).
        let cache = AnalysisCache::in_memory().unwrap();
        assert_eq!(
            cache
                .upsert_stage(
                    TRACK_ID,
                    PATH,
                    Some(HASH_A),
                    AnalysisStage::Key,
                    AnalysisState::Failed,
                    None
                )
                .unwrap(),
            UpsertOutcome::Inserted
        );
        assert_eq!(
            cache
                .upsert_stage(
                    TRACK_ID,
                    PATH,
                    Some(HASH_A),
                    AnalysisStage::Key,
                    AnalysisState::Failed,
                    None
                )
                .unwrap(),
            UpsertOutcome::Unchanged
        );
    }

    /// `track_id` is TEXT (binary compare): a case-different id is a miss.
    #[test]
    fn test_track_id_is_case_sensitive() {
        let cache = AnalysisCache::in_memory().unwrap();
        let payload = fixture_payload(AnalysisStage::Tempo);
        cache
            .upsert_stage(
                "TRK-1",
                PATH,
                Some(HASH_A),
                AnalysisStage::Tempo,
                AnalysisState::Complete,
                Some(&payload),
            )
            .unwrap();
        assert_eq!(
            cache
                .lookup("trk-1", Some(HASH_A), Availability::Available)
                .unwrap(),
            None
        );
        assert!(cache
            .lookup("TRK-1", Some(HASH_A), Availability::Available)
            .unwrap()
            .is_some());
    }

    /// Hammer: concurrent writers and readers share one `AnalysisCache`
    /// (`Mutex<Connection>`). Writers interleave upserts of one stage;
    /// readers must never error, never panic, and always see five stages
    /// with a well-formed freshness.
    #[test]
    fn test_concurrent_upsert_and_lookup_hammer() {
        use std::sync::Arc;
        let cache = Arc::new(AnalysisCache::in_memory().unwrap());
        let mut handles = Vec::new();
        for writer in 0..4u8 {
            let cache = Arc::clone(&cache);
            handles.push(std::thread::spawn(move || {
                for i in 0..25u32 {
                    let mut tempo = fixture_payload(AnalysisStage::Tempo);
                    if let AnalysisStagePayload::Tempo(t) = &mut tempo {
                        t.bpm = 100.0 + f64::from(i) + f64::from(writer);
                    }
                    cache
                        .upsert_stage(
                            TRACK_ID,
                            PATH,
                            Some(HASH_A),
                            AnalysisStage::Tempo,
                            AnalysisState::Complete,
                            Some(&tempo),
                        )
                        .unwrap();
                }
            }));
        }
        for _ in 0..4u8 {
            let cache = Arc::clone(&cache);
            handles.push(std::thread::spawn(move || {
                for _ in 0..50 {
                    // `None` is a valid observation: nothing guarantees this
                    // reader's lookups interleave with the writers' writes
                    // (a fast reader can finish before the first upsert
                    // lands), and a true miss is the documented contract.
                    let record = cache
                        .lookup(TRACK_ID, Some(HASH_A), Availability::Available)
                        .unwrap();
                    let Some(record) = record else { continue };
                    assert_eq!(record.stages.len(), 5);
                    for e in &record.stages {
                        assert!(
                            matches!(
                                e.freshness,
                                StageFreshness::NotStarted
                                    | StageFreshness::InProgress
                                    | StageFreshness::Current
                                    | StageFreshness::Stale
                                    | StageFreshness::Failed
                                    | StageFreshness::Invalid
                                    | StageFreshness::MissingSource
                            ),
                            "freshness must always be a valid variant"
                        );
                    }
                }
            }));
        }
        for handle in handles {
            handle.join().unwrap();
        }
        let record = cache
            .lookup(TRACK_ID, Some(HASH_A), Availability::Available)
            .unwrap()
            .unwrap();
        let tempo = entry(&record, AnalysisStage::Tempo);
        assert_eq!(tempo.freshness, StageFreshness::Current);
        match &tempo.payload {
            Some(AnalysisStagePayload::Tempo(t)) => {
                assert!(
                    (100.0..=127.0).contains(&t.bpm),
                    "final bpm must be one of the written values (100+i+writer), was {}",
                    t.bpm
                );
            }
            _ => panic!("tempo payload must be present"),
        }
    }

    /// A file-backed database persists across close/reopen (the real
    /// two-connection layout) and the schema is re-migrated to the current
    /// version on every open.
    #[test]
    fn test_file_db_persistence_across_reopen() {
        use std::sync::atomic::{AtomicU64, Ordering};
        static SEQ: AtomicU64 = AtomicU64::new(0);
        let seq = SEQ.fetch_add(1, Ordering::Relaxed);
        let dir = std::env::temp_dir().join(format!(
            "pulse-cache-harden-{}-{}-{seq}",
            std::process::id(),
            std::thread::current()
                .name()
                .unwrap_or("thread")
                .replace(|c: char| !c.is_ascii_alphanumeric(), "_")
        ));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("analysis.db");
        let _ = std::fs::remove_file(&path);

        let payload = fixture_payload(AnalysisStage::Mixability);
        {
            let cache = AnalysisCache::open(&path).unwrap();
            cache
                .upsert_stage(
                    TRACK_ID,
                    PATH,
                    Some(HASH_A),
                    AnalysisStage::Mixability,
                    AnalysisState::Complete,
                    Some(&payload),
                )
                .unwrap();
            // Drop closes the connection; the file must retain the row.
        }

        let cache = AnalysisCache::open(&path).unwrap();
        let record = cache
            .lookup(TRACK_ID, Some(HASH_A), Availability::Available)
            .unwrap()
            .unwrap_or_else(|| panic!("row must survive a reopen of {path:?}"));
        assert_eq!(
            entry(&record, AnalysisStage::Mixability).payload,
            Some(payload)
        );

        // The reopened DB is at the current schema version.
        let version: i64 = cache
            .conn()
            .lock()
            .unwrap()
            .query_row("PRAGMA user_version", [], |r| r.get(0))
            .unwrap();
        assert_eq!(version, i64::from(super::super::schema::SCHEMA_VERSION));

        let _ = std::fs::remove_dir_all(&dir);
    }

    /// `open` on a non-SQLite file (garbage bytes) or a path in a
    /// non-existent directory must fail with a typed error, never panic.
    #[test]
    fn test_open_rejects_unusable_paths() {
        use std::sync::atomic::{AtomicU64, Ordering};
        static SEQ: AtomicU64 = AtomicU64::new(0);
        let seq = SEQ.fetch_add(1, Ordering::Relaxed);
        let dir = std::env::temp_dir().join(format!(
            "pulse-cache-harden-bad-{}-{seq}",
            std::process::id()
        ));
        std::fs::create_dir_all(&dir).unwrap();
        let garbage = dir.join("garbage.db");
        std::fs::write(&garbage, vec![0xFFu8; 8192]).unwrap();
        assert!(
            AnalysisCache::open(&garbage).is_err(),
            "a non-database file must not open"
        );
        assert!(
            AnalysisCache::open(dir.join("nope".to_string() + "/x" + "/deep.db")).is_err(),
            "a missing parent directory must not open"
        );
        let _ = std::fs::remove_dir_all(&dir);
    }
}
