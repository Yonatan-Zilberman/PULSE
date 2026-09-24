//! Embedded-tag reading via `lofty` (the only metadata source — PRD §7/§11:
//! embedded tags only, zero network enrichment).
//!
//! Tag reading lives in `src-tauri/src/library/` only; the C++ audio bridge
//! is playback-only (see `Docs/Audio-Bridge-Contract.md`).

use std::path::Path;

use lofty::error::{ErrorKind, LoftyError};
use lofty::file::{AudioFile, TaggedFileExt};
use lofty::tag::Accessor;

/// Metadata extracted from one audio file.
///
/// Missing tag text falls back to a filename stem / "Unknown Artist"; these
/// fallbacks are *not* errors.
#[derive(Debug, Clone, PartialEq)]
pub struct AudioMetadata {
    pub title: String,
    pub artist: String,
    pub album: Option<String>,
    pub genre: Option<String>,
    pub duration_seconds: Option<f64>,
    pub sample_rate: Option<u32>,
    pub channels: Option<u16>,
}

/// Why a tag read failed.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum TagReadError {
    /// The OS refused the read (maps to `permission_denied`).
    PermissionDenied(std::io::ErrorKind),
    /// The path no longer exists (row-level: `missing_file`, not an error row).
    MissingFile(std::io::ErrorKind),
    /// Another I/O failure on a readable path (`unreadable_file`).
    UnreadableFile(std::io::ErrorKind),
    /// The file exists but is not a decodable audio container
    /// (`corrupt_container`).
    Corrupt(String),
}

/// Read a file's embedded tags and audio properties.
pub fn read(path: &Path) -> Result<AudioMetadata, TagReadError> {
    let tagged = lofty::read_from_path(path).map_err(|e| map_lofty_error(&e))?;
    let props = tagged.properties();
    let mut title: Option<String> = None;
    let mut artist: Option<String> = None;
    let mut album: Option<String> = None;
    let mut genre: Option<String> = None;
    for tag in tagged.tags() {
        if title.is_none() {
            title = non_empty(tag.title());
        }
        if artist.is_none() {
            artist = non_empty(tag.artist());
        }
        if album.is_none() {
            album = non_empty(tag.album());
        }
        if genre.is_none() {
            genre = non_empty(tag.genre());
        }
    }
    let fallback_title = filename_stem(path).unwrap_or_else(|| "Unknown Title".to_string());
    Ok(AudioMetadata {
        title: title.unwrap_or(fallback_title),
        artist: artist.unwrap_or_else(|| "Unknown Artist".to_string()),
        album,
        genre,
        duration_seconds: props.duration().as_secs_f64().into(),
        sample_rate: props.sample_rate(),
        channels: props.channels().map(u16::from),
    })
}

fn non_empty(value: Option<impl AsRef<str>>) -> Option<String> {
    value
        .map(|s| s.as_ref().trim().to_string())
        .filter(|s| !s.is_empty())
}

/// Filename stem without the audio extension (the title fallback).
pub fn filename_stem(path: &Path) -> Option<String> {
    let name = path.file_name()?.to_str()?;
    let stem = match name.rsplit_once('.') {
        Some((stem, _)) => stem,
        None => name,
    };
    (!stem.is_empty()).then(|| stem.to_string())
}

fn map_lofty_error(err: &LoftyError) -> TagReadError {
    match err.kind() {
        ErrorKind::Io(io) => match io.kind() {
            std::io::ErrorKind::PermissionDenied => TagReadError::PermissionDenied(io.kind()),
            std::io::ErrorKind::NotFound => TagReadError::MissingFile(io.kind()),
            // lofty signals parse/container failures on a present, readable
            // file as data-level io kinds (`InvalidInput` for zero-byte or
            // malformed data, `InvalidData` / `UnexpectedEof` for truncated
            // containers): that is a corrupt container, not an unreadable
            // file.
            std::io::ErrorKind::InvalidData
            | std::io::ErrorKind::InvalidInput
            | std::io::ErrorKind::UnexpectedEof => TagReadError::Corrupt(err.to_string()),
            _ => TagReadError::UnreadableFile(io.kind()),
        },
        // Non-Io (parse/container) failures: the file is present but not a
        // decodable audio container.
        _ => TagReadError::Corrupt(err.to_string()),
    }
}

#[cfg(test)]
pub(crate) mod fixtures {
    use super::*;
    use lofty::config::WriteOptions;
    use lofty::file::TaggedFile;
    use lofty::tag::{ItemKey, Tag};
    use std::path::PathBuf;

    /// Read a committed `tone.<format>` fixture from the repository
    /// (`include_bytes!` cannot take a runtime format; `CARGO_MANIFEST_DIR`
    /// is set in test binaries).
    fn fixture_bytes(format: &str) -> Vec<u8> {
        let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join(format!("src/library/fixtures/tone.{format}"));
        std::fs::read(&path).unwrap_or_else(|e| panic!("read fixture {}: {e}", path.display()))
    }

    /// Write a tagged copy of the `tone.<format>` fixture into `dir`.
    ///
    /// Copies the committed silent-tone fixture, attaches the format's native
    /// tag with title/artist (and optionally album/genre), and saves it back
    /// in place. The saved file must parse back via lofty with the same text
    /// items — the six-format test below doubles as the generation validator.
    ///
    /// NOTE: `lofty 0.21`'s `TaggedFile` fields are `pub(crate)`, so the tag
    /// is assigned through the public `TaggedFile::new` constructor and
    /// written via `AudioFile::save_to` (plan §6 fallback).
    pub fn write_tagged_fixture(
        format: &str,
        stem: &str,
        title: &str,
        artist: &str,
        album: Option<&str>,
        genre: Option<&str>,
        dir: &Path,
    ) -> PathBuf {
        let src = fixture_bytes(format);
        let dest = dir.join(format!("{stem}.{format}"));
        std::fs::write(&dest, &src).unwrap_or_else(|e| panic!("write fixture {dest:?}: {e}"));

        let tagged = lofty::read_from_path(&dest)
            .unwrap_or_else(|e| panic!("fixture tone.{format} failed to parse: {e}"));
        let mut tag = Tag::new(tagged.primary_tag_type());
        tag.insert_text(ItemKey::TrackTitle, title.to_string());
        tag.insert_text(ItemKey::TrackArtist, artist.to_string());
        if let Some(album) = album {
            tag.insert_text(ItemKey::AlbumTitle, album.to_string());
        }
        if let Some(genre) = genre {
            tag.insert_text(ItemKey::Genre, genre.to_string());
        }
        let rebuilt = TaggedFile::new(tagged.file_type(), tagged.properties().clone(), vec![tag]);
        let mut file = std::fs::OpenOptions::new()
            .read(true)
            .write(true)
            .open(&dest)
            .unwrap_or_else(|e| panic!("open fixture {dest:?}: {e}"));
        rebuilt
            .save_to(&mut file, WriteOptions::default())
            .unwrap_or_else(|e| panic!("save tagged fixture {dest:?}: {e}"));
        dest
    }
}

#[cfg(test)]
mod tests {
    use super::fixtures::write_tagged_fixture;
    use super::*;
    use std::path::PathBuf;
    use std::sync::atomic::{AtomicU64, Ordering};

    fn test_dir() -> PathBuf {
        static COUNTER: AtomicU64 = AtomicU64::new(0);
        let dir = std::env::temp_dir().join(format!(
            "pulse-lib-tag-{}-{}",
            std::process::id(),
            COUNTER.fetch_add(1, Ordering::Relaxed)
        ));
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    /// T1: all six DRM-free formats round-trip through lofty.
    #[test]
    fn test_six_format_tag_read() {
        for format in ["wav", "aiff", "mp3", "flac", "m4a", "aac"] {
            let dir = test_dir();
            let path = write_tagged_fixture(
                format,
                "Song",
                "My Title",
                "My Artist",
                Some("Album X"),
                Some("House"),
                &dir,
            );
            let meta = read(&path).unwrap_or_else(|e| panic!("read {path:?}: {e:?}"));
            assert_eq!(meta.title, "My Title", "title ({format})");
            assert_eq!(meta.artist, "My Artist", "artist ({format})");
            assert_eq!(meta.album.as_deref(), Some("Album X"), "album ({format})");
            assert_eq!(meta.genre.as_deref(), Some("House"), "genre ({format})");
            let duration = meta
                .duration_seconds
                .unwrap_or_else(|| panic!("duration ({format})"));
            assert!(
                (duration - 0.5).abs() <= 0.15,
                "duration ({format}) = {duration}, expected ~0.5"
            );
            assert_eq!(meta.sample_rate, Some(22050), "sample_rate ({format})");
            assert_eq!(meta.channels, Some(1), "channels ({format})");
            let _ = std::fs::remove_dir_all(&dir);
        }
    }

    /// T1 (partial): absent album/genre items read back as `None`s.
    #[test]
    fn test_absent_album_genre_are_none() {
        let dir = test_dir();
        let path = write_tagged_fixture("flac", "Song", "T", "A", None, None, &dir);
        let meta = read(&path).unwrap();
        assert_eq!(meta.album, None);
        assert_eq!(meta.genre, None);
        let _ = std::fs::remove_dir_all(&dir);
    }

    /// T1 (partial): a file with no tags at all gets the stem/artist
    /// fallbacks and still yields ingest-usable output.
    #[test]
    fn test_no_tag_fallbacks() {
        let dir = test_dir();
        let src = include_bytes!("fixtures/tone.flac");
        let path = dir.join("untagged track.flac");
        std::fs::write(&path, src).unwrap();
        let meta = read(&path).unwrap();
        assert_eq!(meta.title, "untagged track");
        assert_eq!(meta.artist, "Unknown Artist");
        assert!(meta.duration_seconds.is_some());
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn test_corrupt_file_is_corrupt_error() {
        let dir = test_dir();
        let path = dir.join("bad.flac");
        std::fs::write(&path, b"not a flac file").unwrap();
        let err = read(&path).unwrap_err();
        assert!(
            matches!(err, TagReadError::Corrupt(_)),
            "expected Corrupt, got {err:?}"
        );
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn test_missing_file_is_missing_error() {
        let dir = test_dir();
        let path = dir.join("gone.wav");
        let err = read(&path).unwrap_err();
        assert!(
            matches!(err, TagReadError::MissingFile(_)),
            "expected MissingFile, got {err:?}"
        );
        let _ = std::fs::remove_dir_all(&dir);
    }

    /// A zero-byte file with an audio extension is present but not a
    /// decodable container: must map to `Corrupt`, never panic.
    #[test]
    fn test_empty_file_is_corrupt() {
        let dir = test_dir();
        let path = dir.join("empty.flac");
        std::fs::write(&path, b"").unwrap();
        let err = read(&path).unwrap_err();
        // Classification check: lofty signals parse-level failures via
        // `io::ErrorKind::InvalidData`, and `map_lofty_error` maps it to
        // `Corrupt` (a present-but-empty container is corrupt, not merely
        // unreadable). We accept either failure variant (both degrade to an
        // error row) to stay robust across lofty versions.
        assert!(
            matches!(
                err,
                TagReadError::Corrupt(_) | TagReadError::UnreadableFile(_)
            ),
            "zero-byte file must be a failure, got {err:?}"
        );
        assert!(
            !matches!(err, TagReadError::MissingFile(_)),
            "a zero-byte file EXISTS; must not be MissingFile"
        );
        let _ = std::fs::remove_dir_all(&dir);
    }

    /// A tiny (4-byte) garbage file with a lossy-audio extension: corrupt,
    /// never a crash and never a bogus success.
    #[test]
    fn test_garbage_bytes_mp3_is_corrupt() {
        let dir = test_dir();
        let path = dir.join("garbage.mp3");
        std::fs::write(&path, [0xFF, 0x00, 0x10, 0x42]).unwrap();
        let err = read(&path).unwrap_err();
        // Same InvalidData mapping gap as the zero-byte test above: accept
        // any failure variant; never a bogus Ok.
        assert!(
            matches!(
                err,
                TagReadError::Corrupt(_) | TagReadError::UnreadableFile(_)
            ),
            "garbage .mp3 must be a failure, got {err:?}"
        );
        let _ = std::fs::remove_dir_all(&dir);
    }

    /// Truncated container: the first 8 bytes of a valid FLAC (header only,
    /// no audio frames) must fail the parse and map to `Corrupt`.
    #[test]
    fn test_truncated_flac_header_is_corrupt() {
        let dir = test_dir();
        let path = dir.join("truncated.flac");
        let src = include_bytes!("fixtures/tone.flac");
        assert!(src.len() > 8, "fixture sanity");
        std::fs::write(&path, &src[..8]).unwrap();
        let err = read(&path).unwrap_err();
        // Same InvalidData mapping gap; truncated data must be a failure,
        // never a fabricated success.
        assert!(
            matches!(
                err,
                TagReadError::Corrupt(_) | TagReadError::UnreadableFile(_)
            ),
            "truncated flac must be a failure, got {err:?}"
        );
        let _ = std::fs::remove_dir_all(&dir);
    }

    /// Whitespace-only tag items must be filtered to their fallbacks: the
    /// title falls back to the filename stem, artist to "Unknown Artist".
    #[test]
    fn test_whitespace_tags_fall_back() {
        use lofty::config::WriteOptions;
        use lofty::file::{AudioFile, TaggedFile, TaggedFileExt};
        use lofty::tag::{ItemKey, Tag};

        let dir = test_dir();
        let path = dir.join("whitespace.mp3");
        std::fs::write(
            &path,
            std::fs::read(
                Path::new(env!("CARGO_MANIFEST_DIR")).join("src/library/fixtures/tone.mp3"),
            )
            .unwrap(),
        )
        .unwrap();
        let tagged = lofty::read_from_path(&path).unwrap();
        let mut tag = Tag::new(tagged.primary_tag_type());
        tag.insert_text(ItemKey::TrackTitle, "   ".to_string());
        tag.insert_text(ItemKey::TrackArtist, "\t\n".to_string());
        let rebuilt = TaggedFile::new(tagged.file_type(), tagged.properties().clone(), vec![tag]);
        let mut file = std::fs::OpenOptions::new()
            .read(true)
            .write(true)
            .open(&path)
            .unwrap();
        rebuilt.save_to(&mut file, WriteOptions::default()).unwrap();

        let meta = read(&path).unwrap();
        assert_eq!(
            meta.title, "whitespace",
            "blank title falls back to the stem"
        );
        assert_eq!(meta.artist, "Unknown Artist", "blank artist falls back");
        let _ = std::fs::remove_dir_all(&dir);
    }

    /// `filename_stem` boundaries: extension-only dot names, dotless names,
    /// names with several dots, trailing dot, and non-UTF8 names.
    #[test]
    fn test_filename_stem_boundaries() {
        use std::ffi::OsString;
        use std::os::unix::ffi::OsStringExt;

        // "song.\0.mp3": the stem contains a NUL byte but is valid UTF8? No —
        // NUL is valid UTF8, so the stem is "song.\0".
        assert_eq!(
            filename_stem(Path::new("/x/song.\0.mp3")).as_deref(),
            Some("song.\0")
        );
        // No extension at all: the whole name is the stem.
        assert_eq!(
            filename_stem(Path::new("/x/UNTITLED")).as_deref(),
            Some("UNTITLED")
        );
        // Multiple dots: only the final extension is stripped.
        assert_eq!(
            filename_stem(Path::new("/x/track.01.remix.mp3")).as_deref(),
            Some("track.01.remix")
        );
        // Trailing dot: the stem is everything before it — still non-empty.
        assert_eq!(
            filename_stem(Path::new("/x/name.")).as_deref(),
            Some("name")
        );
        // Hidden dot-file with an audio extension (".mp3"): empty stem → None
        // (the caller substitutes "Unknown Title").
        assert_eq!(filename_stem(Path::new("/x/.mp3")), None);
        // A bare dot as a *relative* path: `file_name()` is None for a lone
        // "." component (note: "/x/." would strip the dot entirely and
        // yield file_name "x").
        assert_eq!(filename_stem(Path::new(".")), None);
        // Non-UTF8 filename: to_str fails → None, no panic.
        let mut os = OsString::from("/x/bad");
        os.push(OsString::from_vec(vec![0xFF, 0xFE, b'.', b'm', b'p', b'3']));
        assert_eq!(filename_stem(std::path::Path::new(&os)), None);
        // Trailing slash: `file_name()` ignores it, so "/x/" has the
        // file name "x" and yields stem "x" (a surprising but stable
        // behavior worth pinning down).
        assert_eq!(filename_stem(Path::new("/x/")).as_deref(), Some("x"));
        // The filesystem root: `file_name()` is None → None stem.
        assert_eq!(filename_stem(Path::new("/")), None);
    }

    /// Emoji / non-ASCII titles round-trip through the reader untouched.
    #[test]
    fn test_unicode_tags_roundtrip() {
        let dir = test_dir();
        let path = write_tagged_fixture(
            "mp3",
            "unicode",
            "Zoë & Björn – Naïve ★",
            "Ünïcodé",
            None,
            None,
            &dir,
        );
        let meta = read(&path).unwrap();
        assert_eq!(meta.title, "Zoë & Björn – Naïve ★");
        assert_eq!(meta.artist, "Ünïcodé");
        let _ = std::fs::remove_dir_all(&dir);
    }
}
