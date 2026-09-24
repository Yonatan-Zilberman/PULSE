//! The [`LibraryStore`]: owns the ingest state machine, the SHA-256 hash
//! queue, and the duplicate-linking pass. Managed as `Arc<LibraryStore>` in
//! Tauri state.
//!
//! **Invariants**
//! - All row-state transitions flow through the pure [`derive_row_state`]
//!   (no ad-hoc SQL state writes elsewhere).
//! - Content hashing never happens on a Tauri command's critical path: new
//!   and re-tagged files are queued and hashed by the background worker
//!   ([`LibraryStore::spawn_hash_worker`]) or synchronously in tests
//!   ([`LibraryStore::flush_hashes`]).
//! - A content change invalidates the stored hash in the *same durable*
//!   write (`file_hash = NULL` / legacy `''` = "unhashed"): the hash queue
//!   is in-memory, so if the app exits before the worker drains it, the row
//!   must not keep a hash of stale content. "Unhashed" is self-healing: the
//!   dedup pass excludes such rows and the next refresh re-queues them.
//! - The dedup pass is a pure function of the current row set
//!   ([`compute_duplicate_links`]) applied in one transaction: two runs over
//!   the same state produce identical writes (idempotent).

// Public methods unwrap the hash-queue lock guard; a poisoned mutex means
// the process is already unwinding, so panicking is the sound behavior.
#![allow(clippy::missing_panics_doc)]

use std::collections::{HashMap, HashSet};
use std::io::Read;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};

use sha2::{Digest, Sha256};

use super::cache::LibraryCache;
use super::errors::LibraryError;
use super::scanner::{fs_state, scan_folder, validate_folder, FsState};
use super::tag_reader::{read as read_tags, AudioMetadata, TagReadError};
use super::types::{
    Availability, DedupSummary, IngestErrorReason, IngestStatus, ReportEntry, ScanReport, TrackRow,
    TrackSummary, TrackUpsert,
};

/// A queued SHA-256 computation.
#[derive(Debug, Clone)]
struct HashJob {
    id: String,
    file_path: String,
}

pub struct LibraryStore {
    cache: LibraryCache,
    hash_queue: Mutex<Vec<HashJob>>,
}

impl LibraryStore {
    pub fn new(cache: LibraryCache) -> Self {
        Self {
            cache,
            hash_queue: Mutex::new(Vec::new()),
        }
    }

    /// Access the underlying cache (test hook).
    #[cfg(test)]
    #[doc(hidden)]
    pub(crate) fn cache(&self) -> &LibraryCache {
        &self.cache
    }

    /// Register `path` as a library folder (idempotent) and ingest its
    /// contents: tag reads happen here; content hashing is queued for the
    /// background worker.
    pub fn add_folder(&self, path: &str) -> Result<ScanReport, LibraryError> {
        let canonical = validate_folder(path)?;
        self.cache
            .add_folder(canonical.to_string_lossy().as_ref())?;
        let files = scan_folder(&canonical)
            .into_iter()
            .map(|f| {
                (
                    f.path,
                    FsState::Present {
                        size: f.size,
                        mtime: f.mtime,
                    },
                )
            })
            .collect::<Vec<_>>();
        run_scan(self, &files, false)
    }

    /// Re-validate every registered folder and every indexed row (including
    /// rows whose file vanished, so they get lazily re-checked) with
    /// `retry_errored = true`: errored rows are retried for re-tagging.
    pub fn refresh(&self) -> Result<ScanReport, LibraryError> {
        let rows = self.cache.all_tracks()?;
        // Deduplicate with a set (O(n)); a linear scan per row would be
        // O(n²) at 10k rows × 10k folder files.
        let mut paths: Vec<PathBuf> = Vec::new();
        let mut seen: HashSet<PathBuf> = HashSet::new();
        for folder in self.cache.folder_paths()? {
            for f in scan_folder(Path::new(&folder)) {
                if seen.insert(f.path.clone()) {
                    paths.push(f.path);
                }
            }
        }
        for row in &rows {
            let p = PathBuf::from(&row.file_path);
            if seen.insert(p.clone()) {
                paths.push(p);
            }
        }
        paths.sort();
        paths.dedup();
        let files = paths
            .iter()
            .map(|p| (p.clone(), fs_state(p)))
            .collect::<Vec<_>>();
        run_scan(self, &files, true)
    }

    /// All tracks as IPC projections, sorted by `file_path`, with lazy
    /// missing-marking: any row whose file vanished is updated to
    /// `availability = 'missing'` in the DB *and* in the returned
    /// projection.
    pub fn list(&self) -> Result<Vec<TrackSummary>, LibraryError> {
        let rows = self.cache.all_tracks()?;
        let mut missing_ids: Vec<String> = Vec::new();
        let summaries = rows
            .iter()
            .map(|row| {
                let mut summary = TrackSummary::from(row);
                if row.availability == Availability::Available
                    && fs_state(Path::new(&row.file_path)) == FsState::Missing
                {
                    missing_ids.push(row.id.clone());
                    summary.availability = Availability::Missing;
                }
                summary
            })
            .collect::<Vec<_>>();
        self.cache.mark_missing(&missing_ids)?;
        Ok(summaries)
    }

    /// One track by id, with the same lazy missing-marking as [`Self::list`].
    pub fn get(&self, id: &str) -> Result<TrackSummary, LibraryError> {
        let row = self
            .cache
            .get_track(id)?
            .ok_or_else(|| LibraryError::Db(format!("track {id} not found")))?;
        let mut summary = TrackSummary::from(&row);
        if row.availability == Availability::Available
            && fs_state(Path::new(&row.file_path)) == FsState::Missing
        {
            self.cache.mark_missing(&[id.to_string()])?;
            summary.availability = Availability::Missing;
        }
        Ok(summary)
    }

    /// Deterministic hash path (tests, and any sync context): drain the hash
    /// queue, compute each SHA-256 directly (no runtime), apply the hashes,
    /// then run one dedup pass.
    pub fn flush_hashes(&self) -> Result<DedupSummary, LibraryError> {
        let jobs = {
            let mut queue = self.hash_queue.lock().unwrap();
            std::mem::take(&mut *queue)
        };
        // Drop duplicate jobs for the same row (keep the first occurrence;
        // jobs carry no payload beyond `(id, path)`, so order is irrelevant).
        let mut seen: Vec<String> = Vec::new();
        let jobs: Vec<HashJob> = jobs
            .into_iter()
            .filter(|j| {
                if seen.contains(&j.id) {
                    false
                } else {
                    seen.push(j.id.clone());
                    true
                }
            })
            .collect();
        for job in &jobs {
            // A file that vanished mid-flush keeps its old/absent hash.
            if let Ok(hash) = sha256_file(Path::new(&job.file_path)) {
                self.cache.apply_hash(&job.id, &hash)?;
            }
        }
        dedup_pass(self)
    }

    /// Background worker: on a 250 ms interval drain the queue, compute each
    /// SHA-256 on a blocking thread (never the command path), then apply the
    /// hashes plus one dedup pass. Must be called with `Arc<Self>`.
    pub fn spawn_hash_worker(self: &Arc<Self>) {
        let handle = tauri::async_runtime::TokioHandle::current();
        let store = Arc::clone(self);
        handle.spawn(async move {
            let mut interval = tokio::time::interval(std::time::Duration::from_millis(250));
            loop {
                interval.tick().await;
                let jobs = {
                    let mut queue = store.hash_queue.lock().unwrap();
                    std::mem::take(&mut *queue)
                };
                if jobs.is_empty() {
                    continue;
                }
                for job in &jobs {
                    let path = PathBuf::from(job.file_path.clone());
                    let job_id = job.id.clone();
                    let hash = tokio::task::spawn_blocking(move || sha256_file(&path)).await;
                    // File vanished or hash failed: keep the old hash.
                    if let Ok(Ok(h)) = hash {
                        if store.cache.apply_hash(&job_id, &h).is_err() {
                            return; // DB is gone; stop the worker
                        }
                    }
                }
                if dedup_pass(&store).is_err() {
                    return;
                }
            }
        });
    }
}

// ----------------------------------------------------------------------
// Scan / state machine
// ----------------------------------------------------------------------

/// Run the ingest state machine over a set of `(path, fs_state)` pairs.
fn run_scan(
    store: &LibraryStore,
    files: &[(PathBuf, FsState)],
    retry_errored: bool,
) -> Result<ScanReport, LibraryError> {
    let rows = store.cache.all_tracks()?;
    let by_path: HashMap<&str, &TrackRow> =
        rows.iter().map(|r| (r.file_path.as_str(), r)).collect();
    let mut report = ScanReport::default();
    for (path, fs) in files {
        let path_str = path.to_string_lossy();
        let stored = by_path.get(path_str.as_ref()).copied();
        let transition = derive_row_state(stored, path, *fs, retry_errored, &tag_outcome);
        apply_transition(store, stored, &transition, &mut report)?;
    }
    let dedup = dedup_pass(store)?;
    report.duplicates = dedup.duplicates;
    report.duplicate_entries = dedup.entries;
    Ok(report)
}

/// Apply one transition to the cache and accumulate the report counters.
fn apply_transition(
    store: &LibraryStore,
    stored: Option<&TrackRow>,
    t: &RowTransition,
    report: &mut ScanReport,
) -> Result<(), LibraryError> {
    match t.change {
        RowChange::Unchanged => {
            if t.enqueue_hash {
                // Self-heal: the row is stable but unhashed (its earlier hash
                // job never drained before app exit) — re-queue it.
                if let Some(row) = stored {
                    enqueue_hash(store, &row.id, &row.file_path);
                }
            }
            report.unchanged += 1;
            if t.errored_row {
                report.errored += 1;
                report.errored_entries.push(errored_entry(stored, t));
            }
        }
        RowChange::Missing => {
            if let Some(row) = stored {
                store.cache.mark_missing(std::slice::from_ref(&row.id))?;
            }
            report.missing += 1;
        }
        RowChange::Inserted | RowChange::Updated => {
            let upsert = t
                .upsert
                .as_ref()
                .ok_or_else(|| LibraryError::Db("transition missing upsert payload".to_string()))?;
            store.cache.upsert_track(upsert)?;
            if t.change == RowChange::Inserted {
                report.added += 1;
            } else {
                report.updated += 1;
            }
            // Resolve the (possibly just-inserted) row: fresh rows have no
            // `stored` reference, and both the error entry and the hash job
            // need the real id/file_path.
            let row = store.cache.get_track_by_path(&upsert.file_path)?;
            let resolved = row.as_ref().or(stored);
            if upsert.ingest_status == IngestStatus::Error {
                report.errored += 1;
                report.errored_entries.push(errored_entry(resolved, t));
            }
            if t.enqueue_hash {
                if let Some(row) = resolved {
                    enqueue_hash(store, &row.id, &row.file_path);
                }
            }
        }
    }
    Ok(())
}

fn errored_entry(stored: Option<&TrackRow>, t: &RowTransition) -> ReportEntry {
    let id = stored.map(|r| r.id.clone()).unwrap_or_default();
    let path = stored.map(|r| r.file_path.clone()).unwrap_or_default();
    let reason = match t.upsert.as_ref().and_then(|u| u.ingest_error) {
        Some(r) => r.as_str().to_string(),
        None => "unknown".to_string(),
    };
    ReportEntry {
        id,
        file_path: path,
        reason,
    }
}

/// SHA-256 of a file, hex-encoded, streamed in 64 KiB chunks.
pub fn sha256_file(path: &Path) -> Result<String, LibraryError> {
    use std::fmt::Write;

    let file = std::fs::File::open(path)?;
    let mut reader = std::io::BufReader::new(file);
    let mut hasher = Sha256::new();
    let mut buf: Vec<u8> = vec![0; 64 * 1024];
    loop {
        let n = reader.read(&mut buf).map_err(LibraryError::from)?;
        if n == 0 {
            break;
        }
        hasher.update(&buf[..n]);
    }
    let digest = hasher.finalize();
    let hex = digest
        .iter()
        .fold(String::with_capacity(64), |mut out, byte| {
            let _ = out.write_fmt(format_args!("{byte:02x}"));
            out
        });
    Ok(hex)
}

fn tag_outcome(path: &Path) -> TagOutcome {
    match read_tags(path) {
        Ok(meta) => TagOutcome::Read(meta),
        Err(err) => TagOutcome::Failed(err.into()),
    }
}

// ----------------------------------------------------------------------
// Pure state machine
// ----------------------------------------------------------------------

/// Result of reading a file's tags during a transition.
#[derive(Debug, Clone, PartialEq)]
pub enum TagOutcome {
    Read(AudioMetadata),
    Failed(IngestErrorReason),
}

impl From<TagReadError> for IngestErrorReason {
    fn from(err: TagReadError) -> Self {
        match err {
            TagReadError::PermissionDenied(_) => Self::PermissionDenied,
            TagReadError::MissingFile(_) => Self::MissingFile,
            TagReadError::UnreadableFile(_) => Self::UnreadableFile,
            TagReadError::Corrupt(_) => Self::CorruptContainer,
        }
    }
}

/// Which visible change a transition applies.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RowChange {
    /// Nothing changes in the row.
    Unchanged,
    /// The row is newly inserted.
    Inserted,
    /// The row's state/metadata changed.
    Updated,
    /// The row is marked `availability = 'missing'` (only that field).
    Missing,
}

/// The outcome of [`derive_row_state`]: what to persist for one row.
#[derive(Debug, Clone, PartialEq)]
pub struct RowTransition {
    pub change: RowChange,
    /// Payload to persist for `Inserted`/`Updated`; `None` for the other
    /// changes (missing-mark goes through `mark_missing`).
    pub upsert: Option<TrackUpsert>,
    /// Queue the file's SHA-256 after the write (hash is stale or new).
    pub enqueue_hash: bool,
    /// `true` when a refresh retry re-read an errored row and it failed with
    /// the *same* reason: state is unchanged (idempotent) but the row still
    /// counts as errored in the report.
    pub errored_row: bool,
}

impl RowTransition {
    fn unchanged() -> Self {
        Self {
            change: RowChange::Unchanged,
            upsert: None,
            enqueue_hash: false,
            errored_row: false,
        }
    }
}

/// Pure (row × filesystem state) transition table.
///
/// `read` performs the tag read when (and only when) the transition needs
/// one; it must be side-effect free with respect to the store.
///
/// - `Missing` → mark missing (only that field; `duplicate_of`/`ingest_*`
///   untouched). A row already missing stays unchanged.
/// - `Unreachable` → unchanged: a stat failure that is not `ENOENT` (e.g.
///   permission) is not evidence of deletion.
/// - `Present` + size/mtime unchanged + stored available → unchanged.
///   Exception: `retry_errored` (refresh) with `ingest_status = error` →
///   re-read; same failure reason ⇒ state unchanged (idempotent).
/// - `Present` + unchanged + stored missing → restore `available` (no re-tag).
/// - `Present` + size/mtime changed → re-read tags (success ⇒ new metadata;
///   failure ⇒ fallbacks + error) and re-hash.
/// - New path → read tags (success ⇒ insert `ok` row; failure ⇒ insert error
///   row with `duration = NULL` — duration must come from the file, never be
///   fabricated) and hash.
pub fn derive_row_state(
    stored: Option<&TrackRow>,
    path: &Path,
    fs: FsState,
    retry_errored: bool,
    read: &dyn Fn(&Path) -> TagOutcome,
) -> RowTransition {
    match fs {
        FsState::Missing => derive_missing(stored),
        FsState::Unreachable => RowTransition::unchanged(),
        FsState::Present { size, mtime } => {
            derive_present(stored, path, size, mtime, retry_errored, read)
        }
    }
}

/// A vanished file: only an `available` row transitions (to `missing`).
fn derive_missing(stored: Option<&TrackRow>) -> RowTransition {
    match stored {
        Some(row) if row.availability == Availability::Available => RowTransition {
            change: RowChange::Missing,
            upsert: None,
            enqueue_hash: false,
            errored_row: false,
        },
        // Already missing (or nothing indexed) — nothing to do.
        _ => RowTransition::unchanged(),
    }
}

fn derive_present(
    stored: Option<&TrackRow>,
    path: &Path,
    size: u64,
    mtime: i64,
    retry_errored: bool,
    read: &dyn Fn(&Path) -> TagOutcome,
) -> RowTransition {
    let unchanged_fields =
        stored.is_some_and(|r| r.file_size_bytes == size && r.modified_timestamp == mtime);
    let still_missing = stored.is_some_and(|r| r.availability == Availability::Missing);
    match stored {
        None => derive_new_file(path, size, mtime, read),
        Some(row) if unchanged_fields && !still_missing => {
            derive_stable(row, path, size, mtime, retry_errored, read)
        }
        Some(row) if unchanged_fields => derive_restored(row),
        Some(row) => derive_changed(row, path, size, mtime, read),
    }
}

/// A file that is not indexed yet: read its tags and insert the row.
fn derive_new_file(
    path: &Path,
    size: u64,
    mtime: i64,
    read: &dyn Fn(&Path) -> TagOutcome,
) -> RowTransition {
    match read(path) {
        TagOutcome::Read(meta) => RowTransition {
            change: RowChange::Inserted,
            upsert: Some(ok_upsert(path, size, mtime, None, &meta)),
            enqueue_hash: true,
            errored_row: false,
        },
        // Vanished between stat and read; there is no row.
        TagOutcome::Failed(IngestErrorReason::MissingFile) => RowTransition::unchanged(),
        TagOutcome::Failed(reason) => RowTransition {
            change: RowChange::Inserted,
            upsert: Some(error_upsert(path, size, mtime, None, reason)),
            enqueue_hash: false,
            errored_row: false,
        },
    }
}

/// Size+mtime unchanged and the row is `available`: unchanged — except a
/// refresh retry of an errored row, which re-reads the tags.
fn derive_stable(
    row: &TrackRow,
    path: &Path,
    size: u64,
    mtime: i64,
    retry_errored: bool,
    read: &dyn Fn(&Path) -> TagOutcome,
) -> RowTransition {
    if !(retry_errored && row.ingest_status == IngestStatus::Error) {
        // Self-heal: a stable row that is still unhashed (its hash job never
        // drained before app exit) must be re-queued.
        let unhashed = row.file_hash.as_deref().unwrap_or("").is_empty();
        return RowTransition {
            change: RowChange::Unchanged,
            upsert: None,
            enqueue_hash: unhashed,
            errored_row: false,
        };
    }
    match read(path) {
        TagOutcome::Read(meta) => RowTransition {
            change: RowChange::Updated,
            upsert: Some(ok_upsert(
                path,
                size,
                mtime,
                row.file_hash.as_deref(),
                &meta,
            )),
            enqueue_hash: true,
            errored_row: false,
        },
        TagOutcome::Failed(IngestErrorReason::MissingFile) => RowTransition {
            change: RowChange::Missing,
            upsert: None,
            enqueue_hash: false,
            errored_row: false,
        },
        TagOutcome::Failed(reason) => {
            if row.ingest_error == Some(reason) {
                // Same failure as before: state is unchanged so results stay
                // idempotent, but the row still counts as errored.
                RowTransition {
                    change: RowChange::Unchanged,
                    upsert: None,
                    enqueue_hash: false,
                    errored_row: true,
                }
            } else {
                RowTransition {
                    change: RowChange::Updated,
                    upsert: Some(error_upsert(
                        path,
                        size,
                        mtime,
                        row.file_hash.as_deref(),
                        reason,
                    )),
                    enqueue_hash: true,
                    errored_row: false,
                }
            }
        }
    }
}

/// The file is back with identical size+mtime after being missing: restore
/// availability without re-tagging.
fn derive_restored(row: &TrackRow) -> RowTransition {
    let mut upsert = upsert_from_row(row);
    upsert.availability = Availability::Available;
    RowTransition {
        change: RowChange::Updated,
        upsert: Some(upsert),
        enqueue_hash: false,
        errored_row: false,
    }
}

/// Size or mtime differs (or availability changed with a changed file):
/// re-read the tags and re-hash.
///
/// The stored hash is by definition stale (the content changed), so it is
/// invalidated in the same write (`None` → NULL/`''`): if the re-hash job
/// never drains (app exit), the row is "unhashed" and self-heals on the next
/// refresh — never a hash of different content.
fn derive_changed(
    _row: &TrackRow,
    path: &Path,
    size: u64,
    mtime: i64,
    read: &dyn Fn(&Path) -> TagOutcome,
) -> RowTransition {
    match read(path) {
        TagOutcome::Read(meta) => RowTransition {
            change: RowChange::Updated,
            upsert: Some(ok_upsert(path, size, mtime, None, &meta)),
            enqueue_hash: true,
            errored_row: false,
        },
        TagOutcome::Failed(IngestErrorReason::MissingFile) => RowTransition {
            change: RowChange::Missing,
            upsert: None,
            enqueue_hash: false,
            errored_row: false,
        },
        TagOutcome::Failed(reason) => RowTransition {
            change: RowChange::Updated,
            upsert: Some(error_upsert(path, size, mtime, None, reason)),
            enqueue_hash: true,
            errored_row: false,
        },
    }
}

/// A successfully-read file becomes/refreshes an `ok` row.
fn ok_upsert(
    path: &Path,
    size: u64,
    mtime: i64,
    stored_hash: Option<&str>,
    meta: &AudioMetadata,
) -> TrackUpsert {
    TrackUpsert {
        file_path: path.to_string_lossy().into_owned(),
        file_hash: stored_hash.map(str::to_string),
        file_size_bytes: size,
        modified_timestamp: mtime,
        title: meta.title.clone(),
        artist: meta.artist.clone(),
        album: meta.album.clone(),
        genre: meta.genre.clone(),
        duration_seconds: meta.duration_seconds,
        sample_rate: meta.sample_rate,
        channels: meta.channels,
        ingest_status: IngestStatus::Ok,
        ingest_error: None,
        availability: Availability::Available,
    }
}

/// An unreadable/unparseable file becomes (or stays) an `error` row with
/// fallback metadata. `duration` is `None`: it must come from the file, never
/// be fabricated.
fn error_upsert(
    path: &Path,
    size: u64,
    mtime: i64,
    stored_hash: Option<&str>,
    reason: IngestErrorReason,
) -> TrackUpsert {
    let title =
        super::tag_reader::filename_stem(path).unwrap_or_else(|| "Unknown Title".to_string());
    TrackUpsert {
        file_path: path.to_string_lossy().into_owned(),
        file_hash: stored_hash.map(str::to_string),
        file_size_bytes: size,
        modified_timestamp: mtime,
        title,
        artist: "Unknown Artist".to_string(),
        album: None,
        genre: None,
        duration_seconds: None,
        sample_rate: None,
        channels: None,
        ingest_status: IngestStatus::Error,
        ingest_error: Some(reason),
        availability: Availability::Available,
    }
}

/// Preserve the stored values that ingest does not own (album/genre may be
/// re-derived, but the hash/analysis fields are not upsert-managed anyway).
fn upsert_from_row(row: &TrackRow) -> TrackUpsert {
    TrackUpsert {
        file_path: row.file_path.clone(),
        file_hash: row.file_hash.clone(),
        file_size_bytes: row.file_size_bytes,
        modified_timestamp: row.modified_timestamp,
        title: row.title.clone(),
        artist: row.artist.clone(),
        album: row.album.clone(),
        genre: row.genre.clone(),
        duration_seconds: row.duration_seconds,
        sample_rate: row.sample_rate,
        channels: row.channels,
        ingest_status: row.ingest_status,
        ingest_error: row.ingest_error,
        availability: row.availability,
    }
}

// ----------------------------------------------------------------------
// Duplicate linking
// ----------------------------------------------------------------------

/// Pure duplicate-link computation.
///
/// `rows` is `(id, hash, ingest_seq)` for every indexed row; `hash` is
/// `None` for rows excluded from dedup (no hash yet, legacy `''`
/// placeholder, or `ingest_status = error`). Canonical row per content hash
/// = the one with the minimum `ingest_seq`. Returns, in input order, each
/// row id mapped to its computed `duplicate_of` (`None` for canonical rows
/// and excluded rows) — two runs over the same input produce identical
/// output.
pub fn compute_duplicate_links(
    rows: &[(String, Option<String>, i64)],
) -> Vec<(String, Option<String>)> {
    let mut canonical: HashMap<&str, (i64, &str)> = HashMap::new();
    for (id, hash, seq) in rows {
        if let Some(hash) = hash {
            let entry = canonical.entry(hash).or_insert((*seq, id));
            if *seq < entry.0 {
                *entry = (*seq, id);
            }
        }
    }
    rows.iter()
        .map(|(id, hash, _seq)| {
            let dup = hash
                .as_ref()
                .and_then(|h| canonical.get(h.as_str()))
                .filter(|(_, canonical_id)| *canonical_id != id)
                .map(|(_, canonical_id)| Some(canonical_id.to_string()));
            (id.clone(), dup.flatten())
        })
        .collect()
}

/// Apply the dedup pass: compute links from the current row set, diff
/// against stored `duplicate_of`, and write only the deltas (including
/// NULL-clears) in one transaction.
fn dedup_pass(store: &LibraryStore) -> Result<DedupSummary, LibraryError> {
    let rows = store.cache.all_tracks()?;
    let by_id: HashMap<&str, &TrackRow> = rows.iter().map(|r| (r.id.as_str(), r)).collect();
    let inputs: Vec<(String, Option<String>, i64)> = rows
        .iter()
        .map(|r| {
            let eligible = r.ingest_status == IngestStatus::Ok
                && r.file_hash.is_some()
                && !r.file_hash.as_deref().unwrap_or("").is_empty();
            (
                r.id.clone(),
                eligible.then(|| r.file_hash.clone().unwrap_or_default()),
                r.ingest_seq,
            )
        })
        .collect();
    let computed = compute_duplicate_links(&inputs);
    let mut links: Vec<(String, Option<String>)> = Vec::new();
    let mut entries: Vec<ReportEntry> = Vec::new();
    for (id, dup) in computed {
        let stored_dup = by_id
            .get(id.as_str())
            .and_then(|r| r.duplicate_of.as_deref());
        if stored_dup != dup.as_deref() {
            links.push((id.clone(), dup.clone()));
            if let Some(canonical) = dup {
                let row = by_id.get(id.as_str()).unwrap_or_else(|| unreachable!());
                entries.push(ReportEntry {
                    id: id.clone(),
                    file_path: row.file_path.clone(),
                    reason: canonical,
                });
            }
        }
    }
    if !links.is_empty() {
        store.cache.apply_duplicate_links(&links)?;
    }
    Ok(DedupSummary {
        duplicates: entries.len() as u32,
        entries,
    })
}

/// Enqueue a file for background SHA-256 (deduped per row id).
fn enqueue_hash(store: &LibraryStore, id: &str, file_path: &str) {
    let mut queue = store.hash_queue.lock().unwrap();
    if !queue.iter().any(|j| j.id == id) {
        queue.push(HashJob {
            id: id.to_string(),
            file_path: file_path.to_string(),
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::library::tag_reader::fixtures::write_tagged_fixture;
    use crate::library::types::{AnalysisState, IngestErrorReason};
    use std::path::PathBuf;
    use std::sync::atomic::{AtomicU64, Ordering};
    use std::time::{Duration, UNIX_EPOCH};

    fn test_dir() -> PathBuf {
        static COUNTER: AtomicU64 = AtomicU64::new(0);
        let dir = std::env::temp_dir().join(format!(
            "pulse-lib-store-{}-{}",
            std::process::id(),
            COUNTER.fetch_add(1, Ordering::Relaxed)
        ));
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn make_store() -> LibraryStore {
        LibraryStore::new(LibraryCache::in_memory().unwrap())
    }

    fn dir_str(dir: &Path) -> &str {
        dir.to_str().unwrap()
    }

    /// Raw bytes of a committed silent-tone fixture (for content swaps).
    fn tone_bytes(format: &str) -> Vec<u8> {
        std::fs::read(
            Path::new(env!("CARGO_MANIFEST_DIR"))
                .join(format!("src/library/fixtures/tone.{format}")),
        )
        .unwrap()
    }

    fn set_mtime(path: &Path, unix_secs: i64) {
        let file = std::fs::File::open(path).unwrap();
        file.set_modified(UNIX_EPOCH + Duration::from_secs(unix_secs as u64))
            .unwrap();
    }

    fn cleanup(dir: &Path) {
        let _ = std::fs::remove_dir_all(dir);
    }

    /// T2: happy-path ingest of three distinct files.
    #[test]
    fn test_happy_path_ingest() {
        let dir = test_dir();
        write_tagged_fixture("wav", "one", "Title One", "Artist One", None, None, &dir);
        write_tagged_fixture("wav", "two", "Title Two", "Artist Two", None, None, &dir);
        write_tagged_fixture(
            "wav",
            "three",
            "Title Three",
            "Artist Three",
            None,
            None,
            &dir,
        );

        let store = make_store();
        let report = store.add_folder(dir_str(&dir)).unwrap();
        assert_eq!(report.added, 3, "all three files must be added");

        store.flush_hashes().unwrap();
        let list = store.list().unwrap();
        assert_eq!(list.len(), 3);
        let mut hashes = std::collections::HashSet::new();
        for s in &list {
            assert_eq!(s.ingest_status, IngestStatus::Ok);
            assert_eq!(s.availability, Availability::Available);
            assert_eq!(s.analysis_state, AnalysisState::NotStarted);
            let h = s
                .file_hash
                .clone()
                .expect("hash must be present after flush");
            hashes.insert(h);
        }
        assert_eq!(hashes.len(), 3, "distinct files must have distinct hashes");
        let one = list.iter().find(|s| s.title == "Title One").unwrap();
        assert_eq!(one.artist, "Artist One");
        assert!(one.duration_seconds.is_some());
        cleanup(&dir);
    }

    /// T3: identical content at two paths links the later row to the earlier.
    #[test]
    fn test_duplicate_content_at_two_paths() {
        let dir = test_dir();
        let a = dir.join("a");
        let b = dir.join("b");
        std::fs::create_dir_all(&a).unwrap();
        std::fs::create_dir_all(&b).unwrap();
        write_tagged_fixture("wav", "song", "Same Song", "Same Artist", None, None, &a);
        write_tagged_fixture("wav", "song", "Same Song", "Same Artist", None, None, &b);

        let store = make_store();
        store.add_folder(dir_str(&dir)).unwrap();
        // Hashes land via the deterministic flush path; the dedup pass then
        // links the rows (command-level links would wait for the worker).
        let summary = store.flush_hashes().unwrap();
        assert_eq!(summary.duplicates, 1, "one duplicate link expected");

        let list = store.list().unwrap();
        assert_eq!(list.len(), 2, "both duplicate rows appear in list");
        let earlier = list
            .iter()
            .find(|s| s.file_path.contains("/a/"))
            .unwrap()
            .clone();
        let later = list
            .iter()
            .find(|s| s.file_path.contains("/b/"))
            .unwrap()
            .clone();
        assert_eq!(earlier.duplicate_of, None, "earlier row is canonical");
        assert_eq!(later.duplicate_of, Some(earlier.id));
        cleanup(&dir);
    }

    /// T4: delete → refresh marks missing; re-create (same size+mtime) →
    /// refresh restores availability without re-tagging.
    #[test]
    fn test_delete_then_refresh() {
        let dir = test_dir();
        let path = write_tagged_fixture("wav", "song", "Song", "Artist", None, None, &dir);
        let path = path.canonicalize().unwrap();

        let store = make_store();
        store.add_folder(dir_str(&dir)).unwrap();
        store.flush_hashes().unwrap();
        let row = store
            .cache()
            .get_track_by_path(dir_str(&path))
            .unwrap()
            .unwrap();
        let original_hash = row.file_hash.clone().unwrap();
        let mtime = row.modified_timestamp;

        std::fs::remove_file(&path).unwrap();
        let report = store.refresh().unwrap();
        assert_eq!(report.missing, 1, "the vanished file is newly missing");
        let after_delete = store
            .cache()
            .get_track_by_path(dir_str(&path))
            .unwrap()
            .unwrap();
        assert_eq!(after_delete.availability, Availability::Missing);

        // Re-create with the same content, and restore the exact mtime.
        let p = write_tagged_fixture("wav", "song", "Song", "Artist", None, None, &dir);
        set_mtime(&p, mtime);
        let p = p.canonicalize().unwrap();
        let report = store.refresh().unwrap();
        assert_eq!(report.updated, 1, "availability restored counts as updated");
        let restored = store
            .cache()
            .get_track_by_path(dir_str(&p))
            .unwrap()
            .unwrap();
        assert_eq!(restored.availability, Availability::Available);
        // No re-tag happened: metadata and hash are untouched.
        assert_eq!(restored.title, "Song");
        assert_eq!(restored.file_hash.as_deref(), Some(original_hash.as_str()));
        cleanup(&dir);
    }

    /// T5: a changed file is re-tagged and re-hashed on refresh.
    #[test]
    fn test_modify_then_refresh() {
        let dir = test_dir();
        let path = write_tagged_fixture("wav", "song", "Old Title", "Artist", None, None, &dir);
        let path = path.canonicalize().unwrap();
        let store = make_store();
        store.add_folder(dir_str(&dir)).unwrap();
        store.flush_hashes().unwrap();
        let old = store
            .cache()
            .get_track_by_path(dir_str(&path))
            .unwrap()
            .unwrap();
        assert_eq!(old.title, "Old Title");
        let old_hash = old.file_hash.clone().unwrap();

        // Re-tag in place with a different-length title (size changes).
        write_tagged_fixture(
            "wav",
            "song",
            "This Is A Brand New Title",
            "Artist",
            None,
            None,
            &dir,
        );
        let report = store.refresh().unwrap();
        assert_eq!(report.updated, 1, "changed file is re-tagged");
        let new = store
            .cache()
            .get_track_by_path(dir_str(&path))
            .unwrap()
            .unwrap();
        assert_eq!(new.title, "This Is A Brand New Title");
        assert_eq!(new.ingest_status, IngestStatus::Ok);

        store.flush_hashes().unwrap();
        let rehashed = store
            .cache()
            .get_track_by_path(dir_str(&path))
            .unwrap()
            .unwrap();
        assert_ne!(
            rehashed.file_hash.as_deref(),
            Some(old_hash.as_str()),
            "the content hash must change with the content"
        );
        cleanup(&dir);
    }

    /// Regression: a changed file's stale hash must be invalidated in the
    /// same *durable* write. Sequence: re-tag a modified file, the app exits
    /// before the hash worker drains the in-memory queue, and the next launch
    /// must not keep a hash of the pre-change content (the row is simply
    /// "unhashed" and self-heals on the next refresh).
    #[test]
    fn test_changed_file_invalidates_stale_hash_before_restart() {
        let dir = test_dir();
        let db_dir = test_dir();
        let db = db_dir.join("lib.db");
        let path = write_tagged_fixture("wav", "song", "Old Title", "Artist", None, None, &dir);
        let path = path.canonicalize().unwrap();
        let path_str = dir_str(&path).to_string();

        let store = LibraryStore::new(LibraryCache::open(&db).unwrap());
        store.add_folder(dir_str(&dir)).unwrap();
        store.flush_hashes().unwrap();
        let old = store.cache().get_track_by_path(&path_str).unwrap().unwrap();
        let old_hash = old.file_hash.clone().unwrap();
        assert!(!old_hash.is_empty());

        // Content changes (different-length title ⇒ size + mtime change);
        // refresh re-tags and queues the re-hash in memory.
        write_tagged_fixture(
            "wav",
            "song",
            "This Is A Brand New Title",
            "Artist",
            None,
            None,
            &dir,
        );
        store.refresh().unwrap();
        drop(store); // app exits before the worker drains the queue

        let store = LibraryStore::new(LibraryCache::open(&db).unwrap());
        let row = store.cache().get_track_by_path(&path_str).unwrap().unwrap();
        assert!(
            row.file_hash.as_deref().unwrap_or("").is_empty(),
            "stale hash of the pre-change content must be invalidated durably, got {row:?}"
        );

        // Self-heal: the next refresh re-queues the unhashed row and the
        // fresh hash is of the *current* content.
        store.refresh().unwrap();
        store.flush_hashes().unwrap();
        let row = store.cache().get_track_by_path(&path_str).unwrap().unwrap();
        let current = sha256_file(&path).unwrap();
        assert_eq!(row.file_hash.as_deref(), Some(current.as_str()));
        assert_ne!(row.file_hash.as_deref(), Some(old_hash.as_str()));
        let _ = std::fs::remove_dir_all(&dir);
        let _ = std::fs::remove_dir_all(&db_dir);
    }

    /// Regression: a freshly inserted file whose hash job never drains
    /// before app exit stays unhashed (NULL/'' — a state the machine can
    /// self-heal); the next launch's refresh re-queues it.
    #[test]
    fn test_unhashed_new_file_self_heals_after_restart() {
        let dir = test_dir();
        let db_dir = test_dir();
        let db = db_dir.join("lib.db");
        let path = write_tagged_fixture("wav", "song", "Song", "Artist", None, None, &dir);
        let path = path.canonicalize().unwrap();
        let path_str = dir_str(&path).to_string();

        let store = LibraryStore::new(LibraryCache::open(&db).unwrap());
        store.add_folder(dir_str(&dir)).unwrap(); // hash queued, never drained
        drop(store); // app exits before the worker's first tick

        let store = LibraryStore::new(LibraryCache::open(&db).unwrap());
        let row = store.cache().get_track_by_path(&path_str).unwrap().unwrap();
        assert!(
            row.file_hash.as_deref().unwrap_or("").is_empty(),
            "row inserted unhashed, got {row:?}"
        );
        store.refresh().unwrap();
        store.flush_hashes().unwrap();
        let row = store.cache().get_track_by_path(&path_str).unwrap().unwrap();
        assert_eq!(
            row.file_hash.as_deref(),
            Some(sha256_file(&path).unwrap().as_str()),
            "the row must be hashed after the restart's refresh"
        );
        let _ = std::fs::remove_dir_all(&dir);
        let _ = std::fs::remove_dir_all(&db_dir);
    }

    /// T6: a corrupt file degrades to an error row; the scan never aborts.
    #[test]
    fn test_corrupt_file() {
        let dir = test_dir();
        std::fs::write(dir.join("bad.flac"), b"not a flac file").unwrap();
        write_tagged_fixture("wav", "good", "Good Song", "Artist", None, None, &dir);

        let store = make_store();
        let report = store.add_folder(dir_str(&dir)).unwrap();
        assert_eq!(report.errored, 1, "corrupt file ends errored");
        assert_eq!(report.added, 2, "scan never aborts on one bad file");
        // The errored entry identifies the failing file for the frontend.
        assert_eq!(report.errored_entries.len(), 1);
        assert!(
            !report.errored_entries[0].id.is_empty(),
            "freshly inserted error rows must report a real id, got {:?}",
            report.errored_entries[0]
        );
        assert!(
            report.errored_entries[0].file_path.ends_with("bad.flac"),
            "errored entry must name the failing file, got {:?}",
            report.errored_entries[0]
        );

        let list = store.list().unwrap();
        assert_eq!(list.len(), 2);
        let bad = list
            .iter()
            .find(|s| s.file_path.ends_with("bad.flac"))
            .unwrap()
            .clone();
        assert_eq!(bad.ingest_status, IngestStatus::Error);
        assert_eq!(bad.ingest_error, Some(IngestErrorReason::CorruptContainer));
        assert_eq!(bad.availability, Availability::Available);
        assert_eq!(bad.duration_seconds, None, "no fabricated duration");
        assert_eq!(bad.title, "bad", "title falls back to the filename stem");
        let good = list
            .iter()
            .find(|s| s.file_path.ends_with("good.wav"))
            .unwrap();
        assert_eq!(good.ingest_status, IngestStatus::Ok);
        cleanup(&dir);
    }

    /// T7: unreadable permissions degrade to a `permission_denied` row.
    #[cfg(unix)]
    #[test]
    fn test_unreadable_permissions() {
        use std::os::unix::fs::PermissionsExt;
        let dir = test_dir();
        let path = write_tagged_fixture("wav", "secret", "Secret", "Artist", None, None, &dir);
        let path = path.canonicalize().unwrap();
        std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o0)).unwrap();
        // Running as root would still read the file (and CI may be root):
        // probe it, and skip the test rather than assert the wrong state.
        let root_probe = std::fs::File::open(&path)
            .and_then(|mut f| {
                let mut buf = [0u8; 1];
                std::io::Read::read(&mut f, &mut buf)
            })
            .is_ok();
        if root_probe {
            let _ = std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o644));
            cleanup(&dir);
            return;
        }

        let store = make_store();
        // add_folder must still succeed (per-file failure degrades to a row).
        let report = store.add_folder(dir_str(&dir)).unwrap();
        assert_eq!(report.errored, 1);
        // Restore permissions before the remaining assertions/cleanup.
        std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o644)).unwrap();
        let row = store
            .cache()
            .get_track_by_path(dir_str(&path))
            .unwrap()
            .unwrap();
        assert_eq!(row.ingest_status, IngestStatus::Error);
        assert_eq!(row.ingest_error, Some(IngestErrorReason::PermissionDenied));
        cleanup(&dir);
    }

    /// T8: re-scanning an unchanged folder is a no-op (idempotent).
    #[test]
    fn test_idempotent_rescan() {
        let dir = test_dir();
        write_tagged_fixture("wav", "one", "Title One", "Artist One", None, None, &dir);
        write_tagged_fixture("wav", "two", "Title Two", "Artist Two", None, None, &dir);

        let store = make_store();
        store.add_folder(dir_str(&dir)).unwrap();
        store.flush_hashes().unwrap();
        let first = store.list().unwrap();

        let report = store.add_folder(dir_str(&dir)).unwrap();
        assert_eq!(report.added, 0, "no re-inserts");
        assert_eq!(report.updated, 0, "no re-writes (zero changes)");
        let second = store.list().unwrap();
        assert_eq!(first, second, "list is byte-for-byte identical");
        cleanup(&dir);
    }

    /// T9: refresh retries errored rows; a fixed file becomes ok.
    #[test]
    fn test_refresh_retries_errored_rows() {
        let dir = test_dir();
        let path = dir.join("bad.flac");
        std::fs::write(&path, b"not a flac file").unwrap();
        let path = path.canonicalize().unwrap();

        let store = make_store();
        store.add_folder(dir_str(&dir)).unwrap();
        // Still corrupt: refresh retries, fails with the same reason →
        // state unchanged, but the row still counts as errored.
        let report = store.refresh().unwrap();
        assert_eq!(report.errored, 1);
        assert_eq!(report.updated, 0, "same failure keeps the state unchanged");

        // Fix the file in place: valid FLAC bytes (a *valid different* codec
        // with a .flac name — lofty probes by extension; the 11 KB tone
        // differs in size from the 15-byte garbage blob, so the change is
        // detected) → refresh re-tags to a healthy row.
        std::fs::write(&path, tone_bytes("flac")).unwrap();
        let report = store.refresh().unwrap();
        assert_eq!(report.updated, 1, "fixed file is re-tagged");
        let row = store
            .cache()
            .get_track_by_path(dir_str(&path))
            .unwrap()
            .unwrap();
        assert_eq!(row.ingest_status, IngestStatus::Ok);
        assert_eq!(row.ingest_error, None);
        assert_eq!(row.title, "bad", "no tags → stem fallback");
        cleanup(&dir);
    }

    /// T10: reads lazily mark vanished files missing (DB and projection).
    #[test]
    fn test_lazy_missing_mark_on_list() {
        let dir = test_dir();
        let path = write_tagged_fixture("wav", "song", "Song", "Artist", None, None, &dir);
        let path = path.canonicalize().unwrap();
        let store = make_store();
        store.add_folder(dir_str(&dir)).unwrap();
        std::fs::remove_file(&path).unwrap();

        // No refresh — a plain list must notice the vanished file.
        let list = store.list().unwrap();
        assert_eq!(list[0].availability, Availability::Missing);
        let db_row = store
            .cache()
            .get_track_by_path(dir_str(&path))
            .unwrap()
            .unwrap();
        assert_eq!(
            db_row.availability,
            Availability::Missing,
            "DB row updated too"
        );

        // library_get does the same for a single row.
        let summary = store.get(&db_row.id).unwrap();
        assert_eq!(summary.availability, Availability::Missing);
        // Unknown id → message-carrying Db error.
        assert!(store.get("nope").is_err());
        cleanup(&dir);
    }

    /// T12: dedup determinism across stores + pure-function exclusions.
    #[test]
    fn test_dedup_determinism() {
        fn ingest_pair() -> Vec<(String, Option<String>)> {
            let dir = test_dir();
            let a = dir.join("a");
            let b = dir.join("b");
            std::fs::create_dir_all(&a).unwrap();
            std::fs::create_dir_all(&b).unwrap();
            write_tagged_fixture("wav", "dup1", "Dup One", "A", None, None, &a);
            write_tagged_fixture("wav", "dup1", "Dup One", "A", None, None, &b);
            write_tagged_fixture("wav", "uniq", "Unique", "A", None, None, &a);
            let store = make_store();
            store.add_folder(dir_str(&dir)).unwrap();
            store.flush_hashes().unwrap();
            let rows = store.cache().all_tracks().unwrap();
            let dir = dir.canonicalize().unwrap();
            let base = dir.to_string_lossy().to_string();
            let rel = |p: &str| p.strip_prefix(base.as_str()).unwrap().to_string();
            let by_id: std::collections::HashMap<&str, &TrackRow> =
                rows.iter().map(|r| (r.id.as_str(), r)).collect();
            // Normalize to paths relative to this run's unique temp dir so
            // the two runs are comparable despite random row ids.
            let links = rows
                .iter()
                .map(|r| {
                    let dup = r.duplicate_of.as_ref().map(|id| {
                        rel(by_id
                            .get(id.as_str())
                            .expect("duplicate_of points at an indexed row")
                            .file_path
                            .as_str())
                    });
                    (rel(r.file_path.as_str()), dup)
                })
                .collect::<Vec<_>>();
            cleanup(&dir);
            links
        }

        let first = ingest_pair();
        let second = ingest_pair();
        assert_eq!(first, second, "identical ingest order ⇒ identical links");

        // Pure function: hash-less and (mapped-out) error rows excluded;
        // canonical = minimum ingest_seq per hash.
        let rows = vec![
            ("a".to_string(), Some("h1".to_string()), 1),
            ("b".to_string(), Some("h1".to_string()), 2),
            ("c".to_string(), None, 3), // no hash → excluded
            ("d".to_string(), Some("h2".to_string()), 4), // unique hash
        ];
        let links = compute_duplicate_links(&rows);
        assert_eq!(
            links,
            vec![
                ("a".to_string(), None),
                ("b".to_string(), Some("a".to_string())),
                ("c".to_string(), None),
                ("d".to_string(), None),
            ]
        );
        // Re-running over the same state is identical.
        assert_eq!(links, compute_duplicate_links(&rows));
    }

    // -----------------------------------------------------------------
    // Hardened boundary / negative tests (pure state machine, no I/O)
    // -----------------------------------------------------------------

    fn mk_row(availability: Availability, status: IngestStatus) -> TrackRow {
        TrackRow {
            id: "id".to_string(),
            file_path: "/a/x.mp3".to_string(),
            file_hash: None,
            file_size_bytes: 100,
            modified_timestamp: 1000,
            title: "T".to_string(),
            artist: "A".to_string(),
            album: None,
            genre: None,
            duration_seconds: None,
            bpm: None,
            key: None,
            camelot: None,
            profile_json: None,
            ingest_status: status,
            ingest_error: None,
            availability,
            analysis_state: AnalysisState::NotStarted,
            sample_rate: None,
            channels: None,
            duplicate_of: None,
            ingest_seq: 1,
            created_at: None,
            updated_at: None,
        }
    }

    fn ok_meta() -> AudioMetadata {
        AudioMetadata {
            title: "Read".to_string(),
            artist: "R".to_string(),
            album: None,
            genre: None,
            duration_seconds: Some(1.0),
            sample_rate: None,
            channels: None,
        }
    }

    fn never_read(_: &Path) -> TagOutcome {
        panic!("this transition must not perform a tag read")
    }

    /// A vanished file with no row at all: nothing to do, no read.
    #[test]
    fn test_state_machine_missing_unindexed_is_noop() {
        let t = derive_row_state(
            None,
            Path::new("/a/x.mp3"),
            FsState::Missing,
            true,
            &never_read,
        );
        assert_eq!(t.change, RowChange::Unchanged);
        assert!(!t.enqueue_hash && !t.errored_row);
    }

    /// A vanished available row transitions to Missing even when the file
    /// was also errored: availability wins, and a missing file must never
    /// trigger a tag read.
    #[test]
    fn test_state_machine_missing_beats_errored_retry() {
        let row = mk_row(Availability::Available, IngestStatus::Error);
        let t = derive_row_state(
            Some(&row),
            Path::new("/a/x.mp3"),
            FsState::Missing,
            true,
            &never_read,
        );
        assert_eq!(t.change, RowChange::Missing);
        assert!(t.upsert.is_none());
    }

    /// A vanished row that is already missing: idempotent no-op.
    #[test]
    fn test_state_machine_missing_already_missing_is_noop() {
        let row = mk_row(Availability::Missing, IngestStatus::Ok);
        let t = derive_row_state(
            Some(&row),
            Path::new("/a/x.mp3"),
            FsState::Missing,
            true,
            &never_read,
        );
        assert_eq!(t.change, RowChange::Unchanged);
        assert!(!t.errored_row);
    }

    /// Zero-size present file (empty but indexed): a stat-only stable row
    /// is unchanged and must not be re-read.
    #[test]
    fn test_state_machine_zero_size_stable_is_unchanged() {
        let mut row = mk_row(Availability::Available, IngestStatus::Ok);
        row.file_size_bytes = 0;
        let t = derive_row_state(
            Some(&row),
            Path::new("/a/x.mp3"),
            FsState::Present {
                size: 0,
                mtime: 1000,
            },
            false,
            &never_read,
        );
        assert_eq!(t.change, RowChange::Unchanged);
    }

    /// Extreme metadata values survive the transition unchanged: `u64::MAX`
    /// size, `i64::MAX` mtime.
    #[test]
    fn test_state_machine_extreme_size_mtime() {
        let mut row = mk_row(Availability::Available, IngestStatus::Ok);
        row.file_size_bytes = u64::MAX;
        row.modified_timestamp = i64::MAX;
        let t = derive_row_state(
            Some(&row),
            Path::new("/a/x.mp3"),
            FsState::Present {
                size: u64::MAX,
                mtime: i64::MAX,
            },
            false,
            &never_read,
        );
        assert_eq!(t.change, RowChange::Unchanged);
    }

    /// Pre-1970 (negative) mtime: the row stores the same value, so the
    /// transition must be unchanged (mtime is compared, never clamped).
    #[test]
    fn test_state_machine_negative_mtime_stable() {
        let mut row = mk_row(Availability::Available, IngestStatus::Ok);
        row.modified_timestamp = -1;
        let t = derive_row_state(
            Some(&row),
            Path::new("/a/x.mp3"),
            FsState::Present {
                size: 100,
                mtime: -1,
            },
            false,
            &never_read,
        );
        assert_eq!(t.change, RowChange::Unchanged);
    }

    /// mtime changed but size identical (a touched file): the row is
    /// re-read and re-hashed — both inputs must count as "changed".
    #[test]
    fn test_state_machine_mtime_only_change_retags() {
        let row = mk_row(Availability::Available, IngestStatus::Ok);
        let t = derive_row_state(
            Some(&row),
            Path::new("/a/x.mp3"),
            FsState::Present {
                size: 100,
                mtime: 1001,
            },
            false,
            &|_| TagOutcome::Read(ok_meta()),
        );
        assert_eq!(t.change, RowChange::Updated);
        assert!(t.enqueue_hash);
        assert_eq!(t.upsert.as_ref().unwrap().ingest_status, IngestStatus::Ok);
    }

    /// New file whose tags read successfully: insert ok row + hash job.
    #[test]
    fn test_state_machine_new_file_ok() {
        let t = derive_row_state(
            None,
            Path::new("/a/x.mp3"),
            FsState::Present { size: 42, mtime: 7 },
            false,
            &|_| TagOutcome::Read(ok_meta()),
        );
        assert_eq!(t.change, RowChange::Inserted);
        assert!(t.enqueue_hash);
        let u = t.upsert.as_ref().unwrap();
        assert_eq!(u.ingest_status, IngestStatus::Ok);
        assert_eq!(u.file_size_bytes, 42);
    }

    /// New file whose tags fail with a non-missing reason: insert an error
    /// row with fallbacks and NO hash job (hashing an unparseable file is
    /// wasted work).
    #[test]
    fn test_state_machine_new_file_corrupt_no_hash() {
        let t = derive_row_state(
            None,
            Path::new("/a/x.mp3"),
            FsState::Present { size: 42, mtime: 7 },
            false,
            &|_| TagOutcome::Failed(IngestErrorReason::CorruptContainer),
        );
        assert_eq!(t.change, RowChange::Inserted);
        assert!(!t.enqueue_hash);
        let u = t.upsert.as_ref().unwrap();
        assert_eq!(u.ingest_status, IngestStatus::Error);
        assert_eq!(u.ingest_error, Some(IngestErrorReason::CorruptContainer));
        assert_eq!(u.title, "x", "title falls back to the stem");
        assert_eq!(u.artist, "Unknown Artist");
        assert_eq!(u.duration_seconds, None);
    }

    /// A file that vanishes between the stat and the tag read: no row is
    /// created (there is nothing to index).
    #[test]
    fn test_state_machine_new_file_vanished_before_read() {
        let t = derive_row_state(
            None,
            Path::new("/a/x.mp3"),
            FsState::Present { size: 42, mtime: 7 },
            false,
            &|_| TagOutcome::Failed(IngestErrorReason::MissingFile),
        );
        assert_eq!(t.change, RowChange::Unchanged);
        assert!(t.upsert.is_none());
    }

    /// Refresh retry of a stable errored row that fails with the SAME
    /// reason: state unchanged (idempotent) but the row is reported
    /// errored, and no hash job is queued.
    #[test]
    fn test_state_machine_retry_same_error_is_stable() {
        let mut row = mk_row(Availability::Available, IngestStatus::Error);
        row.ingest_error = Some(IngestErrorReason::CorruptContainer);
        let t = derive_row_state(
            Some(&row),
            Path::new("/a/x.mp3"),
            FsState::Present {
                size: 100,
                mtime: 1000,
            },
            true,
            &|_| TagOutcome::Failed(IngestErrorReason::CorruptContainer),
        );
        assert_eq!(t.change, RowChange::Unchanged);
        assert!(t.errored_row);
        assert!(!t.enqueue_hash);
    }

    /// Refresh retry where the failure reason CHANGED: the row is updated
    /// to the new reason and re-hashed.
    #[test]
    fn test_state_machine_retry_changed_error_updates() {
        let mut row = mk_row(Availability::Available, IngestStatus::Error);
        row.ingest_error = Some(IngestErrorReason::CorruptContainer);
        let t = derive_row_state(
            Some(&row),
            Path::new("/a/x.mp3"),
            FsState::Present {
                size: 100,
                mtime: 1000,
            },
            true,
            &|_| TagOutcome::Failed(IngestErrorReason::UnreadableFile),
        );
        assert_eq!(t.change, RowChange::Updated);
        assert!(!t.errored_row);
        assert!(t.enqueue_hash);
        assert_eq!(
            t.upsert.as_ref().unwrap().ingest_error,
            Some(IngestErrorReason::UnreadableFile)
        );
    }

    /// Refresh retry where the file recovered: the row becomes ok again,
    /// keeping the previously applied hash, and re-queues hashing.
    #[test]
    fn test_state_machine_retry_recovers() {
        let mut row = mk_row(Availability::Available, IngestStatus::Error);
        row.ingest_error = Some(IngestErrorReason::CorruptContainer);
        row.file_hash = Some("h0".to_string());
        let t = derive_row_state(
            Some(&row),
            Path::new("/a/x.mp3"),
            FsState::Present {
                size: 100,
                mtime: 1000,
            },
            true,
            &|_| TagOutcome::Read(ok_meta()),
        );
        assert_eq!(t.change, RowChange::Updated);
        assert!(t.enqueue_hash);
        let u = t.upsert.as_ref().unwrap();
        assert_eq!(u.ingest_status, IngestStatus::Ok);
        assert_eq!(u.ingest_error, None);
        assert_eq!(
            u.file_hash.as_deref(),
            Some("h0"),
            "the applied hash is preserved"
        );
    }

    /// A stable healthy row under a plain (non-retry) scan: unchanged and
    /// never re-read.
    #[test]
    fn test_state_machine_stable_ok_untouched() {
        let row = mk_row(Availability::Available, IngestStatus::Ok);
        let t = derive_row_state(
            Some(&row),
            Path::new("/a/x.mp3"),
            FsState::Present {
                size: 100,
                mtime: 1000,
            },
            false,
            &never_read,
        );
        assert_eq!(t.change, RowChange::Unchanged);
    }

    /// A file restored after being missing with identical size+mtime:
    /// availability flips back, no re-tag, no re-hash.
    #[test]
    fn test_state_machine_restored_no_retap() {
        let mut row = mk_row(Availability::Missing, IngestStatus::Ok);
        row.title = "Original".to_string();
        let t = derive_row_state(
            Some(&row),
            Path::new("/a/x.mp3"),
            FsState::Present {
                size: 100,
                mtime: 1000,
            },
            false,
            &never_read,
        );
        assert_eq!(t.change, RowChange::Updated);
        assert!(!t.enqueue_hash);
        let u = t.upsert.as_ref().unwrap();
        assert_eq!(u.availability, Availability::Available);
        assert_eq!(u.title, "Original", "metadata is preserved on restore");
    }

    // -----------------------------------------------------------------
    // sha256_file boundaries
    // -----------------------------------------------------------------

    /// The empty file hashes to the well-known empty-input digest.
    #[test]
    fn test_sha256_empty_file() {
        let dir = test_dir();
        let p = dir.join("empty.bin");
        std::fs::write(&p, b"").unwrap();
        let h = sha256_file(&p).unwrap();
        assert_eq!(
            h,
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
        );
        cleanup(&dir);
    }

    /// A single byte hashes to the known digest (streaming boundary: n=1 <
    /// buffer size, then EOF on the next read).
    #[test]
    fn test_sha256_single_byte() {
        let dir = test_dir();
        let p = dir.join("one.bin");
        std::fs::write(&p, [b'x']).unwrap();
        let h = sha256_file(&p).unwrap();
        assert_eq!(
            h,
            "2d711642b726b04401627ca9fbac32f5c8530fb1903cc4db02258717921a4881"
        );
        cleanup(&dir);
    }

    /// Exactly one full buffer (64 KiB): the read loop must do a final
    /// zero-length read to terminate and include every byte.
    #[test]
    fn test_sha256_exact_buffer_boundary() {
        let dir = test_dir();
        let p = dir.join("64k.bin");
        let payload = vec![0u8; 64 * 1024];
        std::fs::write(&p, &payload).unwrap();
        let got = sha256_file(&p).unwrap();
        let mut expect = Sha256::new();
        expect.update(&payload);
        assert_eq!(got, hex_of(&expect.finalize()));
        cleanup(&dir);
    }

    /// A file just over the buffer size (64 KiB + 1 byte) exercises the
    /// partial final read.
    #[test]
    fn test_sha256_buffer_plus_one() {
        let dir = test_dir();
        let p = dir.join("64k1.bin");
        let payload = vec![1u8; 64 * 1024 + 1];
        std::fs::write(&p, &payload).unwrap();
        let got = sha256_file(&p).unwrap();
        let mut expect = Sha256::new();
        expect.update(&payload);
        assert_eq!(got, hex_of(&expect.finalize()));
        cleanup(&dir);
    }

    /// Nonexistent input is a clean Err, never a panic.
    #[test]
    fn test_sha256_missing_file_is_err() {
        let dir = test_dir();
        let p = dir.join("nope.bin");
        assert!(sha256_file(&p).is_err());
        cleanup(&dir);
    }

    fn hex_of(digest: &[u8]) -> String {
        use std::fmt::Write;
        digest
            .iter()
            .fold(String::with_capacity(digest.len() * 2), |mut out, b| {
                let _ = out.write_fmt(format_args!("{b:02x}"));
                out
            })
    }

    // -----------------------------------------------------------------
    // compute_duplicate_links edge cases (pure function)
    // -----------------------------------------------------------------

    #[test]
    fn test_dedup_empty_input() {
        assert_eq!(
            compute_duplicate_links(&[]),
            Vec::<(String, Option<String>)>::new()
        );
    }

    #[test]
    fn test_dedup_single_rows() {
        // A lone row with a hash is canonical (no self-link).
        assert_eq!(
            compute_duplicate_links(&[("a".into(), Some("h".into()), 1)]),
            vec![("a".into(), None)]
        );
        // A lone row without a hash is excluded (no link).
        assert_eq!(
            compute_duplicate_links(&[("a".into(), None, 1)]),
            vec![("a".into(), None)]
        );
    }

    /// Canonical = minimum `ingest_seq`, even when that row appears LAST in
    /// the input (input order must not influence the choice).
    #[test]
    fn test_dedup_canonical_is_min_seq_not_first_seen() {
        let rows = vec![
            ("first".into(), Some("h".into()), 50),
            ("second".into(), Some("h".into()), 10),
            ("third".into(), Some("h".into()), 30),
        ];
        let links = compute_duplicate_links(&rows);
        assert_eq!(links[0].1.as_deref(), Some("second"));
        assert_eq!(links[1].1, None, "the min-seq row is canonical");
        assert_eq!(links[2].1.as_deref(), Some("second"));
    }

    /// Equal `ingest_seq` values: the choice is implementation-defined (map
    /// order) but must be *self-consistent* — exactly one canonical, no
    /// self-links, every other row linked to the canonical.
    #[test]
    fn test_dedup_equal_seqs_exactly_one_canonical() {
        let rows = vec![
            ("a".into(), Some("h".into()), 7),
            ("b".into(), Some("h".into()), 7),
            ("c".into(), Some("h".into()), 7),
        ];
        let links = compute_duplicate_links(&rows);
        let canonicals = links.iter().filter(|(_, d)| d.is_none()).count();
        assert_eq!(canonicals, 1, "exactly one canonical: {links:?}");
        let canonical_id = links.iter().find(|(_, d)| d.is_none()).unwrap().0.clone();
        for (id, dup) in &links {
            if id != &canonical_id {
                assert_eq!(dup.as_deref(), Some(canonical_id.as_str()));
            }
        }
    }

    /// Two rows sharing an id (defensive: should not happen via the DB
    /// primary key, but the function must stay total): no panic.
    #[test]
    fn test_dedup_duplicate_ids_do_not_panic() {
        let rows = vec![
            ("a".into(), Some("h1".into()), 1),
            ("a".into(), Some("h2".into()), 2),
        ];
        let _ = compute_duplicate_links(&rows);
    }

    /// Distinct hashes never link to each other, regardless of seq order.
    #[test]
    fn test_dedup_distinct_hashes_never_link() {
        let rows = vec![
            ("a".into(), Some("h1".into()), 9),
            ("b".into(), Some("h2".into()), 1),
        ];
        assert_eq!(
            compute_duplicate_links(&rows),
            vec![("a".into(), None), ("b".into(), None)]
        );
    }

    // -----------------------------------------------------------------
    // add_folder negative / boundary paths
    // -----------------------------------------------------------------

    /// A folder with zero audio files ingests nothing but succeeds.
    #[test]
    fn test_add_folder_empty_folder() {
        let dir = test_dir();
        let store = make_store();
        let report = store.add_folder(dir_str(&dir)).unwrap();
        assert_eq!(report.added, 0);
        assert!(store.list().unwrap().is_empty());
        cleanup(&dir);
    }

    /// A nonexistent folder path is a typed `FolderNotFound` error.
    #[test]
    fn test_add_folder_nonexistent() {
        let store = make_store();
        let err = store.add_folder("/definitely/not/here-4f7a").unwrap_err();
        assert!(
            matches!(err, LibraryError::FolderNotFound(_)),
            "got {err:?}"
        );
    }

    /// A path that exists but is a FILE is `NotADirectory` — and never
    /// ingested as a track.
    #[test]
    fn test_add_folder_path_is_file() {
        let dir = test_dir();
        let file = write_tagged_fixture("wav", "file", "T", "A", None, None, &dir);
        let store = make_store();
        let err = store.add_folder(dir_str(&file)).unwrap_err();
        assert!(matches!(err, LibraryError::NotADirectory(_)), "got {err:?}");
        assert!(store.list().unwrap().is_empty());
        cleanup(&dir);
    }

    // -----------------------------------------------------------------
    // Duplicate-linking through the real write path
    // -----------------------------------------------------------------

    /// Identical content at two paths links on the *`add_folder`* dedup pass
    /// itself (no explicit flush): exactly one link, pointing at the
    /// lower-ingest_seq canonical; a re-scan adds zero new links.
    #[test]
    fn test_duplicate_links_survive_rescan() {
        let dir = test_dir();
        let a = dir.join("a");
        let b = dir.join("b");
        std::fs::create_dir_all(&a).unwrap();
        std::fs::create_dir_all(&b).unwrap();
        // Identical bytes (same committed fixture) at two paths.
        let tone = tone_bytes("wav");
        std::fs::write(a.join("song.wav"), &tone).unwrap();
        std::fs::write(b.join("song.wav"), &tone).unwrap();

        let store = make_store();
        let first = store.add_folder(dir_str(&dir)).unwrap();
        assert_eq!(first.added, 2);
        // New rows arrive without hashes (queued for the background
        // worker), so the scan's own dedup pass links nothing yet; the
        // deterministic flush applies the hashes and runs the linking pass.
        assert_eq!(first.duplicates, 0);
        let dedup = store.flush_hashes().unwrap();
        assert_eq!(dedup.duplicates, 1, "the flush's dedup pass links the copy");

        // The canonical is the earlier ingest_seq row.
        let rows = store.cache().all_tracks().unwrap();
        let canonical = rows.iter().filter(|r| r.duplicate_of.is_none()).count();
        assert_eq!(canonical, 1, "exactly one canonical row");
        let dup = rows.iter().find(|r| r.duplicate_of.is_some()).unwrap();
        let canon_row = rows
            .iter()
            .find(|r| r.id == dup.duplicate_of.as_deref().unwrap())
            .unwrap();
        assert!(canon_row.ingest_seq < dup.ingest_seq);

        // Re-scan: the link is stable — zero *new* duplicate links.
        let second = store.add_folder(dir_str(&dir)).unwrap();
        assert_eq!(second.duplicates, 0, "re-scan must not re-link");
        assert_eq!(second.added, 0);
        assert_eq!(second.updated, 0);
        cleanup(&dir);
    }

    /// A canonical row that has a hash must appear in `hash_rows` with its
    /// link cleared (None) — the dedup diff writes NULL-clears, so an
    /// un-duplicated row's stale link is removed.
    #[test]
    fn test_stale_duplicate_link_is_cleared() {
        let dir = test_dir();
        let a = dir.join("a");
        let b = dir.join("b");
        std::fs::create_dir_all(&a).unwrap();
        std::fs::create_dir_all(&b).unwrap();
        write_tagged_fixture("wav", "s1", "One", "A", None, None, &a);
        write_tagged_fixture("wav", "s2", "Two", "A", None, None, &b);

        let store = make_store();
        store.add_folder(dir_str(&dir)).unwrap();
        store.flush_hashes().unwrap();
        let mut rows = store.cache().all_tracks().unwrap();
        // No content duplicates yet: no links.
        assert!(rows.iter().all(|r| r.duplicate_of.is_none()));

        // Corrupt the DB-side state: pretend a link exists.
        let (victim, other) = (rows[0].id.clone(), rows[1].id.clone());
        store
            .cache()
            .apply_duplicate_links(&[(victim.clone(), Some(other.clone()))])
            .unwrap();

        // The next dedup pass (via a no-op rescan) must clear the stale link
        // because the content hashes differ.
        let report = store.add_folder(dir_str(&dir)).unwrap();
        assert_eq!(report.duplicates, 0, "different content: no new links");
        rows = store.cache().all_tracks().unwrap();
        let victim_row = rows.iter().find(|r| r.id == victim).unwrap();
        assert_eq!(
            victim_row.duplicate_of, None,
            "the stale link must be NULL-cleared"
        );
        cleanup(&dir);
    }

    // -----------------------------------------------------------------
    // Concurrency (hammer) tests
    // -----------------------------------------------------------------

    /// Eight threads hammer `add_folder` / `refresh` / `list` on a shared
    /// store simultaneously. The connection mutex must serialize every DB
    /// operation; regardless of interleaving the final database state must
    /// be sound: one row per file, unique contiguous `ingest_seq`s, both
    /// hashes applied, no duplicate rows.
    #[test]
    fn test_store_concurrent_hammer_sound_final_state() {
        let dir = test_dir();
        write_tagged_fixture("wav", "one", "Title One", "A", None, None, &dir);
        write_tagged_fixture("wav", "two", "Title Two", "A", None, None, &dir);
        let store = Arc::new(make_store());
        store.add_folder(dir_str(&dir)).unwrap();

        let dir_s = dir_str(&dir).to_string();
        let mut handles = Vec::new();
        for i in 0..8 {
            let store = Arc::clone(&store);
            let dir_s = dir_s.clone();
            handles.push(std::thread::spawn(move || {
                for _ in 0..8 {
                    match i % 3 {
                        0 => {
                            let _ = store.add_folder(&dir_s);
                        }
                        1 => {
                            let _ = store.refresh();
                        }
                        _ => {
                            let _ = store.list();
                        }
                    }
                    std::thread::sleep(std::time::Duration::from_millis(2));
                }
            }));
        }
        for h in handles {
            h.join().unwrap();
        }

        store.flush_hashes().unwrap();
        let rows = store.cache().all_tracks().unwrap();
        assert_eq!(rows.len(), 2, "exactly one row per file: {rows:?}");
        let mut seqs: Vec<i64> = rows.iter().map(|r| r.ingest_seq).collect();
        seqs.sort_unstable();
        assert_eq!(
            seqs,
            vec![1, 2],
            "ingest_seq must stay unique and contiguous"
        );
        let mut paths: Vec<String> = rows.iter().map(|r| r.file_path.clone()).collect();
        paths.sort();
        let mut deduped = paths.clone();
        deduped.dedup();
        assert_eq!(paths.len(), deduped.len(), "file_path must stay unique");
        assert!(
            rows.iter().all(|r| r.file_hash.is_some()),
            "both hashes applied"
        );
        let hashes: std::collections::HashSet<&str> = rows
            .iter()
            .map(|r| r.file_hash.as_deref().unwrap())
            .collect();
        assert_eq!(hashes.len(), 2, "distinct files, distinct hashes");
        cleanup(&dir);
    }

    /// Concurrent `list()` calls while a file has vanished: the lazy
    /// missing-mark races are benign (the write is idempotent) and every
    /// observer sees `missing` in its projection.
    #[test]
    fn test_concurrent_list_marks_missing_once() {
        let dir = test_dir();
        let path = write_tagged_fixture("wav", "song", "S", "A", None, None, &dir);
        let path = path.canonicalize().unwrap();
        let store = Arc::new(make_store());
        store.add_folder(dir_str(&dir)).unwrap();
        std::fs::remove_file(&path).unwrap();

        let mut handles = Vec::new();
        for _ in 0..4 {
            let store = Arc::clone(&store);
            handles.push(std::thread::spawn(move || {
                let list = store.list().unwrap();
                assert_eq!(list[0].availability, Availability::Missing);
            }));
        }
        for h in handles {
            h.join().unwrap();
        }
        let row = store
            .cache()
            .get_track_by_path(dir_str(&path))
            .unwrap()
            .unwrap();
        assert_eq!(row.availability, Availability::Missing);
        cleanup(&dir);
    }

    // -----------------------------------------------------------------
    // Full-lifecycle idempotency (the "zero changes" invariants)
    // -----------------------------------------------------------------

    /// Registering the same folder twice, refreshing twice, flushing twice,
    /// and listing twice must all be stable: the second of every operation
    /// produces zero transitions, and the list projection is identical.
    #[test]
    fn test_full_lifecycle_idempotent() {
        let dir = test_dir();
        write_tagged_fixture("wav", "one", "Title One", "A", None, None, &dir);
        write_tagged_fixture("wav", "two", "Title Two", "A", None, None, &dir);

        let store = make_store();
        let first_add = store.add_folder(dir_str(&dir)).unwrap();
        assert_eq!(first_add.added, 2);
        let second_add = store.add_folder(dir_str(&dir)).unwrap();
        assert_eq!(second_add.added, 0);
        assert_eq!(second_add.updated, 0, "zero-changes rescan");

        store.flush_hashes().unwrap();
        let first_dedup = store.flush_hashes().unwrap();
        assert_eq!(first_dedup.duplicates, 0, "no new links on the second pass");

        let first_refresh = store.refresh().unwrap();
        assert_eq!(first_refresh.updated, 0);
        assert_eq!(first_refresh.missing, 0);
        let second_refresh = store.refresh().unwrap();
        assert_eq!(second_refresh.updated, 0);
        assert_eq!(second_refresh.missing, 0);

        let first_list = store.list().unwrap();
        let second_list = store.list().unwrap();
        assert_eq!(first_list, second_list, "list is repeatable");
        assert_eq!(first_list.len(), 2);
        cleanup(&dir);
    }

    /// Delete → refresh (missing) → delete-again state → refresh: marking
    /// missing is idempotent at the report level (a row that is already
    /// missing produces no *new* missing transition on the second refresh).
    #[test]
    fn test_missing_marking_idempotent_across_refreshes() {
        let dir = test_dir();
        let path = write_tagged_fixture("wav", "song", "S", "A", None, None, &dir);
        let path = path.canonicalize().unwrap();
        let store = make_store();
        store.add_folder(dir_str(&dir)).unwrap();

        std::fs::remove_file(&path).unwrap();
        let first = store.refresh().unwrap();
        assert_eq!(first.missing, 1);
        let second = store.refresh().unwrap();
        assert_eq!(
            second.missing, 0,
            "an already-missing row must not transition again"
        );
        assert_eq!(second.updated, 0);
        cleanup(&dir);
    }
}
