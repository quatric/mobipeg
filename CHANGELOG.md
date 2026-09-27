# Changelog

## v2.3

### New format support
- Mux/demux stereo VX audio
- Full Mobiclip MO/MODS/MOFLEX regression suite with retail fixtures, stable remux, real frame rate, keyframe tables, and stream-copy support

### Fixes — Nintendo audio formats (DSP, BRSTM, BFSTM, BCSTM, BNS, AST)
- Correct BFSTM/BCSTM references and seek metadata; preserve BRSTM channel identity through coefficient references
- Fix Nintendo wave (BNS) probing, PCM layout, byte order, and range validation; correct wave ADPCM contexts and validate loop points
- Preserve exact DSP and BNS loop state; restore DSP/AST sample accounting and predictor history across seeks
- Validate DSP/AST channel headers, bound packet/interleave sizes, reject partial or nonseekable output
- Reject sample rates that overflow the BRSTM header; read DSP audio at valid rates outside the usual range

### Fixes — other codecs/containers
- RocketVideo: bound table parsing, validate streams, use video timing regardless of stream order, fix decoder validation/reset
- VX: bound GBA motion vectors, frame/audio sizes; reject LPC state overflow instead of wrapping
- F5VID: reproduce retail audio schedule exactly, accept fresh MPEG-4 encoder output, write planar channel audio/AUDD padding matching retail, fix padding/bounds
- THP: fix JPEG stripping and adpcm_thp chunked packet emission (`-shortest` now trims instead of dropping)
- TY: fix probe/PES overflows
- Mobiclip MO/MODS/MOFLEX: fix overflow/OOB reads, short reads, leaks, invalid stream handling; use 64-bit timestamps instead of a 1-bit wrap
- Remove unused mobiclipenc stub (moenc now provides mobiclip_mo)

### GUI / frontend
- Handle unwriteable output directories cleanly and filter legacy volume errors
- Fix frontend output paths, outdir resolution, console encoding, and fallback transcoding reliability
- Fix keyframe spacing, subtitle tagging, and option passthrough in frontends
- Probe decode input stereo layout off the Tk thread and quote paths correctly; fix encode_gui HQ visibility
- Format Python frontends with black

### Reliability / telemetry
- Filter charmap and legacy missing-binary errors from Sentry; fixed Unicode output handling
- Stop enabling sentry-native debug logging in fftools

### CI
- Retry dav1d download on transient network failures
- Fail fast when cross-toolchain apt install exhausts retries
