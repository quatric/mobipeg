# Staged Removal Plan

Not executed in this phase — planning only. Updated to reflect the corrected
finding that real Wii/DS legacy exists (`encode.py`/`encode_gui.py`'s format
selector), not the earlier wrong "nothing to remove" conclusion.

## What "removal" actually means here, given the corrected findings

Mobipeg 3DS is a *new* GUI/CLI implementation (per instruction: reference the
existing `encode.py`/`encode_gui.py`, don't inherit or edit them in place). So
"removing Wii/DS support" mostly means **not carrying the `mo`/`mods` paths forward
into the new source** — a design/scope decision in new code, not a deletion from
existing code. The genuine deletion risk is at the FFmpeg/x264 vendor-tree level
(VX/RVID/KWZ/PPMFLIP/THP, and anything else not confirmed necessary), where files
really do need to be traced before removal.

## Sequence

1. **Preserve a pre-3DS-only archival identity.** `archive/pre-3ds-only` branch (or
   equivalent snapshot) capturing the current investigation state — including this
   session's Step A/B/C reports, Candidate B, and the corrected audit documents —
   before any public-repo work begins. **Update:** the workspace itself
   (`C:\dev\MIVF\mobipeg-3ds-public`, a local clone of `C:\dev\mobipeg` with full
   history, `develop` branch checked out) was created in the first implementation
   phase. `archive/pre-3ds-only` as a *named branch within it* is still not created
   — `develop` currently holds all new work.
2. **Design the new GUI/CLI without Wii/DS/other-format controls**, per
   `GUI_AND_CLI_ARCHITECTURE.md`. Since this is new source, this step is "don't
   build it in," not "delete it" — genuinely lower risk than editing existing code.
   `moflex3d` is explicitly deferred (rejected with a clear message at the backend
   level), not silently dropped — decided, not left open, as of the first
   implementation phase.
3. **Narrow and document the CLI.** Unrecognized platform/format requests get the
   explicit message specified in the architecture plan, never silent ignoring.
4. **Verify the 3DS path builds and encodes correctly** — reuse this session's own
   identity-gate pattern (byte-for-byte SHA-256 match against Candidate B for the
   disabled/default path, full software decode, deterministic repeat) as the
   acceptance bar for the new CLI's `encode` command specifically.
5. **Only then, evaluate genuinely dead backend code for removal** — this is where
   real deletion risk lives. Specifically: trace whether VX, RVID, KWZ, PPMFLIP,
   and THP (`libavcodec/vx*.c`, `rvid.c`, `libavformat/kwzdec.c`, `ppmflipdec.c`,
   `rvid*.c`, `thpenc.c`) have any live call site anywhere (including in
   `encode.py`/`encode_gui.py`'s sibling scripts, CI, or docs) before concluding
   they're removable. This audit found no reference in the two files it read in
   full, but did not exhaustively search the whole tree — do not delete based on
   this alone. Small, individually reviewable commits, one format family at a time.
6. **Rebuild and test after every removal group** — not batched, so a bad removal
   is easy to isolate and revert.
7. **Minimize FFmpeg build configuration (`--disable-*` flags, decoder/muxer allow
   lists) only after source removal is stable** — reducing the build surface before
   confirming the source changes are correct just makes debugging harder if
   something breaks.
8. **Packaging and documentation last** — README, relabeling, release archive
   naming (per `RELABELING_CHECKLIST.md`) once the actual product behavior is
   settled, not before.

Never combine backend deletion, FFmpeg build-config minimization, and GUI
replacement into a single commit — matches the explicit instruction and keeps each
step's blast radius reviewable on its own.
