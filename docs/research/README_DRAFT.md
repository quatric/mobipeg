# Mobipeg 3DS

A dedicated desktop encoder for creating Nintendo 3DS-compatible
MOFLEX video files.

Mobipeg 3DS is an independent community fork of Mobipeg focused
exclusively on Nintendo 3DS MOFLEX creation. It is not affiliated
with or endorsed by Nintendo.

<!-- Sections below are an outline with placeholder notes, not final copy.
     Do not publish until the quality investigation's open questions
     (DZ visual verdict, Phase 4/5 localization) are resolved, since
     several sections depend on settled preset values and accepted
     screenshots. -->

## 1. Development status
Pre-alpha / active research. Link to the quality investigation's current
state honestly — Candidate B is the accepted control; further quality
localization work is in progress. Do not claim a finished preset lineup yet.

## 2. What Mobipeg 3DS does
One sentence: takes a common video file and produces a Nintendo
3DS-compatible `.moflex` (400×240, MobiClip video, 3DS-compatible audio).

## 3. Main features
Derived from the GUI plan's Source→Review flow — describe the workflow,
not internal encoder mechanics (QYX/RDO/tables belong in docs/technical/,
not here).

## 4. Screenshot or GUI preview
Placeholder — no GUI exists yet (see `CURRENT_GUI_STATUS.md`).

## 5. Installation
Placeholder pending actual packaging.

## 6. First encode
A short, real walkthrough once the CLI/GUI exist.

## 7. Quality presets
Explicitly do not finalize preset value tables until the quality candidate
is accepted. Placeholder table naming `Balanced / Anime High Quality /
Maximum Quality / Custom` with values marked "unresolved."

## 8. Supported input types
Whatever ffmpeg's demuxers reasonably support; keep broad here even though
output is narrow.

## 9. Output specification
400×240, MobiClip video, 3DS-compatible audio, `.moflex` container. State
plainly that `-b:v`-style exact target sizing is not available (fixed-QP
encoder) — an estimated range only.

## 10. Verification process
Mirror the GUI plan's three-tier separation: software verification / Azahar
testing / physical 3DS testing, with the same non-conflation warning.

## 11. Tested environments and hardware
Placeholder — be honest about what's actually been tested vs. assumed, same
principle the existing MIVF README already follows ("must not be generalized
to every Nintendo 3DS model or build").

## 12. Known limitations
- Fixed-QP only, no exact target-bitrate control.
- Quality presets not yet finalized (link to the open quality work).
- [others as they become known]

## 13. Building from source
Point at the vendored/pinned FFmpeg+x264 trees and the patch/build process,
once `PUBLIC_REPOSITORY_PLAN.md`'s vendoring-vs-patch-series question is
decided.

## 14. Project lineage
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
forward verbatim from upstream, not to be rewritten).

## 15. Licensing and third-party notices
LGPL v2.1+ (overall) / GPL v2+ (x264-linked build), per upstream FFmpeg's own
`LICENSE.md`. `COPYING.*` files ship unmodified. No personal relicense.
