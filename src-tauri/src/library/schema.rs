//! Schema for the persistent library store (`pulse-library.db`).
//!
//! Migration policy (forward-only, stepwise on `PRAGMA user_version`):
//!
//! - **v0 → v1** — `tracks` + `library_folders`:
//!   - **Fresh databases** (version 0, no `tracks` table): the CREATE batch
//!     below is written with the analysis columns (`file_hash`,
//!     `duration_seconds`, `bpm`, `key`, `camelot`, `profile_json`) **nullable**
//!     — a row whose file cannot be parsed has no duration, and a row whose
//!     hash has not been computed yet has no hash.
//!   - **Pre-existing databases** (version 0, legacy `tracks` table with the
//!     old NOT NULL constraints): the engine cannot drop a NOT NULL constraint via
//!     `ALTER TABLE`, so the migration only ADDS the new state columns; the
//!     legacy NOT NULL constraints on `file_hash`/`duration_seconds`/`bpm`/
//!     `key`/`camelot`/`profile_json` stay in place. In that case ingest
//!     writes placeholder values to those columns (`file_hash=''`,
//!     `duration=0.0`, `bpm=0.0`, `key=''`, `camelot=''`, `profile_json='{}'`)
//!     while the new nullable columns (`sample_rate`, `channels`, …) hold the
//!     unambiguous state. This is safe because no production code reads those
//!     legacy columns (no consumers outside `library/` existed pre-migration).
//!
//!   The `LibraryCache` detects the legacy shape at open time and applies the
//!   placeholders itself.
//! - **v1 → v2** — adds the `analysis_results` table (one wide row per
//!   track with five model-versioned per-stage triplets) and its hash index.
//!   `CREATE TABLE IF NOT EXISTS` is the atomic idiom: a crashed v2 step
//!   cannot leave a partial table behind, so there is no add-missing-column
//!   helper for this step.
//!
//! A fresh (or legacy v0) database flows through both steps; a v1 database
//! runs only the v2 step; a database already at or above the current version
//! is left untouched (foreign/newer DBs are a no-op).

use rusqlite::{Connection, Result};

/// Current schema version (`PRAGMA user_version`).
pub const SCHEMA_VERSION: u32 = 2;

/// Fresh-database `tracks` table (nullable analysis columns + state columns).
pub const CREATE_TRACKS_TABLE: &str = "
CREATE TABLE IF NOT EXISTS tracks (
    id TEXT PRIMARY KEY,
    file_path TEXT NOT NULL UNIQUE,
    file_hash TEXT,
    file_size_bytes INTEGER NOT NULL,
    modified_timestamp INTEGER NOT NULL,
    title TEXT NOT NULL,
    artist TEXT NOT NULL,
    album TEXT,
    genre TEXT,
    duration_seconds REAL,
    bpm REAL,
    key TEXT,
    camelot TEXT,
    profile_json TEXT,
    ingest_status TEXT NOT NULL DEFAULT 'ok' CHECK (ingest_status IN ('ok', 'error')),
    ingest_error TEXT CHECK (ingest_error IS NULL OR ingest_error IN ('permission_denied', 'missing_file', 'corrupt_container', 'unreadable_file')),
    availability TEXT NOT NULL DEFAULT 'available' CHECK (availability IN ('available', 'missing')),
    analysis_state TEXT NOT NULL DEFAULT 'not_started' CHECK (analysis_state IN ('not_started', 'in_progress', 'complete', 'failed')),
    sample_rate INTEGER,
    channels INTEGER,
    duplicate_of TEXT,
    ingest_seq INTEGER NOT NULL DEFAULT 0,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);
";

/// Folder registry: canonicalized folder paths added by the user.
pub const CREATE_FOLDERS_TABLE: &str = "
CREATE TABLE IF NOT EXISTS library_folders (
    id TEXT PRIMARY KEY,
    path TEXT NOT NULL UNIQUE,
    added_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);
";

pub const CREATE_INDEXES: &str = "
CREATE INDEX IF NOT EXISTS idx_tracks_bpm ON tracks(bpm);
CREATE INDEX IF NOT EXISTS idx_tracks_camelot ON tracks(camelot);
CREATE INDEX IF NOT EXISTS idx_tracks_hash ON tracks(file_hash);
CREATE INDEX IF NOT EXISTS idx_tracks_availability ON tracks(availability);
CREATE INDEX IF NOT EXISTS idx_tracks_ingest_status ON tracks(ingest_status);
";

/// Per-track persisted analysis results: one wide row per `track_id` with
/// five `(stage_state, stage_version, stage_payload)` column triplets
/// (`{stage}_state`, `{stage}_version`, `{stage}_payload`). Payloads are
/// serialized [`AnalysisStagePayload`](crate::models::AnalysisStagePayload)
/// JSON. `track_id` is UNIQUE (the primary lookup; its constraint supplies
/// the index — no extra index on it). The table is written exclusively by
/// the analysis cache ([`super::analysis_cache`]); ingest never touches it.
pub const CREATE_ANALYSIS_RESULTS_TABLE: &str = "
CREATE TABLE IF NOT EXISTS analysis_results (
    id TEXT PRIMARY KEY,
    track_id TEXT NOT NULL UNIQUE,
    file_path TEXT NOT NULL,
    file_hash TEXT,
    tempo_state TEXT NOT NULL DEFAULT 'not_started'
        CHECK (tempo_state IN ('not_started','in_progress','complete','failed')),
    tempo_version TEXT,
    tempo_payload TEXT,
    key_state TEXT NOT NULL DEFAULT 'not_started'
        CHECK (key_state IN ('not_started','in_progress','complete','failed')),
    key_version TEXT,
    key_payload TEXT,
    structure_state TEXT NOT NULL DEFAULT 'not_started'
        CHECK (structure_state IN ('not_started','in_progress','complete','failed')),
    structure_version TEXT,
    structure_payload TEXT,
    loudness_state TEXT NOT NULL DEFAULT 'not_started'
        CHECK (loudness_state IN ('not_started','in_progress','complete','failed')),
    loudness_version TEXT,
    loudness_payload TEXT,
    mixability_state TEXT NOT NULL DEFAULT 'not_started'
        CHECK (mixability_state IN ('not_started','in_progress','complete','failed')),
    mixability_version TEXT,
    mixability_payload TEXT,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);
";

pub const CREATE_ANALYSIS_INDEXES: &str = "
CREATE INDEX IF NOT EXISTS idx_analysis_results_hash ON analysis_results(file_hash);
";

/// New state columns added to a legacy (pre-migration) `tracks` table via
/// `ALTER TABLE … ADD COLUMN`. Each declaration must be valid for `ALTER`
/// (NOT NULL columns need a DEFAULT). Fresh databases already have these
/// columns from `CREATE_TRACKS_TABLE`.
/// The pre-migration `tracks` shape (NOT NULL analysis columns, no state
/// columns). Retained so the legacy migration path stays testable after the
/// production CREATE was reworked.
#[cfg(test)]
pub(crate) const LEGACY_CREATE_TRACKS: &str = "
CREATE TABLE IF NOT EXISTS tracks (
    id TEXT PRIMARY KEY,
    file_path TEXT NOT NULL UNIQUE,
    file_hash TEXT NOT NULL,
    file_size_bytes INTEGER NOT NULL,
    modified_timestamp INTEGER NOT NULL,
    title TEXT NOT NULL,
    artist TEXT NOT NULL,
    album TEXT,
    genre TEXT,
    duration_seconds REAL NOT NULL,
    bpm REAL NOT NULL,
    key TEXT NOT NULL,
    camelot TEXT NOT NULL,
    profile_json TEXT NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);
";

const NEW_TRACKS_COLUMNS: &[(&str, &str)] = &[
    ("ingest_status", "TEXT NOT NULL DEFAULT 'ok' CHECK (ingest_status IN ('ok', 'error'))"),
    (
        "ingest_error",
        "TEXT CHECK (ingest_error IS NULL OR ingest_error IN ('permission_denied', 'missing_file', 'corrupt_container', 'unreadable_file'))",
    ),
    (
        "availability",
        "TEXT NOT NULL DEFAULT 'available' CHECK (availability IN ('available', 'missing'))",
    ),
    (
        "analysis_state",
        "TEXT NOT NULL DEFAULT 'not_started' CHECK (analysis_state IN ('not_started', 'in_progress', 'complete', 'failed'))",
    ),
    ("sample_rate", "INTEGER"),
    ("channels", "INTEGER"),
    ("duplicate_of", "TEXT"),
    ("ingest_seq", "INTEGER NOT NULL DEFAULT 0"),
];

/// Run the 0 → `SCHEMA_VERSION` migration on `conn`.
///
/// Idempotent: running it on a database already at `SCHEMA_VERSION` (or
/// above) is a no-op. Steps are guarded by the stored version, so a crash
/// between steps resumes from the next unapplied step on the next open.
pub fn migrate(conn: &Connection) -> Result<()> {
    let version: i64 = conn.query_row("PRAGMA user_version", [], |row| row.get(0))?;
    if version >= i64::from(SCHEMA_VERSION) {
        return Ok(());
    }
    if version < 1 {
        // Fresh databases get the full v1 shape; the CREATE is a no-op when
        // the legacy table already exists (IF NOT EXISTS).
        // Order matters: on a legacy `tracks` table the state columns do not
        // exist until `add_missing_track_columns`, and the indexes reference
        // them — so columns must be added before the index batch runs.
        conn.execute_batch(CREATE_TRACKS_TABLE)?;
        conn.execute_batch(CREATE_FOLDERS_TABLE)?;
        add_missing_track_columns(conn)?;
        conn.execute_batch(CREATE_INDEXES)?;
        // Target the literal step number, not `SCHEMA_VERSION`: a crash
        // between the v1 and v2 steps must resume at the v2 step.
        conn.pragma_update(None, "user_version", 1)?;
    }
    if version < 2 {
        conn.execute_batch(CREATE_ANALYSIS_RESULTS_TABLE)?;
        conn.execute_batch(CREATE_ANALYSIS_INDEXES)?;
        conn.pragma_update(None, "user_version", 2)?;
    }
    Ok(())
}

/// Add any v1 state columns absent from a pre-existing `tracks` table.
///
/// On a legacy table the old NOT NULL constraints on the analysis columns
/// cannot be dropped; only the new columns are appended (see module docs).
pub(crate) fn add_missing_track_columns(conn: &Connection) -> Result<()> {
    let mut stmt = conn.prepare("PRAGMA table_info(tracks)")?;
    let existing: Vec<String> = stmt
        .query_map([], |row| row.get::<_, String>(1))?
        .collect::<Result<_>>()?;
    for (name, ddl) in NEW_TRACKS_COLUMNS {
        if !existing.iter().any(|col| col == name) {
            conn.execute_batch(&format!("ALTER TABLE tracks ADD COLUMN {name} {ddl};"))?;
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::library::cache::LibraryCache;
    use rusqlite::Connection;

    fn table_column_types(conn: &Connection) -> Vec<(String, String, bool)> {
        let mut stmt = conn.prepare("PRAGMA table_info(tracks)").unwrap();
        let rows = stmt
            .query_map([], |r| {
                Ok((
                    r.get::<_, String>(1)?,
                    r.get::<_, String>(2)?,
                    r.get::<_, i32>(3)? != 0,
                ))
            })
            .unwrap();
        rows.collect::<rusqlite::Result<Vec<_>>>().unwrap()
    }

    #[test]
    fn test_migrate_fresh_database() {
        let cache = LibraryCache::in_memory().unwrap();
        let conn = cache.conn().lock().unwrap();
        let version: i64 = conn
            .query_row("PRAGMA user_version", [], |r| r.get(0))
            .unwrap();
        assert_eq!(version, i64::from(SCHEMA_VERSION));
        // Fresh DB: analysis columns are nullable, state columns present.
        let cols = table_column_types(&conn);
        let nullable = |name: &str| {
            cols.iter()
                .find(|(n, _, _)| n == name)
                .is_some_and(|(_, _, notnull)| !*notnull)
        };
        assert!(
            nullable("duration_seconds"),
            "fresh DB duration_seconds must be nullable"
        );
        assert!(nullable("file_hash"), "fresh DB file_hash must be nullable");
        assert!(cols.iter().any(|(n, _, _)| n == "ingest_status"));
        assert!(cols.iter().any(|(n, _, _)| n == "ingest_seq"));
        // The v2 step ran: the analysis-results table exists on a fresh DB.
        assert_analysis_results_table(&conn);
    }

    /// Shared v2 assertion: the `analysis_results` table exists exactly once
    /// with all five stage state columns present.
    fn assert_analysis_results_table(conn: &Connection) {
        let count: i64 = conn
            .query_row(
                "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = 'analysis_results'",
                [],
                |r| r.get(0),
            )
            .unwrap();
        assert_eq!(count, 1, "analysis_results must exist exactly once");
        let mut stmt = conn.prepare("PRAGMA table_info(analysis_results)").unwrap();
        let col_names: Vec<String> = stmt
            .query_map([], |r| r.get::<_, String>(1))
            .unwrap()
            .map(|r| r.unwrap())
            .collect();
        for stage in ["tempo", "key", "structure", "loudness", "mixability"] {
            for suffix in ["_state", "_version", "_payload"] {
                assert!(
                    col_names.iter().any(|c| c == &format!("{stage}{suffix}")),
                    "analysis_results must have the {stage}{suffix} column"
                );
            }
        }
    }

    #[test]
    fn test_migrate_legacy_table() {
        let conn = Connection::open_in_memory().unwrap();
        conn.execute_batch(LEGACY_CREATE_TRACKS).unwrap();
        // Simulate a pre-existing v0 database with a legacy row.
        conn.execute(
            "INSERT INTO tracks (id, file_path, file_hash, file_size_bytes, modified_timestamp, title, artist, duration_seconds, bpm, key, camelot, profile_json)
             VALUES ('legacy-1', '/legacy/song.wav', 'abc', 1234, 1700000000, 'Legacy', 'Artist', 300.0, 120.0, 'Amin', '8a', '{}')",
            [],
        )
        .unwrap();

        migrate(&conn).unwrap();

        let version: i64 = conn
            .query_row("PRAGMA user_version", [], |r| r.get(0))
            .unwrap();
        assert_eq!(version, i64::from(SCHEMA_VERSION));

        let cols = table_column_types(&conn);
        // Old NOT NULL constraints are preserved (SQLite cannot drop them).
        assert!(cols
            .iter()
            .any(|(n, _, notnull)| n == "duration_seconds" && *notnull));
        assert!(cols
            .iter()
            .any(|(n, _, notnull)| n == "file_hash" && *notnull));
        // New state columns were added.
        assert!(cols.iter().any(|(n, _, _)| n == "ingest_status"));
        assert!(cols.iter().any(|(n, _, _)| n == "ingest_seq"));
        assert!(cols.iter().any(|(n, _, _)| n == "duplicate_of"));

        // The legacy row survives and is readable with default state.
        let row: (String, String, String) = conn
            .query_row(
                "SELECT title, ingest_status, availability FROM tracks WHERE id = 'legacy-1'",
                [],
                |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)),
            )
            .unwrap();
        assert_eq!(
            row,
            (
                "Legacy".to_string(),
                "ok".to_string(),
                "available".to_string()
            )
        );

        // The folders table exists after migration.
        let has_folders: bool = conn
            .query_row(
                "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = 'library_folders'",
                [],
                |r| r.get::<_, i64>(0),
            )
            .unwrap()
            > 0;
        assert!(has_folders);
        // The legacy v0 DB must also reach v2.
        assert_analysis_results_table(&conn);
    }

    /// A v1 database (tracks + folders, no `analysis_results`) gains exactly
    /// the v2 table and index on migrate, with the v1 shape untouched.
    #[test]
    fn test_migrate_v1_to_v2() {
        let conn = Connection::open_in_memory().unwrap();
        conn.execute_batch(CREATE_TRACKS_TABLE).unwrap();
        conn.execute_batch(CREATE_FOLDERS_TABLE).unwrap();
        conn.execute_batch(CREATE_INDEXES).unwrap();
        conn.pragma_update(None, "user_version", 1).unwrap();
        // A pre-existing v1 row must survive the v2 step.
        conn.execute(
            "INSERT INTO tracks (id, file_path, file_hash, file_size_bytes, modified_timestamp, title, artist)
             VALUES ('v1-1', '/v1/song.wav', 'deadbeef', 100, 1700000000, 'V1 Song', 'Artist')",
            [],
        )
        .unwrap();

        migrate(&conn).unwrap();

        let version: i64 = conn
            .query_row("PRAGMA user_version", [], |r| r.get(0))
            .unwrap();
        assert_eq!(version, 2);
        assert_analysis_results_table(&conn);
        let index_count: i64 = conn
            .query_row(
                "SELECT COUNT(*) FROM sqlite_master WHERE type = 'index' AND name = 'idx_analysis_results_hash'",
                [],
                |r| r.get(0),
            )
            .unwrap();
        assert_eq!(index_count, 1);
        // The v1 row is untouched and still readable.
        let row: (String, String, String) = conn
            .query_row(
                "SELECT title, file_hash, ingest_status FROM tracks WHERE id = 'v1-1'",
                [],
                |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)),
            )
            .unwrap();
        assert_eq!(
            row,
            (
                "V1 Song".to_string(),
                "deadbeef".to_string(),
                "ok".to_string()
            )
        );
    }

    /// Running migrate twice on a v1 database applies the v2 step exactly
    /// once: same version, same table, identical column shape.
    #[test]
    fn test_migrate_v2_idempotent() {
        let conn = Connection::open_in_memory().unwrap();
        conn.execute_batch(CREATE_TRACKS_TABLE).unwrap();
        conn.execute_batch(CREATE_FOLDERS_TABLE).unwrap();
        conn.execute_batch(CREATE_INDEXES).unwrap();
        conn.pragma_update(None, "user_version", 1).unwrap();

        migrate(&conn).unwrap();
        let table_columns = |conn: &Connection| -> Vec<String> {
            let mut stmt = conn.prepare("PRAGMA table_info(analysis_results)").unwrap();
            stmt.query_map([], |r| r.get::<_, String>(1))
                .unwrap()
                .map(|r| r.unwrap())
                .collect()
        };
        let first = table_columns(&conn);
        migrate(&conn).unwrap();

        let version: i64 = conn
            .query_row("PRAGMA user_version", [], |r| r.get(0))
            .unwrap();
        assert_eq!(version, 2);
        let count: i64 = conn
            .query_row(
                "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = 'analysis_results'",
                [],
                |r| r.get(0),
            )
            .unwrap();
        assert_eq!(
            count, 1,
            "the v2 table must exist exactly once after two runs"
        );
        assert_eq!(first, table_columns(&conn), "column shape must be stable");
    }

    #[test]
    fn test_migrate_is_idempotent() {
        let cache = LibraryCache::in_memory().unwrap();
        let conn = cache.conn().lock().unwrap();
        // Re-running the migration at the current version is a no-op.
        migrate(&conn).unwrap();
        let version: i64 = conn
            .query_row("PRAGMA user_version", [], |r| r.get(0))
            .unwrap();
        assert_eq!(version, i64::from(SCHEMA_VERSION));
        // Column set is unchanged after a second migration.
        let before = table_column_types(&conn);
        migrate(&conn).unwrap();
        let after = table_column_types(&conn);
        assert_eq!(before, after);
    }

    /// A database whose `user_version` is already *above* the current
    /// schema (foreign/newer DB) must be left completely untouched.
    #[test]
    fn test_migrate_future_version_is_noop() {
        let conn = Connection::open_in_memory().unwrap();
        conn.pragma_update(None, "user_version", 999).unwrap();
        migrate(&conn).unwrap();
        let version: i64 = conn
            .query_row("PRAGMA user_version", [], |r| r.get(0))
            .unwrap();
        assert_eq!(version, 999, "the version must not be rewritten");
        let tables: i64 = conn
            .query_row(
                "SELECT COUNT(*) FROM sqlite_master
                 WHERE type = 'table' AND name IN ('tracks', 'library_folders')",
                [],
                |r| r.get(0),
            )
            .unwrap();
        assert_eq!(tables, 0, "no tables may be created for a newer DB");
    }

    /// Crash-recovery shape: a legacy table that already carries *some* of
    /// the new columns must get the missing ones appended exactly once,
    /// without duplicating the pre-existing ones.
    #[test]
    fn test_migrate_partial_column_set_adds_only_missing() {
        let conn = Connection::open_in_memory().unwrap();
        conn.execute_batch(LEGACY_CREATE_TRACKS).unwrap();
        // Simulate a migration that crashed after adding one column.
        conn.execute_batch("ALTER TABLE tracks ADD COLUMN ingest_seq INTEGER NOT NULL DEFAULT 0;")
            .unwrap();
        migrate(&conn).unwrap();

        let mut names: Vec<String> = Vec::new();
        let mut stmt = conn.prepare("PRAGMA table_info(tracks)").unwrap();
        for row in stmt.query_map([], |r| r.get::<_, String>(1)).unwrap() {
            names.push(row.unwrap());
        }
        for (name, _) in NEW_TRACKS_COLUMNS {
            let count = names.iter().filter(|n| n == name).count();
            assert_eq!(count, 1, "column {name} must exist exactly once");
        }
        let version: i64 = conn
            .query_row("PRAGMA user_version", [], |r| r.get(0))
            .unwrap();
        assert_eq!(version, i64::from(SCHEMA_VERSION));
    }

    /// A foreign/newer database (`user_version` above the current one) is
    /// left completely untouched: no tables added, no version downgrade.
    #[test]
    fn test_migrate_newer_user_version_is_noop() {
        let conn = Connection::open_in_memory().unwrap();
        conn.execute_batch("CREATE TABLE foreign_table (id TEXT PRIMARY KEY, blob_col BLOB);")
            .unwrap();
        conn.execute(
            "INSERT INTO foreign_table VALUES ('keep-me', X'DEADBEEF')",
            [],
        )
        .unwrap();
        conn.pragma_update(None, "user_version", 99).unwrap();

        migrate(&conn).unwrap();

        let version: i64 = conn
            .query_row("PRAGMA user_version", [], |r| r.get(0))
            .unwrap();
        assert_eq!(version, 99, "a newer DB must never be downgraded");
        let foreign: i64 = conn
            .query_row(
                "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = 'foreign_table'",
                [],
                |r| r.get(0),
            )
            .unwrap();
        assert_eq!(foreign, 1, "the foreign table must survive byte-identical");
        let row: (String, i64) = conn
            .query_row("SELECT id, length(blob_col) FROM foreign_table", [], |r| {
                Ok((r.get(0)?, r.get(1)?))
            })
            .unwrap();
        assert_eq!(row, ("keep-me".to_string(), 4));
        let analysis: i64 = conn
            .query_row(
                "SELECT COUNT(*) FROM sqlite_master WHERE name = 'analysis_results'",
                [],
                |r| r.get(0),
            )
            .unwrap();
        assert_eq!(analysis, 0, "no v2 table may be created on a newer DB");
    }

    /// The v2 table's integrity constraints: the stage-state CHECK rejects
    /// unknown values (even case-variants), `track_id` is UNIQUE, and a
    /// minimal insert defaults every stage state to `not_started` with
    /// `NULL` version/payload.
    #[test]
    fn test_analysis_results_constraints() {
        let conn = Connection::open_in_memory().unwrap();
        migrate(&conn).unwrap();

        // Minimal insert: only the NOT NULL columns.
        conn.execute(
            "INSERT INTO analysis_results (id, track_id, file_path) VALUES ('a1', 't1', '/a.flac')",
            [],
        )
        .unwrap();
        let states: Vec<String> = {
            let mut stmt = conn
                .prepare("SELECT tempo_state, key_state, structure_state, loudness_state, mixability_state FROM analysis_results WHERE track_id = 't1'")
                .unwrap();
            let mut rows = stmt
                .query_map([], |r| {
                    Ok((
                        r.get::<_, String>(0)?,
                        r.get::<_, String>(1)?,
                        r.get::<_, String>(2)?,
                        r.get::<_, String>(3)?,
                        r.get::<_, String>(4)?,
                    ))
                })
                .unwrap();
            let (s1, s2, s3, s4, s5) = rows.next().unwrap().unwrap();
            vec![s1, s2, s3, s4, s5]
        };
        assert!(
            states.iter().all(|s| s == "not_started"),
            "every state must default to not_started, got {states:?}"
        );

        // The CHECK constraint rejects every unknown state string (and a
        // case-variant of a valid one) on both INSERT and UPDATE.
        for bad in ["DONE", "complete ", "", "in-progress", "pending", "x"] {
            assert!(
                conn.execute(
                    "INSERT INTO analysis_results (id, track_id, file_path, tempo_state) VALUES ('bad', 't2', '/b.flac', ?1)",
                    [bad],
                )
                .is_err(),
                "state {bad:?} must be rejected by the CHECK constraint"
            );
        }
        assert!(
            conn.execute(
                "UPDATE analysis_results SET key_state = 'DONE' WHERE track_id = 't1'",
                [],
            )
            .is_err(),
            "the CHECK constraint must also reject updates"
        );

        // `track_id` is UNIQUE: a second row for the same track fails.
        assert!(conn
            .execute(
                "INSERT INTO analysis_results (id, track_id, file_path) VALUES ('a2', 't1', '/a.flac')",
                [],
            )
            .is_err(),
            "track_id must be unique (one wide row per track)");
    }
}
