# Mobiclip Support Credits

The implementation of Mobiclip support in this software was made possible thanks to the research, documentation, and source code from the following projects and their respective authors:

* [PlayMobic](https://code.pleonex.dev/pleonex/PlayMobic)
* [MobiclipDecoder](https://github.com/Gericom/MobiclipDecoder)
* [Gericom's x264 fork](https://github.com/Gericom/x264)
* [WiiLink24 FFmpeg fork](https://github.com/WiiLink24/FFmpeg)
* [actimagine](https://github.com/CharlesVanEeckhout/actimagine/)
* [flipnote.js](https://github.com/jaames/flipnote.js/)
* [RocketVideoPlayer](https://github.com/RocketRobz/RocketVideoPlayer)

## MODS encoder reference

* **Helwettpackardenterprise** — `MODS_Encoder_v43_2`, a Codex-assisted C
  translation of the retail Mobiclip VfW encoder (`mods.dll`), verified against
  the original under Unicorn. It is the source for the exact transform and
  quantization now used by the Mobiclip encoder in
  [quatric/x264](https://github.com/quatric/x264): the forward integer DCTs,
  the H.264 MF/dequant scale matrices, the single `(1 << shift) / 3` rounding
  bias, and the `shift = 15 + q/6` (4x4) / `16 + q/6` (8x8) quantizer scaling.

## HVQM4 (Hudson Soft `.h4m`)

* [Tilka/hvqm4](https://github.com/Tilka/hvqm4) — bit-accurate HVQM4 1.3/1.5 decoder by Tillmann Karras, based on the audio decoder by flacs/hcs. The core `libavcodec/h4m_audio_decode.c` is vendored from this project.
* [Tilka/ffmpeg_hvqm4](https://github.com/Tilka/ffmpeg_hvqm4) — FFmpeg demuxer/decoder wrappers this port is derived from.
* [Tilka/hvqm2](https://github.com/Tilka/hvqm2) — HVQM2 decoder (reference; HVQM2 video not yet wired into mobipeg).

## TiVo TyStream (`.ty` / `.ty+` / `.tmf`)

* FFmpeg's built-in `ty` demuxer (Series 1/2).
* `s3tots` by B.C. — Series-3 TyStream/TMF → MPEG-2 TS converter, the logic for which has been ported into this codebase.
* [Dan203/s3totsGUI](https://github.com/Dan203/s3totsGUI) — GUI front-end for s3tots (reference).
* [elitak/mfs-utils](https://github.com/elitak/mfs-utils) — `vsplit` and TiVo MFS tooling (reference).
* `tytompg` / `tyffmpeg` (`hdemux`) by B.C. — TyStream → MPEG program-stream converters and the FFmpeg `ty` demuxer patch this work builds on (reference).
