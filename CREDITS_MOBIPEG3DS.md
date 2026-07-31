# Mobipeg 3DS Credits

This file credits work specific to the Mobipeg 3DS fork. It does not replace
`CREDITS_MOBICLIP.md`, which documents the MobiClip codec research this
entire project depends on (PlayMobic, MobiclipDecoder/Gericom, Gericom's
x264 fork, WiiLink24's FFmpeg fork) and must be preserved verbatim.

## This fork's scope

- Encoder-quality investigation (SIMD vectorization, QP/QYX calibration,
  coefficient-level rate-distortion studies) performed against the patched
  x264 encoder, producing the "Candidate B" accepted baseline referenced
  throughout this repository's presets and documentation. See
  `docs/research/` for the full investigation record.
- The `mobipeg3ds` Python package (job model, ffmpeg-argument backend, CLI,
  GUI) — a new, from-scratch implementation, informed by but not derived
  from the reference `encode.py`/`encode_gui.py` (see
  `docs/research/CURRENT_GUI_STATUS.md` for the precedent it follows and
  deliberately does not copy).
- The 3DS-only product audit and repository planning documents under
  `docs/research/`.
