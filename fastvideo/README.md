# FastVideoDS (.fv) encoder for mobipeg

This is [Gericom's FastVideoDS encoder](https://github.com/Gericom/FastVideoDSEncoder)
codec, adapted to build and run on **arm64** and to fit mobipeg's ffmpeg-driven
pipeline. Output plays back with the
[FastVideoDS Player](https://github.com/Gericom/FastVideoDSPlayer).

## Why this fork exists

The upstream encoder is hard-gated on x86 **AVX2** (`if (!Avx2.IsSupported) return;`)
and its hot loops are written with AVX2 intrinsics. Apple-silicon Macs are arm64
and Rosetta 2 has no AVX2, so the reference build cannot run here at all.

Only two files used intrinsics; **everything correctness-critical (VLC tables,
DCT, quantisation, motion estimation, the bitstream writer) is untouched scalar
code**. The changes:

- `Gericom.FastVideoDS/Utils/FrameUtil.cs` — rewritten scalar. Each AVX2
  fast-path already had a bit-identical scalar fallback (or a commented scalar
  reference) beside it; this keeps those and drops the SIMD.
- `Gericom.FastVideoDS/FastVideoDSEncoder.cs` — the three AVX2 helpers
  (`SadIP`, `GxAverage`, `AllZero`) replaced with scalar equivalents; AVX2
  `using`s removed.
- `Gericom.FastVideoDS/Frames/Rgb555Frame.cs` — dropped the `System.Drawing`
  dependency (unpacks BGRA bytes directly instead of `Color.FromArgb`).
- `Program.cs` (new) — replaces the FFmpeg.AutoGen native decoder + the
  threaded `FvEncoder` driver with a thin raw-I/O CLI. mobipeg's bundled ffmpeg
  decodes the source to raw BGRA frames + interleaved s16 stereo PCM; this feeds
  them to the codec and writes the `.fv` container byte-for-byte like the
  reference muxer.

`Adpcm.cs` and `SpanExtensions.cs` are copied unchanged from the upstream app
project.

## Usage

You normally don't call this directly — `encode.py fv …` (or the GUI's
"FastVideoDS .fv (NDS)" format) builds it on demand and drives it. Directly:

    fvenc --height H --fps-num N --fps-den D --frames F \
          [--audio-rate R --pcm audio.s16] [--q 30] [--gop 250] --raw video.bgra out.fv

- `video.bgra` — raw BGRA frames, 256×H (FastVideoDS is fixed 256 wide)
- `audio.s16`  — raw interleaved s16 **stereo** PCM at R Hz
- `--q`        — quantiser (higher = smaller / lower quality)
- `--gop`      — maximum keyframe interval

Build manually with: `dotnet build fvenc.csproj -c Release`

## Status / caveats

Container muxing is verified structurally (frame/audio chain lands exactly at
EOF; keyframe offsets match frame starts; audio-sample accounting is exact). The
codec bitstream itself is Gericom's proven code, unmodified. On-device playback
has **not** been verified against real DS hardware / the player from this
machine.
