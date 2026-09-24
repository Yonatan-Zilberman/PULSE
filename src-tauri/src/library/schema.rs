//! Schema for the persistent library store (`pulse-library.db`).
//!
//! Migration policy (single forward-only migration, `PRAGMA user_version`):
//!
//! - **Fresh databases** (version 0, no `tracks` table): the CREATE batch
//!   below is written with the analysis columns (`file_hash`,
//!   `duration_seconds`, `bpm`, `key`, `camelot`, `profile_json`) **nullable**
//!   — a row whose file cannot be parsed has no duration, and a row whose
//!   hash has not been computed yet has no hash.
//! - **Pre-existing databases** (version 0, legacy `tracks` table with the
//!   old NOT NULL constraints): the engine cannot drop a NOT NULL constraint via
//!   `ALTER TABLE`, so the migration only ADDS the new state columns; the
//!   legacy NOT NULL constraints on `file_hash`/`duration_seconds`/`bpm`/
//!   `key`/`camelot`/`profile_json` stay in place. In that case ingest
//!   writes placeholder values to those columns (`file_hash=''`,
//!   `duration=0.0`, `bpm=0.0`, `key=''`, `camelot=''`, `profile_json='{}'`)
//!   while the new nullable columns (`sample_rate`, `channels`, …) hold the
//!   unambiguous state. This is safe because no production code reads those
//!   legacy columns (no consumers outside `library/` existed pre-migration).
//!
//! The `LibraryCache` detects the legacy shape at open time and applies the
//! placeholders itself.

use rusqlite::{Connection, Result};

/// Current schema version (`PRAGMA user_version`).
pub const SCHEMA_VERSION: u32 = 1;

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
/// above) is a no-op.
pub fn migrate(conn: &Connection) -> Result<()> {
    let version: i64 = conn.query_row("PRAGMA user_version", [], |row| row.get(0))?;
    if version >= i64::from(SCHEMA_VERSION) {
        return Ok(());
    }
    // Fresh databases get the full v1 shape; the CREATE is a no-op when the
    // legacy table already exists (IF NOT EXISTS).
    // Order matters: on a legacy `tracks` table the state columns do not
    // exist until `add_missing_track_columns`, and the indexes reference
    // them — so columns must be added before the index batch runs.
    conn.execute_batch(CREATE_TRACKS_TABLE)?;
    conn.execute_batch(CREATE_FOLDERS_TABLE)?;
    add_missing_track_columns(conn)?;
    conn.execute_batch(CREATE_INDEXES)?;
    conn.pragma_update(None, "user_version", i64::from(SCHEMA_VERSION))?;
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
}
