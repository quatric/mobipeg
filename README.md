# FFmpeg README

FFmpeg is a collection of libraries and tools to process multimedia content
such as audio, video, subtitles and related metadata.

## Libraries

* `libavcodec` provides implementation of a wider range of codecs.
* `libavformat` implements streaming protocols, container formats and basic I/O access.
* `libavutil` includes hashers, decompressors and miscellaneous utility functions.
* `libavfilter` provides means to alter decoded audio and video through a directed graph of connected filters.
* `libavdevice` provides an abstraction to access capture and playback devices.
* `libswresample` implements audio mixing and resampling routines.
* `libswscale` implements color conversion and scaling routines.

| Format | Container | Platform | Encode | Decode |
|--------|-----------|----------|--------|--------|
| MOFLEX 2D | `.moflex` | Nintendo 3DS | ✅ | ✅ |
| MOFLEX 3D | `.moflex` | Nintendo 3DS | ✅ | ✅ |
| MODS | `.mods` | Nintendo DS | ✅ | ✅ |
| MO | `.mo` | Nintendo Wii | ✅ | ✅ |
| VX | `.vx` | Nintendo DS | ✅ | ✅ |
| THP | `.thp` | GameCube / Wii | ✅ | ✅ |
| RVID | `.rvid` | RocketVideo (DS) | ✅ | ✅ |
| HVQM4 | `.h4m` | GameCube / Wii (Hudson Soft) | ✅ | ✅ |
| TiVo TyStream | `.ty` / `.ty+` / `.tmf` | TiVo (Series 1–3) | ✅ | ✅ |

Decode-only inputs can be transcoded into any of the encodable formats
above, or previewed with `encode.py decode <file>`. Series-3 TiVo
TyStreams (and MFS VideoClip resources) are handled through the bundled
[`s3tots`](tools/s3tots) tool, which losslessly rewraps them to MPEG-2 TS
before FFmpeg reads them.

TiVo `.ty` encoding (the `ty` muxer, `mpeg2video` + `mp2`/`ac3`) is ported
from the ["tyffmpeg"](https://repo.mariocube.com/TiVo%20Restore%20Images/_Tools/tyffmpeg.zip)
encoding library and targets the common Series 2 (Stand-Alone) container
layout; it requires `--enable-gpl` and does not write `.ty+`'s trailing XML
metadata block or Series 1/3-specific PES framing.

HVQM4 encoding (the `hvqm4` codec + muxer) is a from-scratch intra-only
implementation: per-block DC prediction, greedy nest-basis matching
pursuit for the AC/texture residual (in place of the original's VQ
codebook), and literal-block escape, built directly against the bundled
decoder rather than any original Hudson Soft encoder. There are no P/B
(motion-compensated) frames yet — every output frame is an I-frame, so
file size scales accordingly for longer clips.

### Splitting stereoscopic MOFLEX video

The MOFLEX demuxer exports the `VideoWithLayout` descriptor as standard
stereoscopic side data. Nintendo 3DS layout 0/1 video stores the left and right
eyes as alternating decoded frames, so both eyes must be decoded before they
are separated. This example duplicates the audio into two ordinary MP4 files:

```sh
ffmpeg -i input.moflex \
  -filter_complex "[0:v]split=2[vl][vr];[vl]stereo3d=al:ml,sidedata=delete:type=STEREO3D[left];[vr]stereo3d=al:mr,sidedata=delete:type=STEREO3D[right]" \
  -map "[left]"  -map 0:a? -c:v h264_videotoolbox -c:a aac left.mp4 \
  -map "[right]" -map 0:a? -c:v h264_videotoolbox -c:a aac right.mp4
```

For layout 1 (right-first), exchange `al` for `ar`. Layouts 2/3 and 4/5 are
top/bottom and side-by-side respectively and can be separated with the
corresponding `stereo3d` input mode.

## Tools

* [ffmpeg](https://ffmpeg.org/ffmpeg.html) is a command line toolbox to
  manipulate, convert and stream multimedia content.
* [ffplay](https://ffmpeg.org/ffplay.html) is a minimalistic multimedia player.
* [ffprobe](https://ffmpeg.org/ffprobe.html) is a simple analysis tool to inspect
  multimedia content.
* Additional small tools such as `aviocat`, `ismindex` and `qt-faststart`.

| Audio Codec | MOFLEX (3DS) | MODS (DS) | MO (Wii) |
|-------------|:------------:|:---------:|:--------:|
| ADPCM | ✅ | ✅ | ✅ |
| FastAudio | ✅ | ✅ | ✅ |
| PCM | ✅ | ✅ | ✅ |
| Vorbis | ➖ | ➖ | ✅ |
| Codebook (SX) | ➖ | ✅ | ➖ |

## Documentation

For full technical documentation of the MobiClip formats (video/audio codecs,
container layouts, and platform requirements across the DS/3DS/Wii), see
[The Mobiclip Formats](https://gist.github.com/quatric/3e2f0caa5c22a9a8b24d9cf1ffdfe860).

The offline documentation is available in the **doc/** directory.

The online documentation is available in the main [website](https://ffmpeg.org)
and in the [wiki](https://trac.ffmpeg.org).

### Examples

Coding examples are available in the **doc/examples** directory.

## License

FFmpeg codebase is mainly LGPL-licensed with optional components licensed under
GPL. Please refer to the LICENSE file for detailed information.

Copyright (c) 2026 quatric
