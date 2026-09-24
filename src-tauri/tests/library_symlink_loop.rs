//! Regression tests for directory symlink cycles inside a library folder.
//!
//! `walk()` maintains a `HashSet` of already-visited canonical directories
//! and skips repeats, so any cycle (`folder/sub/loop -> folder`, or the
//! minimal `folder/self -> folder`) terminates instead of recursing until
//! the stack overflows — an unguarded walk would overflow and abort the
//! whole application (SIGABRT) on the next `library_add_folder` /
//! `library_refresh` scan.
//!
//! Both tests assert the desired behavior: the scan terminates and reports
//! each real file exactly once. They live in a dedicated integration-test
//! binary (one per risk class) rather than in the lib unit tests.

#![allow(unsafe_code)]

use std::os::unix::fs::symlink;
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};

use pulse_core_lib::library::scan_folder;

fn test_root() -> PathBuf {
    static COUNTER: AtomicU64 = AtomicU64::new(0);
    let dir = std::env::temp_dir().join(format!(
        "pulse-lib-loop-{}-{}",
        std::process::id(),
        COUNTER.fetch_add(1, Ordering::Relaxed)
    ));
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

/// A symlink in a subdirectory whose target is the scan root — the classic
/// user-created cycle (`sub/loop -> root`).
#[test]
#[cfg(unix)]
fn test_scan_folder_symlink_cycle_terminates() {
    // Regression: the cycle must collapse to the single real file.
    let root = test_root();
    let sub = root.join("sub");
    std::fs::create_dir_all(&sub).unwrap();
    std::fs::write(sub.join("real.mp3"), b"audio").unwrap();
    symlink(&root, sub.join("loop")).unwrap();

    let found = scan_folder(&root);

    assert_eq!(
        found.len(),
        1,
        "cycle must collapse to the single real file, got {found:?}"
    );
    assert!(found[0].path.ends_with("real.mp3"));
    let _ = std::fs::remove_dir_all(&root);
}

/// The minimal cycle: a symlink pointing at its own containing directory.
#[test]
#[cfg(unix)]
fn test_scan_folder_self_symlink_terminates() {
    // Regression: `self -> root` is a one-step cycle and must add no
    // phantom files.
    let root = test_root();
    std::fs::write(root.join("a.mp3"), b"a").unwrap();
    std::fs::write(root.join("b.mp3"), b"b").unwrap();
    symlink(&root, root.join("self-loop")).unwrap();

    let found = scan_folder(&root);

    let paths: Vec<String> = found
        .iter()
        .map(|f| f.path.to_string_lossy().to_string())
        .collect();
    assert_eq!(
        paths.len(),
        2,
        "self-loop must add no phantom files: {paths:?}"
    );
    let _ = std::fs::remove_dir_all(&root);
}
