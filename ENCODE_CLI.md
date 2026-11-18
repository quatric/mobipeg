# encode.py CLI reference

`encode.py` is the command-line front end for every format in the table in
[README.md](README.md) — the GUI (`encode_gui.py`) is a wrapper around the
same script. This is a reference for the flags; see README for format-specific
notes (container quirks, what each codec actually does).

## Modes

```sh
python3 encode.py <fmt> <audio> <input_file> [input2] [options]   # encode
python3 encode.py decode <input_file> [options]                   # decode
python3 encode.py play <input_file> [options]                     # play, no file written
```

`fmt` selects the mode. Everything below `--` in the tables further down
applies to whichever mode is active; options that only make sense for one
format or mode say so.

In `decode` and `play` mode the positional that's normally the audio codec
becomes the input file instead (`encode.py decode movie.mo`, not
`encode.py decode none movie.mo`) — argparse reuses the same slot rather than
shifting every position over.

All three modes accept `--ffmpeg-args` and the stereoscopic `--stereo`/`--eyes`
pair; MobiClip rate-control flags, `--scale`, `--keyframes`, etc. only apply
to `encode`.

## Formats (`fmt`)

| `fmt` | Default scale | Container/codec |
|---|---|---|
| `mo` | 624x352 | Wii Mobiclip, `mobiclip_mo` |
| `moflex` | 400x240 | 3DS Mobiclip, `moflex` |
| `moflex3d` | 400x240 | 3DS Mobiclip, stereoscopic (two inputs) |
| `mods` | 256x192 | DS Mobiclip |
| `vx` | 256x192 | ActImagine VX (DS) |
| `fastvideo` | 256x192 | FastVideoDS (`.fv`) |
| `flipnote` | 256x192 | DSi Flipnote Studio (`.ppm`), 1-bit dithered, RSA-signed |
| `flipnote3d` | 320x240 | 3DS Flipnote Studio 3D (`.kwz`), six colours on three layers, dithered; signed when a key is given |
| `rvid` | 256x192 | RocketVideo (DS), native encoder |
| `dpg` | 256x192 | MoonShell DPG (DS), MPEG-1 + MP2 |
| `thp` | source | GameCube/Wii THP, Motion JPEG + `adpcm_thp` |
| `hvqm4` | 320x240 | Hudson Soft HVQM4 (GameCube/Wii), video only |
| `ty` | source | TiVo TyStream, MPEG-2 (`--enable-gpl` build) |
| `gba_ads` | 240x160 | GBA Video, LZMA/Dragon Ball GT lineage |
| `gba_hydrogen` | 240x160 | GBA Video, Inflate/Hydrogen lineage |
| `wii_photo` | 640x480 | Wii Photo Channel AVI, Motion JPEG |
| `nintendo_channel` | 378x284 | Wii Nintendo Channel, `libx264` (needs `--enable-libx264`) |
| `3ds_camera` | 480x240 | 3DS Camera AVI, 2D Motion JPEG |
| `3ds_camera3d` | 480x240 | 3DS Camera AVI, native stereo (two inputs) |
| `topos`, `topos_mpeg2` | 400x240 | Topos 3DS homebrew player `.mkv`: H.264 High@3.1 (New 3DS) or MPEG-2 Main 2 Mbit/s (Old 3DS) |
| `mivf` | 400x240 | MIVF 3DS homebrew player `.mivf`: M2Y1 video (`--m2y2` for range-coded M2Y2), seek index |
| `topos3d`, `topos3d_mpeg2` | 400x240 per eye | The same as 800x240 side-by-side 3D (two inputs, or one packed source) |

Audio-only outputs (no `input2`, `--scale`/`--keyframes`/etc. don't apply):

| `fmt` | Audio |
|---|---|
| `dsp` | GameCube/Wii/3DS DSP-ADPCM |
| `brstm`, `bfstm`, `bcstm` | Wii/Wii U/3DS streamed ADPCM or PCM |
| `bns` | Wii banner sound |
| `ast` | GameCube/Wii AFC ADPCM or PCM |
| `btsnd` | Wii U boot sound (fixed 48 kHz stereo PCM) |
| `wii_photo_m4a` | Wii Photo Channel 1.1 AAC-LC |
| `3ds_sound` | Nintendo 3DS Sound AAC-LC |

`decode` accepts any of the above, plus decode-only inputs the table in
README lists as decode-only (the FVMV/Caimans/VX++ GBA Video cartridge
families, First Hunt `.fv`, MOC2/MOC3 `.mo`, TiVo `.ty+`/MFS VideoClip
resources) — nothing
special to pass, just point it at the file.

## Audio codec (`audio` positional, encode mode only)

| `fmt` | Accepted `audio` values |
|---|---|
| `mo` | `adpcm`, `fastaudio`, `pcm`, `vorbis`, `none` |
| `moflex`, `moflex3d` | `adpcm`, `fastaudio`, `pcm`, `none` |
| `mods` | `adpcm`, `fastaudio`, `pcm`, `codebook`, `none` |
| `vx` | `codebook`, `none` |
| `ty` | `mp2`, `ac3`, `none` |
| `dpg` | `mp2`, `none` |
| `wii_photo` | `pcm`, `none` |
| `wii_photo_m4a`, `3ds_sound` | `aac` |
| `nintendo_channel` | `aac`, `none` |
| `topos`, `topos3d` | `aac` (default), `mp2`, `none` |
| `topos_mpeg2`, `topos3d_mpeg2` | `mp2` (default), `aac`, `none` |
| `mivf` | `ia4m` (mono ADPCM, default; `adpcm` means the same), `pc16` (16-bit PCM, stereo), `none` |
| `3ds_camera`, `3ds_camera3d` | `adpcm`, `none` |
| `thp` | `adpcm`, `none` |
| `rvid` | `pcm`, `none` |
| `fastvideo` | `adpcm`, `none` |
| `flipnote` | `pcm` (8192 Hz mono), `none` |
| `flipnote3d` | `pcm` (16364 Hz mono, stored as the note's ADPCM BGM), `none` |
| `hvqm4`, `gba_ads`, `gba_hydrogen` | `none` (no audio support yet) |
| `dsp`, `bns` | `adpcm` (the container's only option — no `none`) |
| `brstm`, `bfstm`, `bcstm`, `ast` | `adpcm`, `pcm` |
| `btsnd` | `pcm` |

## Common options

| Flag | Applies to | Meaning |
|---|---|---|
| `--scale WxH` | encode | Override the format's default scale, e.g. `--scale 320x240` |
| `--fps FPS` | encode | Force a frame rate — decimal (`15`) or exact fraction (`60000/1001`). Empty keeps the source; `.mo` defaults to `30000/1001`. Must usually match the clip you're replacing. |
| `--keyframes N` | encode | Evenly-spaced keyframes across the clip. `0` (default) = auto, as few as practical within the encoder's ~90-frame gap limit. Scene cuts still insert extra keyframes on top. |
| `--audio-rate HZ` | encode | Resample audio. TiVo defaults to 48000; other formats keep the source rate. |
| `--roundtrip` | encode | Decode the just-written output and diff it, to catch encoder round-trip bugs. |
| `--fast-audio` | encode | Skip the `vx_audio` LTP lag search (~90x faster, ~2 dB worse). Only affects `vx` and `mods` codebook (SX) audio. For `mods` the muxer runs the codebook encoder itself, in the same pass as the video, starting an intra audio frame at every video keyframe (`-sx_ltp 0` is what this flag passes). |
| `--outdir DIR` | encode | Output directory (created if missing). **Set this or `OUTDIR`** — the built-in default is a path on the original dev machine and won't exist elsewhere. |
| `-o`, `--output FILE` | decode | Output filename. Default: input's basename + `.mp4` (stereo input appends `_left`/`_right`). |
| `--ffmpeg-args '...'` | all | Extra args appended verbatim just before the output file, shell-word-parsed. Since ffmpeg lets the last occurrence of a flag win, this overrides the format preset: `--ffmpeg-args '-t 5 -af volume=0.5'`. The `--roundtrip` validation pass doesn't see it. |

## Stereoscopic (`decode`/`play`)

| Flag | Meaning |
|---|---|
| `--stereo {auto,none,frameseq,frameseq-r,tb,tb-r,sbs,sbs-r}` | Force the stereo layout instead of reading it from the file — needed when a 3D source carries no layout descriptor. `-r` variants store the right eye first. `none` treats input as 2D. |
| `--eyes {both,left,right,packed}` | decode: `both` (default) writes one file per eye; `left`/`right` writes just that eye; `packed` keeps the interleaved stream untouched. play: `both` shows eyes side by side; `left`/`right` plays one eye full-window; `packed` plays as stored. |

`moflex3d`, `topos3d`, `topos3d_mpeg2` and `3ds_camera3d` take a second input
file (right eye) directly on the command line instead of using `--eyes`; the
first three also accept one packed source, using `--stereo` or its detected
layout:

```sh
python3 encode.py moflex3d adpcm left.mp4 right.mp4
python3 encode.py 3ds_camera3d adpcm left.mp4 right.mp4
```

`--layout N` (default `4`) sets the MOFLEX 3D layout for `moflex3d` output.

A 2D `moflex` carries the plain video descriptor, as retail 2D movies do; a 3D
one carries the descriptor with the layout byte. Should a player want the
layout descriptor on a 2D movie too (layout 6, "simple 2D"), which is what
this muxer wrote before, add `--ffmpeg-args '-mo_vdesc 3'`.

## MobiClip rate control (`mo`, `moflex`, `moflex3d`, `mods`)

Requires a `--enable-libx264` build against the
[quatric/x264](https://github.com/quatric/x264) fork — see README's
*Building* section.

| Flag | Meaning |
|---|---|
| `--qp N` (alias `--quantizer`) | Constant QP, format range 12-63. `0` (default) = format default (22 for CQP). Also settable via `QUANT`/`QP` env vars. |
| `--hq` (alias `--highest-quality`) | QP 12 (the floor), subme 9, no skip threshold. |
| `--bitrate RATE` | Target average bitrate (e.g. `700k`); overrides `--qp`. Retail `Bitrate`. |
| `--multipass {1,2}` | Rate-control passes. `2` needs `--bitrate` and hits the target more accurately. Retail np1/npn, cbr1/cbrn. |
| `--passlog FILE` | Stats file for `--multipass 2` (default `<output>.pass`). |
| `--min-qp N` / `--max-qp N` | Clamp rate control to this QP range, 12-48. Retail `MinQuantizer`/`MaxQuantizer`. |
| `--i-boost N` | Percent extra bits for I-frames, 0-100 (retail `IBoostPercent`, default 40). Only meaningful with `--bitrate`. |
| `--i-threshold N` | Scene-cut sensitivity for inserting I-frames, 0-100 (retail `IThreshold`, default 90). `0` disables scene-cut keyframes. |
| `--buffer-size SIZE` | Rate-control buffer size (e.g. `400k`, retail `BufferSize`). Bounds local bitrate drift. |
| `--me {dia,hex,umh,esa}` | Motion search method (retail `MeMethod`). Default is the preset's (`hex`). |
| `--8x8dct {0,1}` | Allow the 8x8 luma transform (default on). `0` forces 4x4-only. |
| `--mobi-subme N` | Subpel/RD refinement, 2-11 (`0` = preset default, normally 7). 9 is the practical ceiling; 10-11 need trellis, which MobiClip has no representation for. Below 6 disables RD mode decision. |
| `--mobi-skip N` | Macroblock skip error threshold (default 512). `0` keeps every residual (marginally better at low QP); higher freezes near-static blocks to stop dither flicker. |
| `--mobi-intra-only` | Every frame is an I-frame. |
| `--mobi-qyx N` | Coarsen the quantizer by N whole qy tiers (QP + 6*N, 0-8). Legacy — plain `--qp` now covers the format's full range, leave at 0. |
| `--jobs N` | `mo`, `moflex`, `mods`, `mivf`, and the four `topos` formats: encode the video as chunks, N at a time, each decoding its own stretch of the source, then join them by stream copy. `0` (default) = one per two CPU cores, `1` = encode in one piece. MobiClip chunks are whole keyframe intervals (about 3-6 s, 10-30 s for `.mods`, which gains a keyframe per chunk), so the output matches a one-piece encode frame for frame, within ~0.3% in size. Topos MPEG-2 chunks are whole 30-frame GOPs; Topos H.264 chunks run 5-60 s, about one per job, since x264 already keeps several cores busy on its own (a 3-minute clip went from 5.2 s to 3.8 s in 2D, 11.1 s to 8.1 s in 3D, on 4 performance + 6 efficiency cores). Topos audio is encoded at the same time as the video chunks. MIVF chunks are whole 240-frame keyframe intervals, one per CPU core by default (its encoder is single-threaded), and give a byte-identical file. Clips under twice the chunk length, `--multipass 2`, `--ffmpeg-args`, MOFLEX 3D and FastAudio `.mods` always encode in one piece. |

## RVID-only (`rvid`)

| Flag | Meaning |
|---|---|
| `--rvid-mode {rgb555,rgb565,256}` | Pixel mode: `rgb555` (default, unlimited color), `rgb565` (max color), `256` (8bpp palette). |
| `--no-compress` | Store raw frames instead of Nintendo LZ10. |
| `--rvid-interlaced` | One field per frame. |
| `--rvid-no-dither` | Disable the checkerboard ordered dither used when reducing to 16bpp. |

Left alone, the encoder makes the choices Vid2RVID makes, which follow what
RocketVideoPlayer can keep up with: a 16bpp 256x192 video is compressed up to
12 fps and stored as fields above 36 fps, an 8bpp one is compressed up to
48 fps and stored as fields above 72 fps (smaller pictures get more room).
With the same frames, sound and settings the file is the one Vid2RVID writes,
byte for byte. Sound is signed PCM at 32000 Hz at most, the player's limit;
`encode.py` resamples to 32000 Hz unless `--audio-rate` asks for less.

The ffmpeg `rvid` encoder also takes `-dual_screen 1` for a 256x384 picture
holding the top screen above the bottom one, and `-lazy 1` for a slightly
tighter LZ10 parse than Vid2RVID's. The demuxer reads versions 1 to 5 of the
format, dual screen videos included.

## Flipnote (`flipnote`, `flipnote3d`)

| Flag | Meaning |
|---|---|
| `--flipnote-author NAME` | Author name stored in the note, 10 characters at most (default `mobipeg`). Both formats. |
| `--flipnote-dither {none,2x2,4x4,8x8}` | `flipnote3d`: ordered dither matrix used to reduce the picture to the six Flipnote colours (default `4x4`). `none` takes the nearest colour, `2x2` gives the plainest patterns, `8x8` the smoothest tones. |
| `--flipnote-paper {white,black,red,yellow,green,blue}` | `flipnote3d`: paper colour (default `white`). Pixels of the paper colour are left blank on every layer. |
| `--flipnote-key FILE` | `flipnote3d`: RSA private key to sign the note with (PKCS#1 or PKCS#8, DER or PEM). |
| `--flipnote-loop` | `flipnote3d`: set the note's loop flag. |

`flipnote3d` writes at one of the application's playback speeds (0.2, 0.5, 1,
2, 4, 6, 8, 12, 20, 24 or 30 fps); `--fps` picks the nearest one and the
default is 12. A note holds 999 frames at most, and the BGM is cut to the
length of the animation.

Layers are compressed the way Flipnote Studio 3D compresses them: every 8x8
tile in its cheapest form, unchanged tiles skipped in runs. For the same
layer contents that gives the application's own bytes; a comment and the
first frame of a sample note saved by the application come back identical
when decoded and written again. The muxer always uses one colour layout
(layer A white and black, B blue and red, C yellow and green), so a note
drawn with another layout decodes to the same pictures but not the same
layer data. Flipnote Studio 3D only opens notes that carry its RSA-2048 signature.
The signing key is part of the application and is not included here; without
`--flipnote-key` the note is written unsigned, which mobipeg and other
readers accept and the console does not. With a key, the muxer says whether
it is the application's (by the checksum the format documentation
publishes). `flipnote` (`.ppm`) notes are always signed.

## FastVideoDS (`fastvideo`)

The video and the file are what Gericom's FastVideoDSEncoder makes of the same
frames and sound when it runs as one job (`-j 1`), byte for byte: key frames
and predicted frames with its motion search and quantizer, a key frame after
250 predicted ones (`-g` on the ffmpeg `fastvideo` encoder) or when more than
six blocks in ten would be intra, its ADPCM, and the table of key frames
FastVideoDSPlayer needs to open the file.

The player shows pictures 256 wide and up to 192 high (a multiple of 8;
`--scale 256x144` for 16:9), and plays stereo sound at 47605 Hz, which is what
`encode.py` resamples to. Without sound, or where it ends before the video,
the file carries coded silence as the reference encoder's does. The encoder
warns about a frame over the 64 KiB the player has room for; only pictures
close to noise get there.

## Environment variables

| Variable | Same as | Notes |
|---|---|---|
| `OUTDIR` | `--outdir` | |
| `QUANT`, `QP` | `--qp` | `QUANT` wins if both are set |
| `MOBI_QYX` | `--mobi-qyx` | |
| `MOBI_SUBME` | `--mobi-subme` | |
| `MOBI_SKIP` | `--mobi-skip` | |
| `FFMPEG`, `FFPROBE`, `FFPLAY` | — | Override which binaries encode.py shells out to, e.g. to point at a different build. |

## Examples

```sh
# Wii .mo, defaults (ADPCM audio, 624x352, auto keyframes)
python3 encode.py mo adpcm input.mp4 --outdir out/

# 3DS .moflex, near-lossless
python3 encode.py moflex adpcm input.mp4 --hq --outdir out/

# DS .mods with codebook (SX) audio, capped keyframe interval
python3 encode.py mods codebook input.mp4 --keyframes 90 --outdir out/

# Constant-bitrate .mo, two-pass
python3 encode.py mo adpcm input.mp4 --bitrate 700k --multipass 2 --outdir out/

# Decode any supported file to .mp4
python3 encode.py decode input.mo -o output.mp4

# Split a stereoscopic .moflex into left/right .mp4
python3 encode.py decode input.moflex --eyes both

# Preview without writing anything
python3 encode.py play input.mods
python3 encode.py play input.moflex --eyes left

# Pass extra ffmpeg options straight through (5-second clip, volume halved)
python3 encode.py mo adpcm input.mp4 --ffmpeg-args '-t 5 -af volume=0.5' --outdir out/
```
