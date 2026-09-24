# Library test fixtures

Six DRM-free audio fixtures used by the `library` test suite (tag-reader
round-trips, ingest state machine, dedup). Each file is a **0.5 s, 440 Hz
sine tone, 22 050 Hz, mono**, with **no embedded tags** (tests attach tags at
runtime via `tag_reader::fixtures::write_tagged_fixture`).

The committed bytes are the source of truth; do not regenerate casually.
`write_tagged_fixture` copies them into a temp dir before tagging, so the
fixtures themselves are never modified.

## Regeneration recipe

```bash
cd src-tauri/src/library/fixtures
# One command per format: ffmpeg does not reliably apply -ar to multi-output runs.
for fmt in wav aiff mp3 flac m4a aac; do
  ffmpeg -hide_banner -loglevel error -f lavfi -i "sine=frequency=440:duration=0.5" \
    -ar 22050 -ac 1 -y "tone.$fmt"
done
```

(`afconvert`/`sox` are acceptable substitutes per format.) After generating,
run `cargo test --lib tag_reader` — the six-format test doubles as the
generation validator: if a generated file fails to parse via lofty, fix the
generation, not the test.
