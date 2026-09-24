// The public methods unwrap the lock guard; a poisoned mutex means the
// application is already unwinding, so panicking is the only sane option.
#![allow(clippy::missing_panics_doc)]
//! Persistent library cache: a `Mutex`-guarded database connection.
//!
//! `rusqlite::Connection` is `!Sync`, so it is wrapped in a `std::sync::Mutex`
//! to make [`LibraryCache`] `Send + Sync` and usable as `Arc`-managed Tauri
//! state.
//!
//! **Lock-splitting rule:** every public method locks the connection exactly
//! once and delegates to a private `*_locked(conn, …)` helper that assumes
//! the lock is already held. No public method may call another public method
//! on the same receiver (the `std` mutex is non-reentrant).
//!
//! **Legacy placeholder rule:** databases that predate the v1 migration still
//! enforce NOT NULL on the analysis columns (the database engine cannot
//! drop them via `ALTER` — see [`super::schema`]). For those, absent analysis values are
//! written as placeholders (`file_hash=''`, `duration=0.0`, `bpm=0.0`,
//! `key=''`, `camelot=''`, `profile_json='{}'`); fresh databases keep `NULL`.
//! All reads go through the same column names either way.

use rusqlite::{Connection, Result};

use super::errors::LibraryError;
use super::schema::migrate;
use super::types::{
    AnalysisState, Availability, IngestErrorReason, IngestStatus, TrackRow, TrackUpsert,
    UpsertOutcome,
};

/// Placeholder values written to the legacy NOT NULL analysis columns (see
/// module docs).
const LEGACY_PLACEHOLDER_HASH: &str = "";
const LEGACY_PLACEHOLDER_DURATION: f64 = 0.0;
const LEGACY_PLACEHOLDER_BPM: f64 = 0.0;
const LEGACY_PLACEHOLDER_KEY: &str = "";
const LEGACY_PLACEHOLDER_CAMELOT: &str = "";
const LEGACY_PLACEHOLDER_PROFILE: &str = "{}";

pub struct LibraryCache {
    conn: std::sync::Mutex<Connection>,
    /// True when this database predates the v1 migration and still carries
    /// the legacy NOT NULL constraints on the analysis columns.
    legacy_strict: bool,
}

impl LibraryCache {
    /// Open (or create) the persistent database at `path` and migrate it.
    pub fn open<P: AsRef<std::path::Path>>(path: P) -> Result<Self, LibraryError> {
        let conn = Connection::open(path)?;
        Self::from_connection(conn)
    }

    /// An in-memory database with the current schema (test path).
    pub fn in_memory() -> Result<Self, LibraryError> {
        Self::from_connection(Connection::open_in_memory()?)
    }

    fn from_connection(conn: Connection) -> Result<Self, LibraryError> {
        migrate(&conn)?;
        // Fresh v1 databases declare the analysis columns nullable; only a
        // pre-migration (legacy) table still enforces NOT NULL on them.
        let legacy_strict = duration_column_notnull(&conn)?;
        Ok(Self {
            conn: std::sync::Mutex::new(conn),
            legacy_strict,
        })
    }

    /// Test hook: access to the underlying connection (the schema migration
    /// tests need it to pre-create a legacy table).
    #[cfg(test)]
    #[doc(hidden)]
    pub(crate) fn conn(&self) -> &std::sync::Mutex<Connection> {
        &self.conn
    }

    // ------------------------------------------------------------------
    // Folder registry
    // ------------------------------------------------------------------

    /// Register a canonicalized folder path (idempotent). Returns the row id.
    pub fn add_folder(&self, path: &str) -> Result<String, LibraryError> {
        let conn = self.conn.lock().unwrap();
        conn.execute(
            "INSERT OR IGNORE INTO library_folders (id, path) VALUES (?1, ?1)",
            [path],
        )?;
        Ok(path.to_string())
    }

    /// All registered folder paths (canonicalized).
    pub fn folder_paths(&self) -> Result<Vec<String>, LibraryError> {
        let guard = self.conn.lock().unwrap();
        folder_paths_locked(&guard)
    }

    // ------------------------------------------------------------------
    // Track rows
    // ------------------------------------------------------------------

    /// Insert or update a track row keyed on its canonical `file_path`.
    ///
    /// The write covers the metadata/state columns plus the analysis
    /// placeholders (see module docs). `duplicate_of` and `analysis_state`
    /// are never touched: the duplicate link is applied by
    /// [`Self::apply_duplicate_links`], and analysis state is never mutated
    /// by this task. The row id on insert is random (16-byte hex) and the
    /// `ingest_seq` is `MAX(ingest_seq) + 1` within the same statement, so
    /// the whole upsert is a single atomic write.
    pub fn upsert_track(&self, row: &TrackUpsert) -> Result<UpsertOutcome, LibraryError> {
        let guard = self.conn.lock().unwrap();
        upsert_track_locked(&guard, self.legacy_strict, row)
    }

    /// Overwrite the mutable columns of an existing row (keyed by id).
    ///
    /// Used for transitions that must not touch `ingest_seq` (e.g. lazy
    /// missing-mark). Returns `false` if the row id is unknown.
    pub fn update_track(&self, id: &str, row: &TrackRow) -> Result<bool, LibraryError> {
        let guard = self.conn.lock().unwrap();
        update_track_locked(&guard, self.legacy_strict, id, row)
    }

    /// All track rows, ordered by `file_path`.
    pub fn all_tracks(&self) -> Result<Vec<TrackRow>, LibraryError> {
        let guard = self.conn.lock().unwrap();
        all_tracks_locked(&guard)
    }

    /// One row by id.
    pub fn get_track(&self, id: &str) -> Result<Option<TrackRow>, LibraryError> {
        let guard = self.conn.lock().unwrap();
        get_track_locked(&guard, id)
    }

    /// One row by canonical path (the ingest lookup key).
    pub fn get_track_by_path(&self, path: &str) -> Result<Option<TrackRow>, LibraryError> {
        let guard = self.conn.lock().unwrap();
        get_track_by_path_locked(&guard, path)
    }

    /// Record a computed content hash for a row. Returns `false` if the row
    /// id is unknown.
    pub fn apply_hash(&self, id: &str, hash: &str) -> Result<bool, LibraryError> {
        let conn = self.conn.lock().unwrap();
        let n = conn.execute(
            "UPDATE tracks SET file_hash = ?2, updated_at = CURRENT_TIMESTAMP WHERE id = ?1",
            (id, hash),
        )?;
        Ok(n > 0)
    }

    /// Apply duplicate-link updates in one transaction.
    ///
    /// The ingest dedup pass (in `super::store`) calls this with only the
    /// *deltas* against the stored `duplicate_of` (including `None`-clears);
    /// the cache does not re-derive the full mapping itself. Rows absent from
    /// the slice are left untouched.
    pub fn apply_duplicate_links(
        &self,
        links: &[(String, Option<String>)],
    ) -> Result<usize, LibraryError> {
        let mut conn = self.conn.lock().unwrap();
        let tx = conn.transaction()?;
        for (id, duplicate_of) in links {
            tx.execute(
                "UPDATE tracks SET duplicate_of = ?2, updated_at = CURRENT_TIMESTAMP WHERE id = ?1",
                (id, duplicate_of),
            )?;
        }
        tx.commit()?;
        Ok(links.len())
    }

    /// Next `ingest_seq` value (`MAX(ingest_seq) + 1`).
    pub fn next_ingest_seq(&self) -> Result<i64, LibraryError> {
        let conn = self.conn.lock().unwrap();
        conn.query_row(
            "SELECT COALESCE(MAX(ingest_seq), 0) + 1 FROM tracks",
            [],
            |r| r.get(0),
        )
        .map_err(LibraryError::from)
    }

    /// Mark rows missing in one transaction (lazy missing-mark).
    pub fn mark_missing(&self, ids: &[String]) -> Result<usize, LibraryError> {
        if ids.is_empty() {
            return Ok(0);
        }
        let mut conn = self.conn.lock().unwrap();
        let tx = conn.transaction()?;
        let mut n = 0;
        for id in ids {
            n += tx.execute(
                "UPDATE tracks SET availability = 'missing', updated_at = CURRENT_TIMESTAMP WHERE id = ?1",
                [id],
            )?;
        }
        tx.commit()?;
        Ok(n)
    }

    /// `(id, hash, ingest_seq)` for every row with a real content hash —
    /// the dedup pass input. Excludes NULL hashes and legacy `''`
    /// placeholders; ordered by `ingest_seq`.
    pub fn hash_rows(&self) -> Result<Vec<(String, String, i64)>, LibraryError> {
        let guard = self.conn.lock().unwrap();
        let conn: &Connection = &guard;
        let mut stmt = conn.prepare(
            "SELECT id, file_hash, ingest_seq FROM tracks
             WHERE ingest_status = 'ok' AND file_hash IS NOT NULL AND file_hash != ''
             ORDER BY ingest_seq",
        )?;
        let rows = stmt
            .query_map([], |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)))?
            .collect::<Result<Vec<_>>>()?;
        Ok(rows)
    }

    /// Whether this database predates the v1 migration (legacy NOT NULL
    /// analysis columns requiring placeholder writes).
    pub fn legacy_strict(&self) -> bool {
        self.legacy_strict
    }
}

/// Whether the `duration_seconds` column of the `tracks` table carries
/// a NOT NULL constraint (cursor-form pragma — compatible with the older
/// bundled database engine, which lacks table-valued function support).
fn duration_column_notnull(conn: &Connection) -> Result<bool, LibraryError> {
    let mut stmt = conn.prepare("PRAGMA table_info(tracks)")?;
    let mut notnull = false;
    for row in stmt.query_map([], |r| {
        Ok((r.get::<_, String>(1)?, r.get::<_, i32>(3)? != 0))
    })? {
        let (name, is_notnull) = row?;
        if name == "duration_seconds" {
            notnull = is_notnull;
        }
    }
    Ok(notnull)
}

// ----------------------------------------------------------------------
// Locked (lock already held) helpers
// ----------------------------------------------------------------------

fn folder_paths_locked(conn: &Connection) -> Result<Vec<String>, LibraryError> {
    let mut stmt = conn.prepare("SELECT path FROM library_folders")?;
    let rows = stmt.query_map([], |r| r.get::<_, String>(0))?;
    let paths = rows.collect::<Result<Vec<_>>>()?;
    Ok(paths)
}

fn all_tracks_locked(conn: &Connection) -> Result<Vec<TrackRow>, LibraryError> {
    let mut stmt = conn.prepare(&format!(
        "SELECT {TRACKS_COLUMNS} FROM tracks ORDER BY file_path"
    ))?;
    let rows = stmt
        .query_map([], map_track_row)?
        .collect::<Result<Vec<_>>>()?;
    Ok(rows)
}

fn get_track_locked(conn: &Connection, id: &str) -> Result<Option<TrackRow>, LibraryError> {
    let mut stmt = conn.prepare(&format!(
        "SELECT {TRACKS_COLUMNS} FROM tracks WHERE id = ?1"
    ))?;
    let row: Option<TrackRow> = stmt.query_map([id], map_track_row)?.next().transpose()?;
    Ok(row)
}

fn get_track_by_path_locked(
    conn: &Connection,
    path: &str,
) -> Result<Option<TrackRow>, LibraryError> {
    let mut stmt = conn.prepare(&format!(
        "SELECT {TRACKS_COLUMNS} FROM tracks WHERE file_path = ?1"
    ))?;
    let row: Option<TrackRow> = stmt.query_map([path], map_track_row)?.next().transpose()?;
    Ok(row)
}

/// The `tracks` columns in the exact order [`map_track_row`] expects. The
/// physical column order differs between fresh v1 tables and migrated
/// legacy tables (ALTER appends), so all reads select by name, never `*`.
const TRACKS_COLUMNS: &str = "id, file_path, file_hash, file_size_bytes, modified_timestamp, title, artist, album, genre, duration_seconds, bpm, key, camelot, profile_json, ingest_status, ingest_error, availability, analysis_state, sample_rate, channels, duplicate_of, ingest_seq, created_at, updated_at";

fn map_track_row(row: &rusqlite::Row<'_>) -> Result<TrackRow> {
    Ok(TrackRow {
        id: row.get(0)?,
        file_path: row.get(1)?,
        file_hash: row.get(2)?,
        file_size_bytes: row.get::<_, i64>(3)? as u64,
        modified_timestamp: row.get(4)?,
        title: row.get(5)?,
        artist: row.get(6)?,
        album: row.get(7)?,
        genre: row.get(8)?,
        duration_seconds: row.get(9)?,
        bpm: row.get(10)?,
        key: row.get(11)?,
        camelot: row.get(12)?,
        profile_json: row.get(13)?,
        ingest_status: IngestStatus::parse(&row.get::<_, String>(14)?).unwrap_or(IngestStatus::Ok),
        ingest_error: row
            .get::<_, Option<String>>(15)?
            .as_deref()
            .and_then(IngestErrorReason::parse),
        availability: Availability::parse(&row.get::<_, String>(16)?)
            .unwrap_or(Availability::Available),
        analysis_state: AnalysisState::parse(&row.get::<_, String>(17)?)
            .unwrap_or(AnalysisState::NotStarted),
        sample_rate: row.get(18)?,
        channels: row.get::<_, Option<i64>>(19)?.map(|c| c as u16),
        duplicate_of: row.get(20)?,
        ingest_seq: row.get(21)?,
        created_at: row.get(22)?,
        updated_at: row.get(23)?,
    })
}

/// On legacy databases a `None` upsert value is indistinguishable from a
/// previously written placeholder, so treat the pair as unchanged.
fn upsert_analysis_eq(legacy: bool, stored: Option<f64>, incoming: Option<f64>) -> bool {
    if legacy && incoming.is_none() && stored == Some(LEGACY_PLACEHOLDER_DURATION) {
        return true;
    }
    stored == incoming
}

fn upsert_track_locked(
    conn: &Connection,
    legacy: bool,
    row: &TrackUpsert,
) -> Result<UpsertOutcome, LibraryError> {
    let existing = get_track_by_path_locked(conn, &row.file_path)?;
    let outcome = match &existing {
        Some(stored) => {
            if upsert_fields_changed(legacy, stored, row) {
                UpsertOutcome::Changed
            } else {
                UpsertOutcome::Unchanged
            }
        }
        None => UpsertOutcome::Inserted,
    };
    if outcome == UpsertOutcome::Unchanged {
        return Ok(outcome);
    }

    let hash = legacy_or(legacy, row.file_hash.as_deref(), LEGACY_PLACEHOLDER_HASH);
    // `file_size_bytes` is a `u64` but the column is SQLite INTEGER (signed
    // 64-bit). rusqlite binds u64 values above `i64::MAX` as TEXT, which the
    // i64-typed read path cannot map back and would corrupt later reads.
    // No real file reaches 2^63 bytes (9.2 EiB), so saturate at the boundary
    // rather than failing the whole upsert.
    let size = row.file_size_bytes.min(i64::MAX as u64);
    let duration = legacy_or(legacy, row.duration_seconds, LEGACY_PLACEHOLDER_DURATION);
    // The remaining analysis columns (bpm/key/camelot/profile) are never
    // produced by ingest; absent → NULL (fresh) or placeholder (legacy).
    let bpm = legacy_or(legacy, None::<f64>, LEGACY_PLACEHOLDER_BPM);
    let key = legacy_or(legacy, None::<&str>, LEGACY_PLACEHOLDER_KEY);
    let camelot = legacy_or(legacy, None::<&str>, LEGACY_PLACEHOLDER_CAMELOT);
    let profile = legacy_or(legacy, None::<&str>, LEGACY_PLACEHOLDER_PROFILE);

    match outcome {
        UpsertOutcome::Changed => {
            let n = conn.execute(
                "UPDATE tracks SET
                    file_hash = ?1, file_size_bytes = ?2, modified_timestamp = ?3,
                    title = ?4, artist = ?5, album = ?6, genre = ?7,
                    duration_seconds = ?8, bpm = ?9, key = ?10, camelot = ?11, profile_json = ?12,
                    ingest_status = ?13, ingest_error = ?14, availability = ?15,
                    sample_rate = ?16, channels = ?17, updated_at = CURRENT_TIMESTAMP
                 WHERE file_path = ?18",
                rusqlite::params![
                    hash,
                    size,
                    row.modified_timestamp,
                    row.title,
                    row.artist,
                    row.album,
                    row.genre,
                    duration,
                    bpm,
                    key,
                    camelot,
                    profile,
                    row.ingest_status.as_str(),
                    row.ingest_error.map(IngestErrorReason::as_str),
                    row.availability.as_str(),
                    row.sample_rate,
                    row.channels,
                    row.file_path,
                ],
            )?;
            debug_assert!(n <= 1, "file_path is UNIQUE; at most one row matches");
        }
        UpsertOutcome::Inserted => {
            // `file_path` is UNIQUE and the lock is held, so this cannot
            // conflict; the id is random 16-byte hex and the seq is the
            // table max + 1 within the same atomic statement.
            conn.execute(
                "INSERT INTO tracks
                    (id, file_path, file_hash, file_size_bytes, modified_timestamp,
                     title, artist, album, genre, duration_seconds, bpm, key, camelot, profile_json,
                     ingest_status, ingest_error, availability, analysis_state,
                     sample_rate, channels, ingest_seq)
                 VALUES (
                    lower(hex(randomblob(16))),
                    ?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16,
                    'not_started', ?17, ?18,
                    (SELECT COALESCE(MAX(ingest_seq), 0) + 1 FROM tracks))",
                rusqlite::params![
                    row.file_path,
                    hash,
                    size,
                    row.modified_timestamp,
                    row.title,
                    row.artist,
                    row.album,
                    row.genre,
                    duration,
                    bpm,
                    key,
                    camelot,
                    profile,
                    row.ingest_status.as_str(),
                    row.ingest_error.map(IngestErrorReason::as_str),
                    row.availability.as_str(),
                    row.sample_rate,
                    row.channels,
                ],
            )?;
        }
        UpsertOutcome::Unchanged => {}
    }
    Ok(outcome)
}

/// Whether any ingest-managed column of `stored` differs from `row`.
fn upsert_fields_changed(legacy: bool, stored: &TrackRow, row: &TrackUpsert) -> bool {
    stored.file_size_bytes != row.file_size_bytes
        || stored.modified_timestamp != row.modified_timestamp
        || stored.title != row.title
        || stored.artist != row.artist
        || stored.album != row.album
        || stored.genre != row.genre
        || !upsert_analysis_eq(legacy, stored.duration_seconds, row.duration_seconds)
        || stored.sample_rate != row.sample_rate
        || stored.channels != row.channels
        || stored.ingest_status != row.ingest_status
        || stored.ingest_error != row.ingest_error
        || stored.availability != row.availability
}

fn update_track_locked(
    conn: &Connection,
    legacy: bool,
    id: &str,
    row: &TrackRow,
) -> Result<bool, LibraryError> {
    let hash = legacy_or(legacy, row.file_hash.as_deref(), LEGACY_PLACEHOLDER_HASH);
    // Same signed-INTEGER boundary as the upsert path: a `u64` ≥ 2^63 binds
    // as TEXT and would corrupt the i64-typed read path. No real file
    // reaches 9.2 EiB, so saturate at the boundary.
    let size = row.file_size_bytes.min(i64::MAX as u64);
    let duration = legacy_or(legacy, row.duration_seconds, LEGACY_PLACEHOLDER_DURATION);
    let bpm = legacy_or(legacy, row.bpm, LEGACY_PLACEHOLDER_BPM);
    let key = legacy_or(legacy, row.key.as_deref(), LEGACY_PLACEHOLDER_KEY);
    let camelot = legacy_or(legacy, row.camelot.as_deref(), LEGACY_PLACEHOLDER_CAMELOT);
    let profile = legacy_or(
        legacy,
        row.profile_json.as_deref(),
        LEGACY_PLACEHOLDER_PROFILE,
    );
    let n = conn.execute(
        "UPDATE tracks SET
            file_hash = ?1, file_size_bytes = ?2, modified_timestamp = ?3,
            title = ?4, artist = ?5, album = ?6, genre = ?7,
            duration_seconds = ?8, bpm = ?9, key = ?10, camelot = ?11, profile_json = ?12,
            ingest_status = ?13, ingest_error = ?14, availability = ?15,
            sample_rate = ?16, channels = ?17, duplicate_of = ?18, updated_at = CURRENT_TIMESTAMP
         WHERE id = ?19",
        rusqlite::params![
            hash,
            size,
            row.modified_timestamp,
            row.title,
            row.artist,
            row.album,
            row.genre,
            duration,
            bpm,
            key,
            camelot,
            profile,
            row.ingest_status.as_str(),
            row.ingest_error.map(IngestErrorReason::as_str),
            row.availability.as_str(),
            row.sample_rate,
            row.channels,
            row.duplicate_of,
            id,
        ],
    )?;
    Ok(n > 0)
}

/// `None` stays `None` on fresh databases (the new nullable columns hold the
/// truth); on legacy databases it becomes the NOT-NULL placeholder.
fn legacy_or<T>(legacy: bool, value: Option<T>, placeholder: T) -> Option<T>
where
    T: Copy + rusqlite::ToSql,
{
    value.or(legacy.then_some(placeholder))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::library::schema::LEGACY_CREATE_TRACKS;

    #[test]
    fn test_in_memory_library_cache_initialization() {
        let cache = LibraryCache::in_memory();
        assert!(cache.is_ok(), "LibraryCache failed to initialize in memory");
    }

    fn sample_upsert(path: &str) -> TrackUpsert {
        TrackUpsert {
            file_path: path.to_string(),
            file_hash: None,
            file_size_bytes: 100,
            modified_timestamp: 1_700_000_000,
            title: "T".to_string(),
            artist: "A".to_string(),
            album: None,
            genre: None,
            duration_seconds: Some(0.5),
            sample_rate: Some(22050),
            channels: Some(1),
            ingest_status: IngestStatus::Ok,
            ingest_error: None,
            availability: Availability::Available,
        }
    }

    #[test]
    fn test_upsert_insert_then_unchanged_then_changed() {
        let cache = LibraryCache::in_memory().unwrap();
        let upsert = sample_upsert("/tmp/song.wav");
        assert_eq!(
            cache.upsert_track(&upsert).unwrap(),
            UpsertOutcome::Inserted
        );
        // Same values again -> unchanged.
        assert_eq!(
            cache.upsert_track(&upsert).unwrap(),
            UpsertOutcome::Unchanged
        );
        // Different title -> changed.
        let mut changed = upsert.clone();
        changed.title = "New Title".to_string();
        assert_eq!(
            cache.upsert_track(&changed).unwrap(),
            UpsertOutcome::Changed
        );
        // Row is readable back with a stable id and seq 1; fresh DB keeps
        // the hash NULL until applied.
        let row = cache.get_track_by_path("/tmp/song.wav").unwrap().unwrap();
        assert_eq!(row.title, "New Title");
        assert_eq!(row.ingest_seq, 1);
        assert_eq!(row.file_hash, None);
    }

    #[test]
    fn test_apply_hash_and_duplicate_links() {
        let cache = LibraryCache::in_memory().unwrap();
        let a = sample_upsert("/tmp/a.wav");
        let mut b = sample_upsert("/tmp/b.wav");
        b.title = "B".to_string();
        cache.upsert_track(&a).unwrap();
        cache.upsert_track(&b).unwrap();
        let a_id = cache.get_track_by_path("/tmp/a.wav").unwrap().unwrap().id;
        let b_id = cache.get_track_by_path("/tmp/b.wav").unwrap().unwrap().id;
        assert_ne!(a_id, b_id);
        assert!(cache.apply_hash(&a_id, "hash1").unwrap());
        assert!(cache.apply_hash(&b_id, "hash1").unwrap());
        let links = cache.hash_rows().unwrap();
        assert_eq!(links.len(), 2);
        cache
            .apply_duplicate_links(&[(a_id.clone(), None), (b_id.clone(), Some(a_id.clone()))])
            .unwrap();
        assert_eq!(cache.get_track(&a_id).unwrap().unwrap().duplicate_of, None);
        assert_eq!(
            cache.get_track(&b_id).unwrap().unwrap().duplicate_of,
            Some(a_id)
        );
    }

    #[test]
    fn test_mark_missing_and_folders() {
        let cache = LibraryCache::in_memory().unwrap();
        assert_eq!(cache.add_folder("/Music").unwrap(), "/Music");
        assert_eq!(cache.add_folder("/Music").unwrap(), "/Music");
        assert_eq!(cache.folder_paths().unwrap(), vec!["/Music".to_string()]);
        let upsert = sample_upsert("/tmp/song.wav");
        cache.upsert_track(&upsert).unwrap();
        let id = cache
            .get_track_by_path("/tmp/song.wav")
            .unwrap()
            .unwrap()
            .id;
        assert_eq!(cache.mark_missing(std::slice::from_ref(&id)).unwrap(), 1);
        assert_eq!(
            cache.get_track(&id).unwrap().unwrap().availability,
            Availability::Missing
        );
    }

    /// `next_ingest_seq` starts at 1 on an empty table, and inserted rows
    /// receive a strictly monotonically increasing contiguous sequence.
    #[test]
    fn test_ingest_seq_monotonic() {
        let cache = LibraryCache::in_memory().unwrap();
        assert_eq!(cache.next_ingest_seq().unwrap(), 1);
        for i in 1..=3 {
            cache
                .upsert_track(&sample_upsert(&format!("/s/{i}.wav")))
                .unwrap();
        }
        assert_eq!(cache.next_ingest_seq().unwrap(), 4);
        let mut seqs: Vec<i64> = cache
            .all_tracks()
            .unwrap()
            .iter()
            .map(|r| r.ingest_seq)
            .collect();
        seqs.sort_unstable();
        assert_eq!(seqs, vec![1, 2, 3]);
    }

    /// Extreme but legal column values round-trip through the DB intact:
    /// `i64::MAX` size (the true storage boundary — `u64` values above
    /// `i64::MAX` cannot round-trip `SQLite`'s signed INTEGER and reading
    /// them back errors), `i64::MAX` timestamp, `f64::INFINITY` duration,
    /// `u32::MAX` sample rate, `u16::MAX` channels, a 100 KB title.
    #[test]
    fn test_upsert_extreme_values_roundtrip() {
        let cache = LibraryCache::in_memory().unwrap();
        let mut u = sample_upsert("/boundary/x.wav");
        u.file_size_bytes = i64::MAX as u64;
        u.modified_timestamp = i64::MAX;
        u.duration_seconds = Some(f64::INFINITY);
        u.sample_rate = Some(u32::MAX);
        u.channels = Some(u16::MAX);
        u.title = "a".repeat(100_000);
        assert_eq!(cache.upsert_track(&u).unwrap(), UpsertOutcome::Inserted);
        let row = cache.get_track_by_path("/boundary/x.wav").unwrap().unwrap();
        assert_eq!(row.file_size_bytes, i64::MAX as u64);
        assert_eq!(row.modified_timestamp, i64::MAX);
        assert_eq!(row.duration_seconds, Some(f64::INFINITY));
        assert_eq!(row.sample_rate, Some(u32::MAX));
        assert_eq!(row.channels, Some(u16::MAX));
        assert_eq!(row.title.len(), 100_000);
        // Identical re-upsert: unchanged (stable equality on every column).
        assert_eq!(cache.upsert_track(&u).unwrap(), UpsertOutcome::Unchanged);
        // A one-tick timestamp change: changed.
        let mut u2 = u.clone();
        u2.modified_timestamp = i64::MAX - 1;
        assert_eq!(cache.upsert_track(&u2).unwrap(), UpsertOutcome::Changed);
    }

    /// Sizes above the signed-INTEGER boundary (`u64` values ≥ 2^63, i.e.
    /// 9.2 EiB files — physically impossible) saturate to `i64::MAX` at the
    /// upsert boundary instead of binding as TEXT and breaking the i64-typed
    /// read path; `all_tracks()` must keep working.
    #[test]
    fn test_upsert_oversized_saturates() {
        let cache = LibraryCache::in_memory().unwrap();
        let mut u = sample_upsert("/boundary/huge.wav");
        u.file_size_bytes = i64::MAX as u64 + 1;
        assert_eq!(cache.upsert_track(&u).unwrap(), UpsertOutcome::Inserted);
        let mut m = sample_upsert("/boundary/max.wav");
        m.file_size_bytes = u64::MAX;
        assert_eq!(cache.upsert_track(&m).unwrap(), UpsertOutcome::Inserted);
        let rows = cache.all_tracks().unwrap();
        assert_eq!(
            rows.len(),
            2,
            "all_tracks must survive oversized rows: {rows:?}"
        );
        for row in &rows {
            assert_eq!(row.file_size_bytes, i64::MAX as u64);
        }
    }

    /// `update_track` shares the upsert boundary rule: a `file_size_bytes`
    /// ≥ 2^63 (physically impossible; 9.2 EiB) saturates at `i64::MAX`
    /// instead of binding as TEXT and corrupting the i64-typed read path,
    /// so `all_tracks()` must keep working.
    #[test]
    fn test_update_oversized_saturates() {
        let cache = LibraryCache::in_memory().unwrap();
        cache.upsert_track(&sample_upsert("/u/x.wav")).unwrap();
        let id = cache.get_track_by_path("/u/x.wav").unwrap().unwrap().id;
        let mut row = cache.get_track(&id).unwrap().unwrap();
        row.file_size_bytes = i64::MAX as u64 + 1;
        assert!(cache.update_track(&id, &row).unwrap());
        let rows = cache.all_tracks().unwrap();
        assert_eq!(rows.len(), 1, "all_tracks must survive the update");
        assert_eq!(rows[0].file_size_bytes, i64::MAX as u64);
    }

    /// `apply_hash` edges: unknown id → false (no error); the legacy
    /// `''` placeholder is a legal value but is excluded from the dedup
    /// input (`hash_rows`); a real hash re-includes the row.
    #[test]
    fn test_apply_hash_edges() {
        let cache = LibraryCache::in_memory().unwrap();
        assert!(!cache.apply_hash("ghost", "h").unwrap());
        let up = sample_upsert("/h/x.wav");
        cache.upsert_track(&up).unwrap();
        let id = cache.get_track_by_path("/h/x.wav").unwrap().unwrap().id;
        assert!(cache.apply_hash(&id, "").unwrap());
        assert!(
            cache.hash_rows().unwrap().is_empty(),
            "the '' placeholder is excluded from dedup"
        );
        assert!(cache.apply_hash(&id, "real").unwrap());
        assert_eq!(cache.hash_rows().unwrap().len(), 1);
    }

    /// `mark_missing` is idempotent and safe on empty/unknown ids.
    #[test]
    fn test_mark_missing_idempotent() {
        let cache = LibraryCache::in_memory().unwrap();
        assert_eq!(cache.mark_missing(&[]).unwrap(), 0);
        assert_eq!(cache.mark_missing(&["ghost".to_string()]).unwrap(), 0);
        let up = sample_upsert("/m/x.wav");
        cache.upsert_track(&up).unwrap();
        let id = cache.get_track_by_path("/m/x.wav").unwrap().unwrap().id;
        assert_eq!(cache.mark_missing(std::slice::from_ref(&id)).unwrap(), 1);
        assert_eq!(cache.mark_missing(std::slice::from_ref(&id)).unwrap(), 1);
        let row = cache.get_track(&id).unwrap().unwrap();
        assert_eq!(row.availability, Availability::Missing);
    }

    /// `apply_duplicate_links` edges: an empty batch is a safe no-op;
    /// unknown ids are ignored; a `None` link clears a stored link.
    #[test]
    fn test_apply_duplicate_links_edges() {
        let cache = LibraryCache::in_memory().unwrap();
        assert_eq!(cache.apply_duplicate_links(&[]).unwrap(), 0);
        assert_eq!(
            cache
                .apply_duplicate_links(&[("ghost".to_string(), None)])
                .unwrap(),
            1
        );
        let a = sample_upsert("/d/a.wav");
        let mut b = sample_upsert("/d/b.wav");
        b.title = "B".to_string();
        cache.upsert_track(&a).unwrap();
        cache.upsert_track(&b).unwrap();
        let a_id = cache.get_track_by_path("/d/a.wav").unwrap().unwrap().id;
        let b_id = cache.get_track_by_path("/d/b.wav").unwrap().unwrap().id;
        cache
            .apply_duplicate_links(&[(b_id.clone(), Some(a_id.clone()))])
            .unwrap();
        assert_eq!(
            cache.get_track(&b_id).unwrap().unwrap().duplicate_of,
            Some(a_id.clone())
        );
        // Clear the link.
        cache
            .apply_duplicate_links(&[(b_id.clone(), None)])
            .unwrap();
        assert_eq!(cache.get_track(&b_id).unwrap().unwrap().duplicate_of, None);
    }

    /// `add_folder` registry: empty-path and duplicate registrations.
    #[test]
    fn test_folder_registry_edges() {
        let cache = LibraryCache::in_memory().unwrap();
        assert_eq!(cache.add_folder("").unwrap(), "");
        assert_eq!(cache.add_folder("/Music").unwrap(), "/Music");
        assert_eq!(
            cache.add_folder("/Music").unwrap(),
            "/Music",
            "re-registration is idempotent"
        );
        let paths = cache.folder_paths().unwrap();
        assert_eq!(paths, vec![String::new(), "/Music".to_string()]);
    }

    /// Legacy (pre-v1) database opened through the real `open` path:
    /// `None` analysis values become NOT-NULL placeholders, and a `None`
    /// duration re-upsert compares equal to the `0.0` placeholder
    /// (unchanged), while a real duration is a change.
    #[test]
    fn test_legacy_placeholders() {
        // Simulate a pre-migration DB file, then open it through the real
        // LibraryCache::open path (migrate + legacy detection).
        let dir = std::env::temp_dir().join(format!(
            "pulse-lib-{}-legacy-{}",
            std::process::id(),
            std::process::id()
        ));
        std::fs::create_dir_all(&dir).unwrap();
        let db = dir.join("legacy.db");
        let _ = std::fs::remove_file(&db);
        {
            let conn = Connection::open(&db).unwrap();
            conn.execute_batch(LEGACY_CREATE_TRACKS).unwrap();
            conn.pragma_update(None, "user_version", 0).unwrap();
        }

        let cache = LibraryCache::open(&db).unwrap();
        assert!(
            cache.legacy_strict(),
            "legacy table must be detected at open time"
        );
        let mut upsert = sample_upsert("/legacy/song.wav");
        upsert.duration_seconds = None; // unparseable row
        cache.upsert_track(&upsert).unwrap();
        let row = cache
            .get_track_by_path("/legacy/song.wav")
            .unwrap()
            .unwrap();
        // Legacy NOT NULL columns received placeholders, not NULLs.
        assert_eq!(row.file_hash.as_deref(), Some(LEGACY_PLACEHOLDER_HASH));
        assert_eq!(row.duration_seconds, Some(LEGACY_PLACEHOLDER_DURATION));
        // The new nullable columns still hold the truth.
        assert_eq!(row.sample_rate, Some(22050));
        assert_eq!(row.ingest_status, IngestStatus::Ok);
        let _ = std::fs::remove_dir_all(&dir);
    }

    /// Legacy upsert equality: `None` analysis values compare equal to the
    /// placeholders they were stored as, so a repeated `None` upsert is
    /// `Unchanged` (no write); a real duration is a `Changed` write.
    #[test]
    fn test_legacy_placeholder_equality() {
        use super::super::schema::LEGACY_CREATE_TRACKS;
        let dir = std::env::temp_dir().join(format!("pulse-lib-{}-legacy-eq", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let db = dir.join("legacy.db");
        {
            let conn = Connection::open(&db).unwrap();
            conn.execute_batch(LEGACY_CREATE_TRACKS).unwrap();
            conn.pragma_update(None, "user_version", 0).unwrap();
        }
        let cache = LibraryCache::open(&db).unwrap();
        assert!(cache.legacy_strict());
        let mut up = sample_upsert("/le/x.wav");
        up.duration_seconds = None;
        assert_eq!(cache.upsert_track(&up).unwrap(), UpsertOutcome::Inserted);
        assert_eq!(
            cache.upsert_track(&up).unwrap(),
            UpsertOutcome::Unchanged,
            "None must compare equal to the stored 0.0 placeholder"
        );
        let mut up2 = up.clone();
        up2.duration_seconds = Some(300.0);
        assert_eq!(cache.upsert_track(&up2).unwrap(), UpsertOutcome::Changed);
        let row = cache.get_track_by_path("/le/x.wav").unwrap().unwrap();
        assert_eq!(row.duration_seconds, Some(300.0));
        let _ = std::fs::remove_dir_all(&dir);
    }

    /// Fresh (v1) databases: `None` values stay `NULL` (the nullable
    /// columns are the source of truth), and a repeated `None` upsert is
    /// still `Unchanged`.
    #[test]
    fn test_fresh_null_equality() {
        let cache = LibraryCache::in_memory().unwrap();
        assert!(!cache.legacy_strict());
        let mut up = sample_upsert("/fr/x.wav");
        up.duration_seconds = None;
        up.sample_rate = None;
        up.channels = None;
        assert_eq!(cache.upsert_track(&up).unwrap(), UpsertOutcome::Inserted);
        let row = cache.get_track_by_path("/fr/x.wav").unwrap().unwrap();
        assert_eq!(row.duration_seconds, None, "fresh DB keeps NULL");
        assert_eq!(row.file_hash, None);
        assert_eq!(cache.upsert_track(&up).unwrap(), UpsertOutcome::Unchanged);
        // 0.0 is a *value* on a fresh DB, distinct from NULL: a change.
        let mut up2 = up.clone();
        up2.duration_seconds = Some(0.0);
        assert_eq!(cache.upsert_track(&up2).unwrap(), UpsertOutcome::Changed);
    }
}
