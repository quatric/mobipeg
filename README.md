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
| HVQM4 | `.h4m` | GameCube / Wii (Hudson Soft) | ➖ | ✅ |
| TiVo TyStream | `.ty` / `.ty+` / `.tmf` | TiVo (Series 1–3) | ➖ | ✅ |

Decode-only inputs (HVQM4, TiVo) can be transcoded into any of the encodable
formats above, or previewed with `encode.py decode <file>`. Series-3 TiVo
TyStreams (and MFS VideoClip resources) are handled through the bundled
[`s3tots`](tools/s3tots) tool, which losslessly rewraps them to MPEG-2 TS
before FFmpeg reads them.

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

The offline documentation is available in the **doc/** directory.

The online documentation is available in the main [website](https://ffmpeg.org)
and in the [wiki](https://trac.ffmpeg.org).

### Examples

Coding examples are available in the **doc/examples** directory.

## License

FFmpeg codebase is mainly LGPL-licensed with optional components licensed under
GPL. Please refer to the LICENSE file for detailed information.

Copyright (c) 2026 quatric
