# Mobiclip Support Credits

The implementation of Mobiclip support in this software was made possible thanks to the research, documentation, and source code from the following projects and their respective authors:

* [PlayMobic](https://code.pleonex.dev/pleonex/PlayMobic)
* [MobiclipDecoder](https://github.com/Gericom/MobiclipDecoder)
* [Gericom's x264 fork](https://github.com/Gericom/x264)
* [WiiLink24 FFmpeg fork](https://github.com/WiiLink24/FFmpeg)
* [actimagine](https://github.com/CharlesVanEeckhout/actimagine/)
* [flipnote.js](https://github.com/jaames/flipnote.js/)
* [RocketVideoPlayer](https://github.com/RocketRobz/RocketVideoPlayer)
* [Vid2RVID](https://github.com/RocketRobz/Vid2RVID) - the RVID encoder's choices and LZ parse follow it, so that the same input gives the same file
* [Oldhimaster1's Mobipeg-MOFLEX](https://github.com/Oldhimaster1/Mobipeg-MOFLEX) - **Oldhimaster1** (GBAtemp) made modifications to Mobipeg; the quality profiles, encoding speed choices and several encoder options here follow that fork
* [MoflexTool](https://github.com/koori0831/MoflexTool) - the FastAudio encoder's analysis and pulse search follow its encoder; its decoders served as a reference for MOFLEX audio and video

## MODS encoder reference

* **Helwettpackardenterprise** (GBAtemp) - reverse engineered much of the Mobiclip format, and wrote `MODS_Encoder_v43_2`, a verified-accurate C
  translation of the retail Mobiclip VfW encoder (`mods.dll`), verified against
  the original under Unicorn. It is the source for the exact transform and
  quantization now used by the Mobiclip encoder in
  [quatric/x264](https://github.com/quatric/x264): the forward integer DCTs,
  the H.264 MF/dequant scale matrices, the single `(1 << shift) / 3` rounding
  bias, and the `shift = 15 + q/6` (4x4) / `16 + q/6` (8x8) quantizer scaling.

## 3DS CIA Video Injection

* [VidInjector9000](https://github.com/FoofooTheGuy/VidInjector9000) by **FoofooTheGuy** - `VidInjector9002-CLI` is vendored and integrated to package converted Moflex and video streams directly into installable Nintendo 3DS `.cia` video titles.

## HVQM4 (Hudson Soft `.h4m`)

* [Tilka/hvqm4](https://github.com/Tilka/hvqm4) - bit-accurate HVQM4 1.3/1.5 decoder by Tillmann Karras, based on the audio decoder by flacs/hcs. The core `libavcodec/h4m_audio_decode.c` is vendored from this project.
* [Tilka/ffmpeg_hvqm4](https://github.com/Tilka/ffmpeg_hvqm4) - FFmpeg demuxer/decoder wrappers this port is derived from.
* [Tilka/hvqm2](https://github.com/Tilka/hvqm2) - HVQM2 decoder (reference; HVQM2 video not yet wired into mobipeg).

## TiVo TyStream (`.ty` / `.ty+` / `.tmf`)

* FFmpeg's built-in `ty` demuxer (Series 1/2).
* `s3tots` by B.C. - Series-3 TyStream/TMF → MPEG-2 TS converter, the logic for which has been ported into this codebase.
* [Dan203/s3totsGUI](https://github.com/Dan203/s3totsGUI) - GUI front-end for s3tots (reference).
* [elitak/mfs-utils](https://github.com/elitak/mfs-utils) - `vsplit` and TiVo MFS tooling (reference).
* `tytompg` / `tyffmpeg` (`hdemux`) by B.C. - TyStream → MPEG program-stream converters and the FFmpeg `ty` demuxer patch this work builds on (reference).

## FastVideoDS (`.fv`)

* **Gericom** - mobipeg's scalar ARM64 FastVideoDS encoder
  (`libavcodec/fastvideo.c` and the FastVideoDS entry in `encode.py`) is a
  port of Gericom's AVX2-gated FastVideoDS reference encoder, the same
  source of ActImagine-format expertise credited above for MobiclipDecoder
  and the Mobiclip x264 fork. The ADPCM coder (`libavcodec/adpcm_fvds.h`) and
  the file layout (`libavformat/fvenc.c`) follow
  [FastVideoDSEncoder](https://github.com/Gericom/FastVideoDSEncoder) and
  [FastVideoDSPlayer](https://github.com/Gericom/FastVideoDSPlayer) as well.

## Flipnote Studio (`.ppm`) and Flipnote Studio 3D (`.kwz`)

* [Flipnote Collective](https://github.com/Flipnote-Collective) - the
  [PPM](https://github.com/Flipnote-Collective/flipnote-studio-docs/wiki/PPM-format)
  and [KWZ](https://github.com/Flipnote-Collective/flipnote-studio-3d-docs/wiki/KWZ-Format)
  format documentation the demuxers and muxers are written from.
* [flipnote.js](https://github.com/jaames/flipnote.js/) by **James Daniel** -
  the reference the decoders are ported from, and the source of the sample
  notes the `.kwz` muxer's layer compression was checked against.

## mobipeg

* **quatric** (<quatricsoftware@gmail.com>) - the mobipeg fork itself: the
  Mobiclip, ActImagine, FastVideoDS, RVID, GBA Video, HVQM4, Flipnote and
  Nintendo audio encoders, decoders, muxers and demuxers, `encode.py` and the
  GUI.
