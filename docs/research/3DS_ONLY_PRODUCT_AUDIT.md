# 3DS-Only Product Audit — Mobipeg 3DS Planning

Read-only audit. No files deleted, no code changed, nothing published. Scope confirmed
with the user: "Mobipeg 3DS" is a **new, standalone** product built around the
mobipeg/mobiclip encoder — separate from `mivf-gui`, the existing MIVF player/toolkit,
and its M2Y1/M2Y2 codec, which are all explicitly out of scope and untouched.

## The single most important finding

**"Mobipeg" is not this project's own code.** It is an existing, independent
open-source project: a fork of FFmpeg itself, maintained at
`https://github.com/quatric/mobipeg.git`, with a companion patched x264 fork at
`https://github.com/quatric/x264.git` (commit `92dc38457e22a3624224e27cf75c32136c072279`,
"add mobiclip support"). The real upstream checkout lives at `C:\dev\mobipeg`
(confirmed: `git remote -v` → `quatric/mobipeg.git`, HEAD `038641d038 avformat/mobiclip:
add mobiclip support`). `C:\dev\MIVF\mobipeg` — the tree all of this session's quality
work happened in — holds **only investigation artifacts** (this was stated explicitly
in `mobipeg/project_state/MOBIPEG_PROJECT_STATE.md`, written by an earlier phase of this
same investigation, and independently reconfirmed here).

This means "Mobipeg 3DS" will be a **fork of a fork**: FFmpeg → quatric/mobipeg →
Mobipeg 3DS, with a second lineage arm x264 → quatric/x264 → (the patched encoder this
session hardened). Every license and attribution obligation from both upstream chains
carries forward. See `PUBLIC_REPOSITORY_PLAN.md` for the specific requirements this
creates.

## Second important finding: the low-level codec is shared, but the product surface
## has real, concrete Wii-only and DS-only code — corrected after further tracing

**This section originally concluded there was no Wii/DS code to strip. That was
wrong, and the user correctly pushed back on it.** The error: I traced the MobiClip
*codec's* internal tables/VLC/reconstruction logic (genuinely format-uniform, not
platform-branched — see `SHARED_CODE_MAP.md`) and stopped there, without checking
whether the *product surface* (the GUI/CLI wrapper around that codec) had its own
platform selection. It does.

`C:\dev\mobipeg\encode_gui.py` (found on a second pass — a real search miss the
first time, since it's a top-level file, not a `*gui*`-named directory) contains,
verbatim:

```python
self.enc_fmt_var = tk.StringVar(value="Wii Mobiclip .mo")
self.formats_map = {
    "Wii Mobiclip .mo": "mo",
    "3DS Mobiclip .moflex (2D)": "moflex",
    "3DS Mobiclip .moflex (3D)": "moflex3d",
    "DS Mobiclip .mods": "mods"
}
```

`encode.py` confirms what each targets (resolution, demuxer name): `mo` → Wii,
624×352; `moflex`/`moflex3d` → 3DS, 400×240 (flat and stereoscopic 3D
respectively); `mods` → DS/DSi, 256×192 (DS's exact native resolution). Full detail,
with confidence labels, in `CURRENT_GUI_STATUS.md` and
`LEGACY_FEATURE_INVENTORY.csv`.

**Corrected consequence:** the MobiClip codec's own tables/VLC/reconstruction rules
remain genuinely shared and format-defined (that part of the original conclusion
was right) — but the existing product's *format selector*, defaulting to Wii, is
real, concrete legacy to remove, and `mo`/`mods` are the specific paths that don't
belong in Mobipeg 3DS. Separately, `moflex3d` (3DS stereoscopic 3D) is a genuine
3DS-exclusive feature this planning pass's GUI/CLI architecture does not yet cover —
flagged as an open scope question, not silently dropped. A further batch of other
Nintendo-adjacent format additions (VX, RVID, KWZ, PPMFLIP, THP) exists at the
FFmpeg-codec level but was not traced to any confirmed current call site — marked
`Unknown`/unresolved rather than guessed in either direction.

## Third finding, corrected: GUI source does exist, with confirmed provenance

An earlier pass of this audit concluded no GUI source existed, based on searching
only for directories named `*gui*`. That missed `C:\dev\mobipeg\encode_gui.py` (a
top-level file, not a directory). Corrected: `encode_gui.py` (Tkinter, 252 lines)
wraps `encode.py` (argparse CLI, 211 lines) via `subprocess`, and
`mobipeg-gui.spec` (a PyInstaller spec, confirmed present) builds exactly this
source into the `mobipeg-gui.exe`/`.app` found in `mobipeg_release/`. Provenance is
therefore confirmed, same repository and license as the rest of upstream mobipeg —
this is not an unknown-provenance binary.

**Consequence, per explicit instruction:** treat this as a reference for behavior
only (it demonstrates a real, working "thin wrapper — subprocess plus argument
array, no encoder logic in the GUI" pattern worth following) — do not reverse-engineer
or repackage the compiled binary, and do not simply strip its format selector down to
build the new product. The new GUI should be planned as a clean, source-controlled
frontend around the accepted Mobipeg/Mobiclip CLI, informed by this reference's
design principle, not inheriting its Tkinter implementation or its multi-platform
scope. Full detail in `CURRENT_GUI_STATUS.md`.

## Fourth finding, corrected: a CLI wrapper exists too, but it is multi-platform

`C:\dev\mobipeg\encode.py` is a real, working CLI wrapper (`encode.py mo|moflex|
moflex3d|mods <audio> <input> ...`), predating and separate from every raw
`ffmpeg -c:v mobiclip ...` invocation this session's own investigation work has run
directly against the patched binary (this session's work always drove ffmpeg
directly, never through `encode.py`). Same correction as the GUI: this is a real
precedent for a thin CLI wrapper, but it targets four platforms/formats including
Wii (`mo`) and DS (`mods`), so `mobipeg3ds`'s CLI is a narrowed, re-scoped surface —
built following this reference's wrapper pattern, not a copy of its format list.

## Scope and depth of this audit

Given the actual size of `C:\dev\MIVF` (hundreds of dated experiment/backup
directories spanning a multi-week investigation, plus the entirely separate MIVF
player/GUI/M2Y product), a complete file-by-file trace of every historical directory
was not attempted. This audit traced:

- The real upstream mobipeg/x264 checkouts (`C:\dev\mobipeg`, license/credit files,
  git history) — directly, with commands, not by filename guess.
- The accepted encoder state this session itself produced and validated (Candidate B
  and its lineage — SIMD work, QP calibration, Step A/B/C — all summarized in
  `mobipeg/project_state/MOBIPEG_PROJECT_STATE.md`, which this audit re-confirms is
  the accurate current-state record, not the older `research/` notes).
- The mobipeg-side investigation tree's actual infrastructure directories
  (`scripts/`, `tests/`, `patches/`, `research/`, `builds/`, `project_state/`) —
  confirmed empty/populated by direct listing, not assumption.
- The absence of a GUI/CLI source tree — confirmed by direct search, not inference
  from directory names.

What this audit did **not** do: line-by-line classification of every one of the ~150+
dated phase/backup directories under `C:\dev\MIVF` (most predate the mobipeg
investigation and belong to the separate MIVF player/GUI project, out of scope per
the clarified goal). `LEGACY_FEATURE_INVENTORY.csv` classifies the ones inspected and
flags the rest as `Unknown` rather than guessing.

## Summary table (corrected)

| Question | Answer | Confidence |
|---|---|---|
| Does the MobiClip *codec itself* have Wii/DS-specific logic to remove? | No — its tables/VLC/reconstruction rules are one shared, format-defined implementation | Verified |
| Does the *product surface* (GUI/CLI) have Wii/DS-specific code to remove? | **Yes** — `encode_gui.py`'s format selector (defaults to Wii) and `encode.py`'s `mo`/`mods` paths are concrete, confirmed legacy | Verified |
| Does a GUI source exist to reference? | Yes — `encode_gui.py`, confirmed same source as the packaged binary via `mobipeg-gui.spec` | Verified |
| Does a CLI wrapper exist to reference? | Yes — `encode.py`, multi-platform, needs narrowing not copying | Verified |
| Is there a real 3DS-exclusive feature (stereoscopic 3D) already implemented upstream? | Yes — `moflex3d`; not yet covered by this planning pass's architecture | Verified, open scope question |
| Is "Mobipeg" this project's own code? | No — third-party fork of FFmpeg + x264, GPL/LGPL-encumbered | Verified |
| Are VX/RVID/KWZ/PPMFLIP/THP relevant to Mobipeg 3DS? | Unresolved — added upstream, not wired into the GUI/CLI surface, no call site traced | Unresolved |
| Is the accepted quality baseline documented? | Yes — Candidate B, `MOBIPEG_PROJECT_STATE.md`, this session's Step A/B/C reports | Verified |
| Is mivf-gui/M2Y in scope? | No — explicitly out of scope per user clarification | Confirmed by user |
