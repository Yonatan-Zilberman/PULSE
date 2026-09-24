//! Folder discovery: canonicalized recursive walks of user-added library
//! folders.
//!
//! All paths are canonicalized *before* extension matching (so extension
//! filtering sees the real suffix after symlink resolution) and results are
//! deduplicated + sorted by canonical path (two symlinks to one file yield
//! one entry). Unreadable subdirectories are skipped without aborting the
//! scan. Visited canonical directories are tracked, so directory symlink
//! cycles terminate instead of recursing to a stack overflow.

use std::path::{Path, PathBuf};
use std::time::UNIX_EPOCH;

use super::errors::LibraryError;

/// The six DRM-free formats PULSE ingests (matched case-insensitively).
pub const AUDIO_EXTENSIONS: &[&str] = &["mp3", "wav", "flac", "aac", "m4a", "aiff"];

/// A file found during a folder scan.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DiscoveredFile {
    /// Canonicalized absolute path (the row key).
    pub path: PathBuf,
    pub size: u64,
    /// Unix seconds.
    pub mtime: i64,
}

/// Observed filesystem state of a path.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FsState {
    /// The path does not exist (`ENOENT`): the file genuinely vanished.
    Missing,
    /// The metadata call failed for a reason other than absence (e.g.
    /// `EACCES` on a permission-revoked or unmounted parent directory):
    /// the row keeps its prior state — permission failures are owned by
    /// the tag reader, and a stat failure is not evidence of deletion.
    Unreachable,
    Present {
        size: u64,
        mtime: i64,
    },
}

/// Validate a user-supplied folder path: it must exist, be a directory, and
/// be canonicalized.
pub fn validate_folder(path: &str) -> Result<PathBuf, LibraryError> {
    let p = Path::new(path);
    if !p.exists() {
        return Err(LibraryError::FolderNotFound(path.to_string()));
    }
    if !p.is_dir() {
        return Err(LibraryError::NotADirectory(path.to_string()));
    }
    p.canonicalize().map_err(LibraryError::from)
}

/// Recursively scan `root` for audio files.
///
/// Results are sorted lexicographically by canonical path and deduplicated
/// by it. Per-entry failures (unreadable dirs, broken symlinks) are skipped
/// and never abort the scan.
pub fn scan_folder(root: &Path) -> Vec<DiscoveredFile> {
    let mut found: Vec<DiscoveredFile> = Vec::new();
    let mut visited: std::collections::HashSet<PathBuf> = std::collections::HashSet::new();
    if let Ok(canonical_root) = root.canonicalize() {
        walk(&canonical_root, &mut found, &mut visited);
    }
    found.sort_by(|a, b| a.path.cmp(&b.path));
    found.dedup_by(|a, b| a.path == b.path);
    found
}

fn walk(
    dir: &Path,
    out: &mut Vec<DiscoveredFile>,
    visited: &mut std::collections::HashSet<PathBuf>,
) {
    // Skip directories we already entered: without this, any directory
    // symlink cycle (e.g. `sub/loop -> root`) would recurse until the
    // stack overflows and aborts the process.
    if !visited.insert(dir.to_path_buf()) {
        return;
    }
    let Ok(entries) = std::fs::read_dir(dir) else {
        // Unreadable subtree: skip it, continue with the rest.
        return;
    };
    for entry in entries.flatten() {
        if entry.file_type().is_err() {
            continue;
        }
        let path = entry.path();
        // `fs::metadata` follows symlinks (broken links simply fail here).
        let Ok(meta) = std::fs::metadata(&path) else {
            continue;
        };
        // Canonicalize so symlink loops / duplicate targets collapse.
        let Ok(canonical) = path.canonicalize() else {
            continue;
        };
        if meta.is_dir() {
            walk(&canonical, out, visited);
            continue;
        }
        if is_audio_file(&canonical) {
            let size = meta.len();
            let mtime = mtime_of(&meta);
            out.push(DiscoveredFile {
                path: canonical,
                size,
                mtime,
            });
        }
    }
}

/// Case-insensitive extension check on the canonicalized path.
fn is_audio_file(path: &Path) -> bool {
    path.extension()
        .and_then(|e| e.to_str())
        .is_some_and(|ext| {
            AUDIO_EXTENSIONS
                .iter()
                .any(|known| ext.eq_ignore_ascii_case(known))
        })
}

/// Current filesystem state of `path` (metadata-only; read permission
/// problems are surfaced by the tag reader, not here).
///
/// Only `ENOENT` classifies as [`FsState::Missing`]; any other metadata
/// failure (e.g. `EACCES` on a revoked parent directory) is
/// [`FsState::Unreachable`] — callers keep the row's prior state rather
/// than flipping a merely-unreachable file to missing.
pub fn fs_state(path: &Path) -> FsState {
    match std::fs::metadata(path) {
        Ok(meta) => FsState::Present {
            size: meta.len(),
            mtime: mtime_of(&meta),
        },
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => FsState::Missing,
        Err(_) => FsState::Unreachable,
    }
}

/// Unix seconds of the file's modification time (0 if unavailable).
fn mtime_of(meta: &std::fs::Metadata) -> i64 {
    let Ok(modified) = meta.modified() else {
        return 0;
    };
    let Ok(since_epoch) = modified.duration_since(UNIX_EPOCH) else {
        return 0;
    };
    i64::try_from(since_epoch.as_secs()).unwrap_or(i64::MAX)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn test_root() -> PathBuf {
        use std::sync::atomic::{AtomicU64, Ordering};
        static COUNTER: AtomicU64 = AtomicU64::new(0);
        std::env::temp_dir().join(format!(
            "pulse-lib-test-{}-{}",
            std::process::id(),
            COUNTER.fetch_add(1, Ordering::Relaxed)
        ))
    }

    #[test]
    fn test_validate_folder_errors() {
        let err = validate_folder("/definitely/not/a/real/dir/xyz123");
        assert!(matches!(err, Err(LibraryError::FolderNotFound(_))));
        let file = test_root().with_extension("afile");
        std::fs::create_dir_all(file.parent().unwrap()).unwrap();
        std::fs::write(&file, b"not a dir").unwrap();
        let err = validate_folder(file.to_str().unwrap());
        assert!(matches!(err, Err(LibraryError::NotADirectory(_))));
    }

    #[test]
    fn test_scan_finds_audio_and_dedupes() {
        let root = test_root();
        let sub = root.join("sub");
        std::fs::create_dir_all(&sub).unwrap();
        let a = root.join("a.mp3");
        let s = sub.join("b.WAV"); // case-insensitive match
        let n = root.join("note.txt");
        std::fs::write(&a, b"audio").unwrap();
        std::fs::write(&s, b"audio").unwrap();
        std::fs::write(&n, b"text").unwrap();
        // A symlink duplicate of `a` under the subfolder.
        #[cfg(unix)]
        {
            std::os::unix::fs::symlink(&a, sub.join("link.mp3")).unwrap();
        }

        let found = scan_folder(&root);
        let paths: Vec<String> = found
            .iter()
            .map(|f| f.path.to_string_lossy().to_string())
            .collect();
        // Two real audio files; the symlink collapses onto `a`'s canonical path.
        assert_eq!(paths.len(), 2);
        assert!(paths.iter().any(|p| p.ends_with("a.mp3")));
        assert!(paths.iter().any(|p| p.ends_with("b.WAV")));
        // Sorted lexicographically.
        let mut sorted = paths.clone();
        sorted.sort();
        assert_eq!(paths, sorted);
        assert!(found.iter().all(|f| f.size == 5));
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn test_fs_state() {
        let root = test_root();
        std::fs::create_dir_all(&root).unwrap();
        let f = root.join("x.flac");
        std::fs::write(&f, b"data").unwrap();
        assert!(matches!(fs_state(&f), FsState::Present { size: 4, .. }));
        std::fs::remove_file(&f).unwrap();
        assert_eq!(fs_state(&f), FsState::Missing);
        let _ = std::fs::remove_dir_all(&root);
    }

    /// A path whose *parent* directory lost its execute/read permission
    /// (e.g. `chmod 000`, or a revoked mount) is not a vanished file: the
    /// metadata call fails with EACCES, not ENOENT, and the row must keep
    /// its prior state (the tag reader owns permission failures).
    #[test]
    #[cfg(unix)]
    fn test_fs_state_permission_error_is_not_missing() {
        use std::os::unix::fs::PermissionsExt;
        let root = test_root();
        std::fs::create_dir_all(&root).unwrap();
        let f = root.join("x.flac");
        std::fs::write(&f, b"data").unwrap();
        std::fs::set_permissions(&root, std::fs::Permissions::from_mode(0o000)).unwrap();
        // Running as root bypasses permission bits; probe and skip if so.
        let still_reachable = matches!(fs_state(&f), FsState::Present { .. });
        if still_reachable {
            std::fs::set_permissions(&root, std::fs::Permissions::from_mode(0o755)).unwrap();
            let _ = std::fs::remove_dir_all(&root);
            return;
        }
        let state = fs_state(&f);
        assert!(
            !matches!(state, FsState::Missing),
            "EACCES must not be classified as a vanished file, got {state:?}"
        );
        std::fs::set_permissions(&root, std::fs::Permissions::from_mode(0o755)).unwrap();
        let _ = std::fs::remove_dir_all(&root);
    }

    /// Runs `scan_folder` on a thread that must finish within 5 s; a
    /// runaway walk (symlink cycle) fails the test instead of hanging CI.
    fn scan_with_timeout(root: &Path) -> Vec<DiscoveredFile> {
        use std::sync::mpsc;
        let (tx, rx) = mpsc::channel();
        let root = root.to_path_buf();
        std::thread::spawn(move || {
            let files = scan_folder(&root);
            let _ = tx.send(files);
        });
        rx.recv_timeout(std::time::Duration::from_secs(5))
            .unwrap_or_else(|_| panic!("scan_folder did not terminate within 5 s (symlink loop?)"))
    }

    /// Paths containing NUL bytes are invalid at the OS level and must be
    /// rejected with a typed error, never a panic or a raw I/O error.
    #[test]
    fn test_validate_folder_rejects_null_bytes() {
        let err = validate_folder("/tmp/\0music");
        assert!(
            matches!(err, Err(LibraryError::FolderNotFound(_))),
            "NUL path must be FolderNotFound, got {err:?}"
        );
    }

    /// Hostile user-supplied path strings degrade to typed errors.
    #[test]
    fn test_validate_folder_hostile_inputs() {
        // Empty string: does not exist → FolderNotFound (no panic).
        assert!(matches!(
            validate_folder(""),
            Err(LibraryError::FolderNotFound(_))
        ));
        // Path traversal tokens are legal paths (cwd's parent) — must either
        // validate as a real directory or reject; never panic.
        let dotdot = validate_folder("..");
        assert!(
            dotdot
                .as_ref()
                .map_or(true, |p| p.is_absolute() && p.is_dir()),
            "'..' must resolve to a real absolute directory, got {dotdot:?}"
        );
        // Whitespace-only: not a directory.
        let err = validate_folder("   ");
        assert!(matches!(err, Err(LibraryError::FolderNotFound(_)),));
    }

    // NOTE: symlink-cycle regression tests deliberately live in the
    // dedicated `tests/library_symlink_loop.rs` integration binary (one
    // binary per risk class), not in the lib unit tests.

    /// An unreadable nested directory is skipped; readable siblings still
    /// surface. (Skipped when running as root — root bypasses permissions.)
    #[test]
    fn test_scan_unreadable_subdir_skipped() {
        use std::os::unix::fs::PermissionsExt;
        let root = test_root();
        let secret = root.join("secret");
        std::fs::create_dir_all(&secret).unwrap();
        std::fs::write(root.join("ok.mp3"), b"ok").unwrap();
        std::fs::write(secret.join("hidden.mp3"), b"hid").unwrap();
        std::fs::set_permissions(&secret, std::fs::Permissions::from_mode(0o0)).unwrap();
        let root_readable = std::fs::read_dir(&secret).is_err();
        if !root_readable {
            // Running as root: permission bits are bypassed; nothing to test.
            let _ = std::fs::set_permissions(&secret, std::fs::Permissions::from_mode(0o755));
            let _ = std::fs::remove_dir_all(&root);
            return;
        }
        let found = scan_with_timeout(&root);
        let names: Vec<String> = found
            .iter()
            .map(|f| f.path.file_name().unwrap().to_string_lossy().to_string())
            .collect();
        assert!(
            names.contains(&"ok.mp3".to_string()),
            "readable file still found: {names:?}"
        );
        assert!(
            !names.contains(&"hidden.mp3".to_string()),
            "unreadable subtree must be skipped: {names:?}"
        );
        let _ = std::fs::set_permissions(&secret, std::fs::Permissions::from_mode(0o755));
        let _ = std::fs::remove_dir_all(&root);
    }

    /// A large (10 MiB) file must be reported with an exact size; the
    /// scanner only stats files, never buffers them.
    #[test]
    fn test_scan_large_file_exact_size() {
        let root = test_root();
        std::fs::create_dir_all(&root).unwrap();
        let big = root.join("big.flac");
        let payload = vec![0xA5u8; 10 * 1024 * 1024];
        std::fs::write(&big, &payload).unwrap();
        let found = scan_with_timeout(&root);
        assert_eq!(found.len(), 1);
        assert_eq!(found[0].size, (10 * 1024 * 1024) as u64);
        let _ = std::fs::remove_dir_all(&root);
    }

    /// A single 1-byte file: the smallest non-empty collection member.
    #[test]
    fn test_scan_tiny_file() {
        let root = test_root();
        std::fs::create_dir_all(&root).unwrap();
        std::fs::write(root.join("tiny.wav"), [0x01]).unwrap();
        let found = scan_folder(&root);
        assert_eq!(found.len(), 1);
        assert_eq!(found[0].size, 1);
        let _ = std::fs::remove_dir_all(&root);
    }

    /// Two distinct symlinks pointing at the same file collapse to one
    /// entry even when the duplicates are non-adjacent before sorting.
    #[test]
    fn test_scan_symlink_duplicates_collapse() {
        let root = test_root();
        let a = root.join("a");
        let b = root.join("b");
        std::fs::create_dir_all(&a).unwrap();
        std::fs::create_dir_all(&b).unwrap();
        let real = a.join("same.mp3");
        std::fs::write(&real, b"same").unwrap();
        #[cfg(unix)]
        {
            std::os::unix::fs::symlink(&real, a.join("link1.mp3")).unwrap();
            std::os::unix::fs::symlink(&real, b.join("link2.mp3")).unwrap();
        }
        let found = scan_with_timeout(&root);
        assert_eq!(
            found.len(),
            1,
            "two links to one file must yield one entry: {found:?}"
        );
        let _ = std::fs::remove_dir_all(&root);
    }
}
