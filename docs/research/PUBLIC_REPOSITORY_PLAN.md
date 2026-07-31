# Public Repository Plan

Planning only. Nothing created, pushed, or published in this phase.

## Licensing and attribution — non-negotiable, traced from the real upstream

Mobipeg 3DS is a fork of a fork:

```
FFmpeg  →  quatric/mobipeg (github.com/quatric/mobipeg)  →  Mobipeg 3DS
x264    →  quatric/x264 (commit 92dc3845, "add mobiclip support")  →  (patched encoder)
```

Confirmed directly from `C:\dev\mobipeg`:
- `LICENSE.md`: LGPL v2.1+ applies overall; GPL v2+ applies to the parts enabled by
  `--enable-gpl` (which this fork's x264 linkage requires).
- `COPYING.GPLv2`, `COPYING.GPLv3`, `COPYING.LGPLv2.1`, `COPYING.LGPLv3` — all present
  upstream, must ship verbatim, unmodified, in the new repo.
- `CREDITS_MOBICLIP.md` — names the actual research lineage (PlayMobic,
  MobiclipDecoder/Gericom, Gericom's x264 fork, WiiLink24's FFmpeg fork). This must
  carry forward; it is not this project's own research to claim credit for.
- The x264 side needs its own equivalent credit to `quatric/x264` and, transitively,
  to `Gericom/x264`.

**Do not replace these with a new personal license** (explicit instruction, and also
a legal requirement given GPL/LGPL copyleft — a from-scratch relicense is not an
available option regardless of preference).

## Target fork

The planned public repository is `Oldhimaster1/Mobipeg-3DS` (matching the same
GitHub identity already used for `Oldhimaster1/MIVF`, per `CREDITS.md`). **Not
interacted with in this phase** — no remote created, no push, no repository
creation. Recorded here purely as the agreed target name for when implementation
actually starts.

## Proposed structure

```
C:\dev\MIVF\mobipeg                  research workspace — preserve unchanged
C:\dev\MIVF\mobipeg-3ds-public        future clean public repository (not created yet)
C:\dev\MIVF\mobipeg-3ds-archive       optional archival bundle/snapshot (not created yet)
```

Do not clean the research workspace in place. It remains the working investigation
tree for as long as encoder work continues (Phase 4/5 localization per the quality
roadmap is still open).

### Public repo layout (proposed, for when it's actually created)

```
mobipeg-3ds-public/                  (created this phase, local clone of C:\dev\mobipeg)
├── COPYING.GPLv2, COPYING.GPLv3, COPYING.LGPLv2.1, COPYING.LGPLv3   (verbatim upstream, unchanged by the clone)
├── LICENSE.md                                                       (verbatim upstream)
├── CREDITS_MOBICLIP.md                                              (verbatim upstream)
├── CREDITS_MOBIPEG3DS.md                                            (new: this fork's own history)
├── NOTICE.md                                                        (fork relationship: FFmpeg -> quatric/mobipeg -> this repo)
├── README.md                                                        (new, adapted from README_DRAFT.md)
├── libavcodec/, libavformat/, fftools/, x264-src/, ... (existing upstream layout, untouched, NOT moved under vendor/)
├── mobipeg3ds/           (new Python package: job model, presets, ffmpeg-arg backend, CLI)
├── tests/                (new: comparison tests against encode.py's actual behavior)
├── docs/
│   ├── research/         (condensed research reports: Step A/B/C summaries, QP-calibration
│   │                      findings, SIMD validation notes -- reports, not raw dumps)
│   └── technical/        (format notes, quantizer/QYX explanation, verification pipeline)
└── .github/workflows/    (build/test CI, not yet added)
```

GUI source (`gui/`) is intentionally not created this phase — GUI implementation is
explicitly deferred to a later phase, per instruction.

### Whether to vendor FFmpeg/x264 source directly or track upstream via patches

**Resolved during the first implementation phase:** keep the existing upstream
FFmpeg repository layout as-is at the repo root (the same layout
`C:\dev\mobipeg` already has) — do not move it under `vendor/` or otherwise
restructure it. `mobipeg-3ds-public` was created as a local clone of
`C:\dev\mobipeg` (full git history preserved, `upstream` remote pointing at that
local path), so the FFmpeg/x264 source is tracked as ordinary first-class repo
content, not a vendored subtree. The new product code
(`mobipeg3ds/` Python package, tests, docs) sits alongside it, not wrapping it.

## Branches (do not create yet)

- `main` — tested public states only.
- `develop` — active 3DS-only work.
- `archive/pre-3ds-only` — preserved historical reference (this investigation's
  current state, so the multi-week research trail isn't lost even though it won't
  ship in `main`).

## What to include

- The accepted encoder changes only — Candidate B plus whatever the quality
  localization work (Phase 4/5) eventually validates. Not every experimental patch
  tried along the way.
- The new 3DS-only GUI and CLI, once built.
- Verification utilities (the identity-gate / decode-verification harness pattern
  this session used repeatedly — e.g. `validate_block_rate.c`-style tools —
  adapted into a proper, documented test suite rather than ad-hoc harness files).
- Versioned quality presets, once the DZ visual verdict and any further
  localization work actually settles on real values (explicitly not finalized yet
  per the quality roadmap).
- Tests, build configuration, documentation, required license/attribution files.

## What to exclude

- Rejected experiments: the blur experiment, independent chroma QYX (impossible by
  construction), any abandoned cachefix-style work.
- Temporary worktrees, build directories, and `prefix-*` install trees — all
  reproducible from source; ~6GB of exactly this category was already cleaned from
  the research workspace this session (documented precedent for what "safe to
  exclude" looks like in this project).
- Raw coefficient dumps (this session's Step A/B/C `.txt`/`.csv` captures run into
  tens of megabytes per run) and large logs — keep the *reports*
  (`STEP_B_ZEROING_COUNTERFACTUAL.md`, this planning set) under `docs/research/`,
  not the raw data behind them.
- MOFLEX test outputs generated during development.
- Copyrighted test media — `tests/official/official_1400k.moflex` (the real
  Nintendo-produced reference sample) must never go into a public repository.
- Azahar emulator state, personal file paths, credentials, internal context bundles,
  dead experimental binaries (`.exe` harnesses like `step_b_analyze.exe`,
  `validate_block_rate.exe` — keep the `.c` source, not the built binaries).

## Immediate next action, not yet taken

None. This phase is audit and planning only, per explicit instruction. The first
concrete implementation step (once approved) is described in the main response as
"Recommended first phase."
