# SUPER MOFLEX Builder

Turns a `.moflex` video into a **self-contained** one: a second audio language, every subtitle
track, the library info and the poster all travel inside the file — while stock players keep
playing it as an ordinary movie.

Runs on **macOS, Windows and Linux**. Python plus ffmpeg, nothing else to install.

<p align="center">
  <img src="docs/screenshot.png" alt="SUPER MOFLEX Builder" width="720">
</p>

## What it produces

A normal-looking `.moflex` (the extension has to stay `.moflex`) that carries:

| Where | What | Who sees it |
|---|---|---|
| in-band audio stream | one language, volume-normalized (English by default) | **every** player, including the original one |
| trailer, past the last block | second audio language | players that know about the trailer |
| trailer | every subtitle track, language-tagged | ditto |
| trailer | title, year, genres, description, runtime, poster | ditto — the library needs no lookup |

The trailer sits after the last video block, where players that don't know about it never look.
That is the whole trick: extra *in-band* streams break the original player (it hangs on an unknown
descriptor, and chokes to about 1 fps on an extra audio stream), while trailing bytes are
invisible to it.

## Requirements

- **Python 3.8+** — [python.org](https://www.python.org/downloads/) (on Windows tick *Add Python
  to PATH*; Tkinter is included).
- **ffmpeg**, including `ffprobe` (every distribution ships both). The app checks on startup and
  says so plainly if it is missing — a warning bar across the window with install instructions
  for your platform — so you find out before starting a build rather than during one.
  - macOS: `brew install ffmpeg`, or a build from [evermeet.cx](https://evermeet.cx/ffmpeg/)
  - Windows: `winget install Gyan.FFmpeg`, or the "release full" zip from
    [gyan.dev](https://www.gyan.dev/ffmpeg/builds/) — copy `ffmpeg.exe` and `ffprobe.exe` into an
    `ffmpeg\` folder next to the app and it finds them
  - Linux: `sudo apt install ffmpeg` (or dnf/pacman)

  Installed somewhere unusual? *Tools ▸ Locate ffmpeg…* — pointing at `ffmpeg` picks up
  `ffprobe` beside it, and the choice is remembered.
- Disk: roughly the size of the encode plus ~600 MB per hour of audio for scratch WAVs.
- RAM: the container is rewritten in memory, so allow about 4× the size of the encode
  (a 1.5 GB feature film peaked near 7 GB).

## Using it

```
python3 smoflex_gui.py          # macOS / Linux
py smoflex_gui.py               # Windows
```

1. **Source video** — the original MKV/MP4. Its audio and subtitles are what get embedded.
2. **Encoded .moflex** — your mobiclip encode of the same title. The video comes from here; the
   encode's own audio is replaced.
3. **Output folder** — the finished file is named from the metadata, with an `(S)` marker:
   `Movie Name (2004) (3D) (S).moflex`, or
   `Show Name (2026) (S) - S01e02 - Episode Title.moflex`.
4. Pick which audio goes in-band (what stock players hear) and which becomes the second track.
5. Add `.srt` files for any language the source only has as *picture* subtitles — Blu-ray rips
   carry PGS images, which cannot be turned into text without OCR. Name them
   `Whatever.eng.srt`, `Whatever.jpn.srt` so each track ends up labelled.
6. **Sync offset** — leave at 0 when the encode was made from this same file. If it came from a
   different master (one that opens with a logo or leader the source lacks), set the number of
   seconds the encode runs ahead. Measure it, don't guess: decode the encode's own audio and
   cross-correlate it against the source track. One real case needed **+14.446 s**, flat across
   the whole film. The offset moves the audio and the source's own subtitle tracks; `.srt` files
   you added are left alone, because downloaded subtitles are usually already timed to the same
   master the encode came from (`--srt-offset` if they are not).
7. **Build**. Audio extraction is the slow part; the log tells you where it is, and *Cancel*
   stops it without leaving a half-written file.

Subtitle order in the finished file is English first (the default track), then Japanese, then
everything else alphabetically.

**Metadata** is yours to control. Every field — title, year, episode title, air/release date,
genres, runtime, category, description, poster — can be typed in, and what you type always wins.
A TMDB read token is optional: with one, **Look up** fills those fields in so you can see and
edit them before building, and Build fetches them anyway if you leave them blank. Get a token
free from *themoviedb.org ▸ Settings ▸ API*. Nothing is embedded that you did not see.

Everything is verified after writing: the trailer is walked back section by section, because a
flaky network share once zeroed the tail of a file that had reported success.

## Command line

Same pipeline, no window:

```
python3 smoflex_build.py source.mkv encoded.moflex -d ./super \
        --srt Movie.eng.srt --srt Movie.jpn.srt --format converted
```

`--help` lists the rest. `TMDB_TOKEN` in the environment works instead of `--tmdb`.

## Files

| File | Role |
|---|---|
| `smoflex_gui.py` | the desktop app |
| `smoflex_build.py` | the pipeline: probe → extract → metadata → assemble → verify (importable, and a CLI) |
| `smoflex_core.py` | container surgery: ADPCM encoding, frame-accurate repack, trailer writing |

## Licence

MIT — see [LICENSE](LICENSE).

## Notes and limits

- Subtitles must be **text**. PGS/VobSub tracks are reported and skipped.
- Audio is resampled to 44.1 kHz stereo — that is what the format carries.
- Normalization is peak-only: one constant multiplier so the loudest sample lands at −1 dBFS.
  No compression, no dynamics touched, and clipping is impossible by construction.
- A no-op repack is byte-identical to the source file, so the video is never re-encoded or
  degraded by this tool.
