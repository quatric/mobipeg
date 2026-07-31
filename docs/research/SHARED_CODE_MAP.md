# Shared Code Map

What in the mobipeg/mobiclip encoder is format-defined (shared, not removable, not
platform-specific) versus what is this project's own choice (3DS-specific tuning,
safely adjustable). Grounded in direct source reading and validation from this
session's Step A/B/C work, not filename inference.

## Format-defined, shared, do not touch without re-validating against the real decoder

These exist because the MobiClip bitstream and the real hardware/software decoder
require them. Changing them risks producing files that fail to decode correctly on
any target, 3DS included. All were read directly and several were independently
validated bit-exact against the real production functions this session:

- **VLC symbol tables** (`vx2_table0_a/b`, `vx2_table1_a/b`, `vx2_table*_rev`,
  `x264_coeff_token`, `x264_total_zeros`, `x264_8_run_before`, etc.) — the
  entropy-coding tables. Validated via the Step A oracle (9,590 cases, 0 mismatches).
- **Quantization tables** (`mobi_q4`, `mobi_q8`, `mobi_q8_chroma`, `mobi_zigzag4x4`,
  `mobi_zigzag8x8`) — decoder-matched dequant tables. Reused verbatim across every
  harness this session built.
- **The whole-block CAVLC-family walk** (`encode_dct` in `cavlc.c`: skip-run tracking,
  the `lastnonzero==0` empty-block case, the `if(i==lastnonzero) break` early exit) —
  format syntax, not a tunable. Step B's `block_rate()` re-walk had to reproduce this
  exactly; an initial miss of the break condition caused 61/16,864 validation
  mismatches before the fix.
- **The single shared per-slice quantizer header field**
  (`(sh->i_qp % 6) + 12 + 6*mobi_qyx(h)`, written once in `encoder.c`, read by
  `setup_qtables()` in the decoder) — this is *why* independent chroma QYX was
  rejected this session: the format has no per-plane quantizer signal. Any future
  chroma-specific tuning must work within this constraint (as `MOBI_CHROMA_DZ` does,
  by changing only the forward-quant rounding threshold, never the signaled step).
- **The luma re-quantization overshoot guard** (`[-64,319]` clamp, iterative
  coefficient attenuation) — reproduces the real decoder's lookup-table bounds
  (`MinMaxTable`), inherited via the WiiLink24/Gericom research lineage. Chroma has
  no equivalent guard (confirmed by reading `mobi_add8x8_idct8`) — this asymmetry is
  a real format property, not an oversight to "fix."
- **CAVLC-only entropy coding** — this fork removes CABAC project-wide for MobiClip
  mode (per `MOBIPEG_PROJECT_STATE.md`); there is no CABAC path to preserve or strip.
- **4×4-only intra prediction, QP range [12,161]** — decoder-enforced constraints
  from the same patch.

## This project's own choice, safely 3DS-specific, tunable without format risk

- **Output resolution 400×240** — a `-vf scale/pad` choice at the ffmpeg command
  line, not a codec-level constant. The GUI plan must compute this from source
  aspect ratio rather than hardcoding 400×224 (the test-clip-specific value used
  throughout this session's own diagnostic encodes) — flagged explicitly in the
  user's own GUI plan and confirmed correct here.
- **QP / `-mobi_qyx` values** — encoder-side rate/quality choices within the
  decoder-enforced QP range. Candidate B's QP25/QYX3 is this session's accepted
  control, not a format requirement.
- **`MOBI_DZ` / `MOBI_CHROMA_DZ` rounding deadzone** — pure encoder-side rounding
  threshold, proven bitstream-compatible this session precisely because it never
  touches the shared quantizer step or header field.
- **Q-RDO (rate-distortion-optimized mode decision using the real bit-cost oracle)**
  — an encoder-side search strategy, not a format requirement.
- **SIMD vectorization of RDO/SAD/reconstruction hot paths** — pure speed work,
  provably bit-identical to the scalar path (per this session's own commit history:
  "perf(encoder): SSE2-vectorize..." commits).
- **Thread count, keyint/GOP structure** — standard encoder-side controls.

## Explicitly rejected this session — do not resurrect as "3DS-specific" features

- **Independent chroma QYX** — impossible without breaking bitstream compatibility
  (the shared single-field constraint above). Closed.
- **Single-coefficient rate-blind pruning** (Step B) and **0→±1 low-frequency chroma
  activation** (Step C) — both tested with real validated tooling, both came back
  negative/weak signal. Closed unless new evidence reopens them.
- **Keyframe/GOP reduction for size** — no meaningful improvement, tested and closed.

## Correction: the codec is shared, but the product surface is not

Everything above (VLC tables, quant tables, the guard, the single shared quantizer
header field) is genuinely format-defined and platform-uniform — that conclusion
holds. What does **not** hold, and was corrected after the user pushed back: the
existing `encode.py`/`encode_gui.py` wrapper around this codec has a real, explicit
Wii/3DS/DS format selector (`mo`=Wii 624×352, `moflex`/`moflex3d`=3DS 400×240 flat/
stereoscopic, `mods`=DS 256×192), defaulting to Wii. See `CURRENT_GUI_STATUS.md` and
`LEGACY_FEATURE_INVENTORY.csv` for the verified detail. The lesson: "the codec has
no platform branches" and "the product has no platform selector" are different
claims, and only the first was actually true.

## Boundary with the 3DS player (`source/moflex/`)

Not fully traced this pass (see `LEGACY_FEATURE_INVENTORY.csv`). What's known: the
player's `source/moflex/decoder/mobiclip.c` decodes MobiClip video for playback,
independent of whether this project's own encoder produced it or the official
Nintendo encoder did — decode compatibility with official files was explicitly
confirmed earlier in this investigation (`MOBIPEG_PROJECT_STATE.md`: "the same
decoder correctly reads the real Nintendo-produced official `.moflex` sample too").
The encoder GUI/CLI being planned here does not need to modify or ship the player;
it only needs to produce files the player (or the official Nintendo software) can
already decode. Confirming the exact relationship between `source/moflex/decoder/`
and `libavcodec/mobiclip.c` (are they the same code, forked, or independent
reimplementations?) is flagged as an open question for whoever picks up the player
side — out of scope for this GUI/repo plan.
