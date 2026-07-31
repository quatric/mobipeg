# Current GUI Status (corrected)

**Correction notice:** an earlier pass of this document concluded "no GUI source
exists" based on searching only for directories named `*gui*`. That search missed
top-level Python files. This is now corrected with the actual source read directly.

## A real, source-available GUI exists — VERIFIED

`C:\dev\mobipeg\encode_gui.py` (252 lines, Tkinter) wraps `C:\dev\mobipeg\encode.py`
(211 lines, argparse-based CLI) via `subprocess`. `mobipeg-gui.spec` (a PyInstaller
spec file, confirmed present) builds exactly `encode_gui.py` into
`mobipeg-gui.exe` / `mobipeg-gui.app` — this is the actual source of the compiled
binary found in `mobipeg_release/`, not an unknown-provenance artifact. Same repo,
same license as the rest of upstream mobipeg (LGPL v2.1+/GPL v2+, per its
`LICENSE.md`).

## The GUI already has exactly the platform selector the target product must not have — VERIFIED

`encode_gui.py:68-73`, read directly:

```python
self.enc_fmt_var = tk.StringVar(value="Wii Mobiclip .mo")
self.formats_map = {
    "Wii Mobiclip .mo": "mo",
    "3DS Mobiclip .moflex (2D)": "moflex",
    "3DS Mobiclip .moflex (3D)": "moflex3d",
    "DS Mobiclip .mods": "mods"
}
```

This is a **literal, explicit, currently-shipping Wii/3DS/DS format dropdown,
defaulting to Wii**. `encode.py`'s format table (read directly) confirms what each
targets:

| GUI label | `fmt` value | demuxer | scale | target (confidence) |
|---|---|---|---|---|
| Wii Mobiclip .mo | `mo` | `mobiclip_mo` | 624×352 | Wii — **likely** (resolution doesn't match any Nintendo handheld; GUI explicitly labels it Wii) |
| 3DS Mobiclip .moflex (2D) | `moflex` | `moflex` | 400×240 | 3DS — **verified** (this session's entire encoder investigation targets exactly this resolution/format) |
| 3DS Mobiclip .moflex (3D) | `moflex3d` | `moflex` | 400×240 per eye, side-by-side | 3DS stereoscopic — **verified** (3DS is the only Nintendo handheld with an autostereoscopic 3D screen; the GUI wires an `--input2`/right-eye field specifically for this mode) |
| DS Mobiclip .mods | `mods` | `mods` | 256×192 | DS/DSi — **likely** (256×192 is the DS/DSi's exact native screen resolution) |

**This directly overturns the earlier draft's "no Wii/DS code to strip" conclusion.**
That conclusion was based on tracing the MobiClip *codec's* internal tables/logic
(genuinely format-uniform, that part still stands — see `SHARED_CODE_MAP.md`), not
the *product surface* choosing which format to target. The product surface has real,
concrete Wii-only and DS-only paths, verified at the source level.

## Also found, not yet resolved: VX / RVID / KWZ / PPMFLIP / THP

A single upstream commit (`e562b22e5e`, "Add VX, RVID, KWZ, PPMFLIP, THP format
support") added substantial new codec/demuxer/muxer source
(`libavcodec/vx*.c`, `libavcodec/rvid.c`, `libavformat/kwzdec.c`,
`libavformat/ppmflipdec.c`, `libavformat/rvid*.c`, `libavformat/thpenc.c`) — genuine
other-format additions, likely Nintendo-platform-related (KWZ = Flipnote Studio 3D,
a 3DS application format; PPMFLIP = the original DSi Flipnote Studio format; THP =
GameCube/Wii video; VX and RVID origin not determined this pass). **Unresolved:**
grepping `encode.py`/`encode_gui.py` found no reference to any of these — they are
not exposed through the current GUI's format selector. Whether they're invoked by
some other script (the commit also added `rvid.py`, which no longer exists at HEAD),
used only for research/testing, or genuinely dead code was not determined. Do not
classify these as removable or required until someone actually traces their current
call sites — flagged `Unknown` in `LEGACY_FEATURE_INVENTORY.csv`, not guessed either
way.

## Contradiction found and resolved: `encode.py` does not use this session's accepted quality settings at all

`encode.py`'s `moflex` path (re-verified by direct grep across the whole file) never
passes `-qp` or `-mobi_qyx` under any code path. Its `enc_opts` for `moflex` is
exactly `["-mo_audio", audio, "-c:v", "mobiclip", "-mobiclip", "1"]` — every encode
runs at whatever the patched x264's *default* QP/QYX are, not Candidate B's
accepted QP25/QYX3. This means the existing reference implementation's output
quality/size is unrelated to this entire investigation's findings — it predates and
is disconnected from the Step A/B/C and QP-calibration work entirely.

**Resolution for Mobipeg 3DS:** the new backend must explicitly pass `-qp` and
`-mobi_qyx` (and `MOBI_QYX_RDO`, `MOBI_CHROMA_DZ` once its own visual verdict lands)
resolved from the job's quality preset — never fall back to unstated encoder
defaults the way `encode.py` does. The comparison tests (see
`mobipeg-3ds-public/tests/`) assert on the *shared* elements (`-c:v mobiclip`,
`-mobiclip 1`, `scale=400:240`) and explicitly document this QP/QYX difference
rather than asserting false equivalence.

## What this means for "Mobipeg 3DS"

- The existing `encode.py`/`encode_gui.py` pair is a legitimate, working reference
  for *how thin the wrapper can be* (subprocess call, argument array, no encoder
  logic in the GUI layer) — worth following that principle.
- It is **not** a source to strip down in place. Per explicit instruction, the new
  GUI should be a clean, source-controlled frontend built for this planning phase's
  own architecture (versioned job model, staged verification, etc.), informed by
  this reference rather than inheriting its code or its Tkinter implementation.
- **`moflex3d` (stereoscopic 3D) is a real, currently-implemented, 3DS-exclusive
  feature** that the GUI/CLI architecture plan drafted so far does not cover (it
  only describes flat 2D output). This needs a deliberate decision, not a silent
  drop — recommend treating it as an explicit open scope question for the next
  planning pass rather than assuming Mobipeg 3DS is 2D-only by default.
- `mo` (Wii) and `mods` (DS) are the concrete legacy paths to exclude from the new
  product's format surface — confirmed, not assumed.
