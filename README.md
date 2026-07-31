# Mobipeg 3DS

**Experimental Preview.** A dedicated desktop encoder for creating Nintendo
3DS-compatible MOFLEX video files.

Mobipeg 3DS is an independent community fork of
[Mobipeg](https://github.com/quatric/mobipeg) (itself a fork of
[FFmpeg](https://github.com/FFmpeg/FFmpeg)) focused exclusively on Nintendo
3DS MOFLEX creation. It is not affiliated with or endorsed by Nintendo.

## Development status

Pre-alpha, experimental preview. This repository currently provides:
- A shared job model and ffmpeg-argument backend (`mobipeg3ds/`).
- A CLI (`inspect`, `encode` with `--dry-run`, `presets`, `version`).
- An experimental PySide6 GUI (see below).

Quality presets are **explicitly not finalized** — "Balanced", "Anime High
Quality", and "Maximum Quality" all currently resolve to the same values
(QP25/QYX3, Q-RDO enabled, single-threaded video encoding), carried over as a
placeholder from this project's own encoder-quality investigation
(`docs/research/`). Do not treat these values as final or as
Nintendo-equivalent — see `docs/research/` for what's actually been tested
and what's still open.

## What Mobipeg 3DS does

Takes a common video file and produces a Nintendo 3DS-compatible `.moflex`
(400×240, MobiClip video, 3DS-compatible audio).

Only flat 2D output is implemented end-to-end right now. The reference
implementation this project forked from (`encode.py`/`encode_gui.py`) already
has a working stereoscopic-3D (`moflex3d`) path, but it is deliberately
deferred here — not playback-verified, not exposed by this CLI/GUI. Passing
`target="moflex3d"` to the backend raises an explicit error rather than
silently producing an unverified job.

## Supported formats — deliberately narrow

This project supports **Nintendo 3DS 2D MOFLEX only.** The reference
implementation's Wii (`.mo`) and DS (`.mods`) formats are not exposed here at
all, by design — see `docs/research/3DS_ONLY_PRODUCT_AUDIT.md` for the full
audit of what was and wasn't carried forward, and why.

## CLI usage

```bash
mobipeg3ds inspect input.mp4
mobipeg3ds encode input.mp4 --preset balanced --output output.moflex --dry-run
mobipeg3ds encode input.mp4 --preset balanced --output output.moflex
mobipeg3ds presets
mobipeg3ds version
```

`--dry-run` resolves and prints the exact ffmpeg command (including any
environment variables it needs) without starting an encode.

## GUI

An experimental PySide6 GUI is included (`mobipeg3ds/gui/`). Launch with:

```bash
python -m mobipeg3ds.gui
```

It implements Source → Picture → Quality → Audio → Output → Review → Encode →
Verify, using the exact same backend the CLI uses — the GUI never constructs
ffmpeg arguments itself.

## Verification

Three genuinely different things, never conflated:
- **Software verification** (built in): does the file decode cleanly, correct
  frame count, SHA-256 recorded. Proves the file is well-formed in software.
- **Emulator testing** (not yet part of this project): interface/playback
  behavior in an emulator such as Azahar. Not proof of hardware behavior.
- **Physical Nintendo 3DS testing**: the only real proof of on-hardware
  behavior. Not yet performed against anything produced by this codebase.

## Known limitations

- Fixed-QP only; there is no exact target-bitrate control (`-b:v` is
  confirmed non-functional for this encoder).
- Quality presets are placeholders, not a finished lineup.
- Stereoscopic 3D (`moflex3d`) is deferred.
- No packaging/installer yet; run from source.
- GUI is a first experimental preview, not a finished product.

## Building from source

Requires Python 3.10+. The GUI needs `PySide6`:

```bash
pip install -e ".[gui]"
```

The patched ffmpeg/x264 source lives at the repository root (`libavcodec/`,
`libavformat/`, `fftools/`, `x264-src/`, etc.) in the same layout as upstream
Mobipeg — it is not vendored under a separate subdirectory. Building it
follows the same process as upstream Mobipeg/FFmpeg.

## Project lineage

```
FFmpeg (github.com/FFmpeg/FFmpeg)
  -> Mobipeg (github.com/quatric/mobipeg) -- adds MobiClip support
    -> Mobipeg 3DS (this repository) -- 3DS-only GUI/CLI and quality work

x264 (videolan.org/x264)
  -> quatric/x264 (commit 92dc3845, "add mobiclip support")
    -> (patched encoder used by Mobipeg 3DS)
```

MobiClip research credit: PlayMobic, MobiclipDecoder (Gericom), Gericom's
x264 fork, WiiLink24's FFmpeg fork — see `CREDITS_MOBICLIP.md` (carried
forward verbatim from upstream). This fork's own additions are credited in
`CREDITS_MOBIPEG3DS.md`.

## Licensing and third-party notices

LGPL v2.1+ (overall) / GPL v2+ (x264-linked build), per upstream FFmpeg's own
`LICENSE.md`. `COPYING.*` files ship unmodified. No personal relicense.
