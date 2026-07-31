# GUI and CLI Architecture Plan

Both surfaces share one authoritative job model and both ultimately shell out to the
confirmed-working ffmpeg flags listed in `CURRENT_GUI_STATUS.md`. Neither duplicates
encoder logic.

## Recommended stack (recommendation only — not implemented this phase)

**Python + PySide6**, with the patched FFmpeg/Mobipeg CLI remaining the sole
authoritative encoder — the GUI is a process-driving frontend, never a second
encoder implementation.

Why, given what this audit actually found:
- The existing reference (`encode_gui.py`) is Tkinter, which is fine for a
  quick internal tool but not for a maintainable, source-controlled public product
  (limited styling, no native async progress widgets, dated look).
- PySide6 (Qt for Python, LGPL-licensed) gives a proper native-feeling desktop app
  on Windows/macOS/Linux, real progress/threading primitives for the staged
  Encode/Verify flow, and is a common, well-documented choice for exactly this kind
  of "wrap a CLI tool with a friendly desktop frontend" product.
- Staying in Python keeps the whole toolchain (GUI + CLI + job-model schema) in one
  language, matching the existing reference's own choice and minimizing new
  contributor friction.
- Explicitly not chosen: Electron/web-stack (heavier runtime for a simple
  file-in/file-out tool), a compiled-native GUI toolkit (larger jump from the
  existing Python reference for no clear benefit here).

This is a recommendation to validate once implementation actually starts, not a
locked decision — flagged as such per the "plan only, don't implement" scope of
this phase.

## Shared job model (versioned)

```
EncodeJob v1
├── source: { path, probed_stream_info }
├── video_stream_index
├── audio_stream_index | null   (video-only allowed)
├── picture: { mode: fit|fill|custom_crop, custom_crop?, target_fps }
├── quality: { preset: balanced|anime_hq|maximum|custom, revision, advanced?: {
│       qp, mobi_qyx, q_rdo: bool, chroma_dz?, threads } }
├── audio: { mode: recommended|video_only, downmix_policy }
├── output: { path, temp_dir }
└── verification_policy: { software_decode: bool, sha256: bool }
```

Presets resolve to concrete `advanced` values; `custom` exposes them directly. Preset
*values* are explicitly NOT finalized in this plan — the DZ visual verdict and any
Phase 4/5 localization findings are still open, so "Anime High Quality" etc. are
placeholders pointing at Candidate B's QP25/QYX3 until real acceptance data exists.

The GUI generates an `EncodeJob`, serializes it, and invokes the backend (a thin
process wrapper translating the job into the exact ffmpeg argument array) — never
constructs ffmpeg arguments inline in GUI code, and never reimplements quantizer or
rate-control logic in the GUI layer.

`.mobipegproj` (optional, later): a saved `EncodeJob` plus source reference, for
resuming a configuration. Not a queue system — single-job reliability first, per
explicit instruction.

## GUI: Source → Picture → Quality → Audio → Output → Review → Encode/Verify

- **Source**: drag-and-drop or file picker; probe via ffprobe-equivalent for
  duration/resolution/fps/aspect/streams; warn on unusual sources (e.g. no video
  stream, exotic pixel format) rather than silently failing later.
- **Picture**: output is always 400×240, but the *how* (fit-with-bars vs fill-crop
  vs custom crop) must compute filter-chain parameters from the source's actual
  aspect ratio — do not hardcode the 400×224 active-picture value this session used
  for its specific anime test clip. Frame-rate choices validated against what the
  encoder/format actually accepts.
  **Open scope question, not resolved this pass:** the existing reference CLI
  (`encode.py`) already implements a working `moflex3d` path — 3DS stereoscopic 3D,
  400×240 per eye, side-by-side, with a right-eye input. This is genuinely
  3DS-exclusive functionality, not legacy to strip. Whether v1 of Mobipeg 3DS
  includes a "Picture: 2D / Stereoscopic 3D" choice (with a second source/right-eye
  field when 3D is selected) or defers it to a later version is a real product
  decision the next planning pass should make deliberately, not by default omission.
- **Quality**: presets on top, `Custom`/Advanced Mode exposes the confirmed-working
  raw controls only (QP, effective `QP%6` tier, QYX, Q-RDO toggle, `MOBI_CHROMA_DZ`
  *only once its own visual verdict lands* — do not expose it as an ordinary control
  before that). Never surface a rejected/research-only control (independent chroma
  QYX, coefficient-pruning knobs) as a normal setting.
- **Audio**: friendly track selection, a single recommended 3DS-compatible mode,
  explicit stereo/downmix behavior, video-only option. Do not expose arbitrary
  ffmpeg audio codecs unless each one is separately validated for the 3DS target.
- **Output**: fixed `.moflex` extension, destination + free-space + overwrite +
  temp-space checks. Show an estimated *size range*, not an exact figure — the
  encoder is fixed-QP, not target-bitrate (`-b:v` is confirmed non-functional).
- **Review**: a plain-language summary of every value above plus the literal
  resolved ffmpeg argument array, so the "authoritative encoder command" is always
  inspectable, echoing the existing MIVF product's own "Review the authoritative
  encoder command" pattern (a good precedent worth reusing, even though that
  product itself is out of scope here).
- **Encode/Verify**: staged progress (Preparing → Encoding video → Encoding audio →
  Writing MOFLEX → Finalizing → Verifying → Complete), frame count/%/speed/
  elapsed/remaining/current size, safe cancellation. Write to `.moflex.partial`
  until verification passes, then rename.

## Verification, staged and clearly labeled

Automatic (always run): container/codec recognition, 400×240 coded size check,
frame rate check, expected-vs-actual frame count, full software decode, audio
presence/duration, decoder error scan, SHA-256 recorded.

Explicitly separated in the UI, never conflated:
- **Software verification** (the automatic checks above) — proves the file is
  well-formed and decodes without error in software.
- **Azahar testing** — an emulator; useful for interface-level and playback
  behavior checks, not proof of physical hardware behavior (this exact conflation
  risk is already called out in the existing MIVF README's own development-status
  note, and applies identically here).
- **Physical Nintendo 3DS testing** — the only real proof of on-hardware behavior.

## CLI

```
mobipeg3ds inspect input.mp4
mobipeg3ds encode input.mp4 --preset anime-hq --output output.moflex
mobipeg3ds verify output.moflex
mobipeg3ds presets
mobipeg3ds version
```

Every subcommand constructs the same `EncodeJob` structure the GUI does and drives
the same backend — the CLI is not a second implementation.

Discontinued options (Wii/DS/generic-platform/codec/container selectors) do not
apply here since none currently exist to discontinue (per `CURRENT_GUI_STATUS.md`,
there is no prior CLI at all) — but if a user reasonably expects such a flag from
general ffmpeg familiarity and passes one, the response should be explicit:

```
This Mobipeg 3DS release supports Nintendo 3DS MOFLEX output only.
```

Never silently ignore an unrecognized platform/format flag.
