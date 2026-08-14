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
README lists as decode-only (Flipnote `.ppm`/`.kwz`, the FVMV/Caimans/VX++ GBA
Video cartridge families, TiVo `.ty+`/MFS VideoClip resources) — nothing
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
| `3ds_camera`, `3ds_camera3d` | `adpcm`, `none` |
| `thp` | `adpcm`, `none` |
| `rvid` | `pcm`, `none` |
| `fastvideo` | `adpcm`, `none` |
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
| `--fast-audio` | encode | Skip the `vx_audio` LTP lag search (~90x faster, ~2 dB worse). Only affects `vx` and `mods` codebook (SX) audio. |
| `--outdir DIR` | encode | Output directory (created if missing). **Set this or `OUTDIR`** — the built-in default is a path on the original dev machine and won't exist elsewhere. |
| `-o`, `--output FILE` | decode | Output filename. Default: input's basename + `.mp4` (stereo input appends `_left`/`_right`). |
| `--ffmpeg-args '...'` | all | Extra args appended verbatim just before the output file, shell-word-parsed. Since ffmpeg lets the last occurrence of a flag win, this overrides the format preset: `--ffmpeg-args '-t 5 -af volume=0.5'`. Internal analysis passes (mods keyframe probe, `--roundtrip` validation) don't see it. |

## Stereoscopic (`decode`/`play`)

| Flag | Meaning |
|---|---|
| `--stereo {auto,none,frameseq,frameseq-r,tb,tb-r,sbs,sbs-r}` | Force the stereo layout instead of reading it from the file — needed when a 3D source carries no layout descriptor. `-r` variants store the right eye first. `none` treats input as 2D. |
| `--eyes {both,left,right,packed}` | decode: `both` (default) writes one file per eye; `left`/`right` writes just that eye; `packed` keeps the interleaved stream untouched. play: `both` shows eyes side by side; `left`/`right` plays one eye full-window; `packed` plays as stored. |

`moflex3d` and `3ds_camera3d` take a second input file (right eye) directly on
the command line instead of using `--eyes`:

```sh
python3 encode.py moflex3d adpcm left.mp4 right.mp4
python3 encode.py 3ds_camera3d adpcm left.mp4 right.mp4
```

`--layout N` (default `4`) sets the MOFLEX 3D layout for `moflex3d` output.

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

## RVID-only (`rvid`)

| Flag | Meaning |
|---|---|
| `--rvid-mode {rgb555,rgb565,256}` | Pixel mode: `rgb555` (default, unlimited color), `rgb565` (max color), `256` (8bpp palette). |
| `--no-compress` | Store raw 16bpp frames instead of Nintendo LZ10. |
| `--rvid-interlaced` | One field per frame. |
| `--rvid-no-dither` | Disable the checkerboard ordered dither used when reducing to 16bpp. |

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
