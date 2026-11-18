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
| Bink 1 (BIKi) | `.bik` | PC games (RAD Game Tools) | ✅ | ✅ |
| Bink 2 (KB2j) | `.bk2` | PC and console games (RAD Game Tools) | ✅ | ✅ |
| BPK1 | `.bpk`, `.bpk1`, `.apd` | Nintendo 3DS Swapdoodle / Swapnote | ✅ | ✅ |
| DPG | `.dpg` | Nintendo DS (MoonShell) | ✅ | ✅ |
| Factor 5 DivX | `.vid` | GameCube (Factor 5) | ✅ | ✅ |
| FastVideo DS | `.fv` | Nintendo DS | ✅ | ✅ |
| FVDS (First Hunt) | `.fv` | Nintendo DS (Metroid Prime Hunters: First Hunt demo) | — | ✅ |
| Flipnote Studio | `.ppm` | Nintendo DS / DSi | ✅ | ✅ |
| Flipnote Studio 3D | `.kwz` | Nintendo 3DS | ✅ (signed when given the key) | ✅ |
| GBA FVMV | `.gba` | Game Boy Advance (Pokémon) | — | ✅ |
| GBA Video (ADS / Hydrogen) | `.mmstr` / `.gba` | Game Boy Advance (Majesco) | ✅ (`.mmstr` resources, video only) | ✅ |
| GBA Video (Caimans 2.2) | `.gba` | Game Boy Advance | — | ✅ |
| GBA Video (CaimansPro) | `.gba` | Game Boy Advance | — | ✅ |
| GBA Video (VX++) | `.gba` | Game Boy Advance (ActImagine) | — | ✅ |
| HVQM4 | `.h4m` | GameCube / Wii (Hudson Soft) | ✅ | ✅ |
| Nintendo Channel | `.3gp` | Wii | ✅ | ✅ |
| Nintendo 3DS Camera (2D / 3D) | `.avi` | Nintendo 3DS | ✅ | ✅ |
| MIVF (M2Y1 / M2Y2) | `.mivf` | Nintendo 3DS homebrew | ✅ | ✅ |
| MO | `.mo` | Nintendo Wii | ✅ | ✅ |
| MOC2 / MOC3 | `.mo` | Nintendo Wii (early MobiClip) | — | ✅ (`fla2`/`fla3` are Em-Flash (Flash 3-5) movies, rendered by the `emflash` decoder: shapes, text, buttons, sprites, masks and ActionScript 1; `-autoclick 2` presses on-screen buttons so games get past their menus, `-fla_frames N` sets the length, `-video_width`/`-video_height` render at any size; `-fla_data 1 -c copy -f data out.swf` writes the movie as an SWF file, `-fla_raw 1` the bare payload) |
| MODS | `.mods` | Nintendo DS | ✅ | ✅ |
| MOFLEX 2D | `.moflex` | Nintendo 3DS | ✅ | ✅ |
| MOFLEX 3D | `.moflex` | Nintendo 3DS | ✅ | ✅ |
| Super MOFLEX | `.moflex` | Nintendo 3DS | ✅ | ✅ |
| RVID | `.rvid` | RocketVideo (DS) | ✅ | ✅ |
| THP | `.thp` | GameCube / Wii | ✅ | ✅ |
| Topos player (2D / SBS 3D) | `.mkv` | Nintendo 3DS homebrew | ✅ | ✅ |
| Wii Photo Channel | `.avi` | Wii | ✅ | ✅ |
| TiVo TyStream | `.ty` / `.ty+` / `.tmf` | TiVo (Series 1–3) | ✅ | ✅ |
| VX | `.vx` | Nintendo DS | ✅ | ✅ |

### Audio formats

| Format | Container | Platform | Encode | Decode |
|--------|-----------|----------|--------|--------|
| AST | `.ast` | GameCube / Wii | ✅ | ✅ |
| BCSTM | `.bcstm` | Nintendo 3DS | ✅ | ✅ |
| BCWAV | `.bcwav`, `.cwav` | Nintendo 3DS | ✅ | ✅ |
| BFSTM | `.bfstm` | Nintendo Wii U | ✅ | ✅ |
| BFWAV | `.bfwav`, `.fwav` | Nintendo Wii U / Switch | ✅ | ✅ |
| BNS | `.bns` | Nintendo Wii (banner sound) | ✅ | ✅ |
| BRSTM | `.brstm` | Nintendo Wii | ✅ | ✅ |
| BTSND | `.btsnd` | Nintendo Wii U (boot sound) | ✅ | ✅ |
| DSP-ADPCM | `.dsp` | GameCube / Wii / 3DS | ✅ | ✅ |
| HPS | `.hps` | GameCube (HAL Laboratory) | — | ✅ |
| MobiClip mus1 | `.mo` | Pixter Multimedia | — | ✅ |
| NUS3BANK | `.nus3bank`, `.nus3audio` | Nintendo Wii U / 3DS | ✅ | ✅ |
| RWAV | `.brwav`, `.rwav` | Nintendo Wii | ✅ | ✅ |
| SADL | `.sadl` | Nintendo DS (Level-5) | — | ✅ |
| Wii Photo Channel AAC | `.m4a` | Wii | ✅ | ✅ |
| Nintendo 3DS Sound AAC | `.m4a` | Nintendo 3DS | ✅ | ✅ |

The DSP-ADPCM encoder derives its predictor coefficients over the whole
stream, the same autocorrelation and Levinson refinement Nintendo's own
DSPADPCM tool uses, so these land around 80 dB SDR. AST's ADPCM is AFC, a
variant with a predictor table fixed by the format rather than derived, and
lands around 65 dB; AST can also be written as uncompressed PCM, as can
BRSTM, BFSTM and BCSTM. BTSND is always 48 kHz stereo big-endian PCM,
because its player on the console reads no format fields at all.

BNS files are usually LZ10-compressed, wrapped in an IMD5 header, or both.
Decoding unwraps whichever combination it finds; `-compress 1` writes the
compressed form.

RWAV, FWAV and CWAV are the same single-sample wave container across three
console generations — Wii `.brwav`, Wii U / Switch `.bfwav`, 3DS `.bcwav` —
and share one demuxer and one muxer here. All three carry DSP-ADPCM
(`adpcm_thp`, or `adpcm_thp_le` when the BOM says little-endian), planar
PCM16 or planar PCM8, and all three store their channels as separate blocks
referenced from the INFO chunk rather than interleaved. The byte order is
read from the `0xFEFF` / `0xFFFE` BOM at offset 4, not assumed from the
magic, so a little-endian `.brwav` and a big-endian `.bcwav` both decode.
These are the individual samples that RWAR / FWAR / CWAR wave archives and
the RBNK / BRSAR instrument banks reference; the sibling
[wiimms-szs-tools-plus](https://github.com/quatric/wiimms-szs-tools-plus)
unpacks those archives into the loose wave files this reads and writes.

```sh
ffmpeg -i input.wav -c:a adpcm_thp out.brwav
ffmpeg -i input.wav -c:a adpcm_thp out.bfwav
ffmpeg -i input.wav -c:a pcm_s16le_planar out.bcwav
ffmpeg -i sound.bcwav out.wav
```

They are FFmpeg-level formats only: `encode.py` and the GUI do not list them
among their audio targets yet, so use `ffmpeg` directly.

Several encoders are written against a reference implementation and give
its output exactly for the same input, which is how they are tested:

| Output | Reference | What matches |
|--------|-----------|--------------|
| FastVideoDS `.fv` (video, `adpcm_ima_fvds` audio, container) | [FastVideoDSEncoder](https://github.com/Gericom/FastVideoDSEncoder) | the whole file |
| RVID `.rvid` | [Vid2RVID](https://github.com/RocketRobz/Vid2RVID) | the whole file |
| MOFLEX FastAudio | [MoflexTool](https://github.com/koori0831/MoflexTool) | every audio packet |
| MOFLEX IMA ADPCM | MoflexTool | every sample code |
| Flipnote Studio 3D `.kwz` layers | notes saved by Flipnote Studio 3D | the compressed data of a layer that stands alone or skips unchanged tiles in runs, for the same layer contents |

Both Flipnote formats are written as well as read. `.ppm` notes are 256x192,
1-bit and signed with the DSi application's key. `.kwz` notes are 320x240 in
the six Flipnote colours spread over three layers, with ordered dithering, a
JPEG thumbnail and the BGM in the format's 2/4-bit ADPCM. Flipnote Studio 3D
only opens notes carrying its own RSA-2048 signature, and that key is not
part of mobipeg: pass it with `--flipnote-key` (`-kwz_key` on the `ffmpeg`
command line) to get a signed note, or leave it out for an unsigned one that
mobipeg and other readers still play.

```sh
python3 encode.py flipnote pcm input.mp4
python3 encode.py flipnote3d pcm input.mp4 --flipnote-key kwz_key.der
```

MOC2 and MOC3 are the two MobiClip container generations that predate the
`.mo` layout the `mo` demuxer reads, and use the same `.mo` extension. The
`moc3` demuxer covers the `fla2`, `gvi3` and `vid2` payload tags; `vid2`
carries MOC5-style chunk headers, while `fla2` and `gvi3` are a raw
bitstream whose frame boundaries are recovered by scanning. It is
decode-only — new files are written in the `mo` layout — and `vid2` seeking
is not implemented. Its header size is not fixed (64 bytes on MOC2,
240/284/288/292 on MOC3), so it is read from the header rather than assumed.

Decode-only inputs (the FVMV, Caimans, and VX++ GBA Video cartridge families,
First Hunt `.fv`, MOC2 / MOC3 and HPS, SADL and mus1 audio) can be transcoded
into any of the encodable formats above, or previewed with
`encode.py decode <file>`. Series-3 TiVo
TyStreams (and MFS VideoClip resources) are handled through an internal
port of the `s3tots` tool, which losslessly rewraps them to MPEG-2 TS
before FFmpeg reads them.

TiVo `.ty` encoding (the `ty` muxer, `mpeg2video` + `mp2`/`ac3`) is ported
from the ["tyffmpeg"](https://repo.mariocube.com/TiVo%20Restore%20Images/_Tools/tyffmpeg.zip)
encoding library and targets the common Series 2 (Stand-Alone) container
layout; it requires `--enable-gpl` and does not write `.ty+`'s trailing XML
metadata block or Series 1/3-specific PES framing.

HVQM4 encoding (the `hvqm4` codec + muxer) is a from-scratch implementation:
per-block DC prediction, greedy nest-basis matching pursuit for the AC/texture
residual (in place of the original's VQ codebook), literal-block escape, and
P-pictures with per-8x8 integer-pixel motion search plus intra fallback. It is built
directly against the bundled decoder rather than any original Hudson Soft
encoder. Half-pixel search, predictive AOT residuals, and B-pictures are not
implemented yet. Set FFmpeg's `-g` option to control the I-picture interval
(`-g 1` produces an all-intra stream).

### Encoding ADS-era GBA Video

The `ads_gba` encoder and `mmstr` muxer write the in-house codec Majesco used
before the GCC-era carts moved to ActImagine VX. Frames are all-intra and carry
no prediction at all: a frame is nothing but 8-bit indices into a 256-entry
codebook, which is built by k-means over every block in a chunk.

```sh
ffmpeg -i input.mp4 -s 240x160 -pix_fmt rgb24 -c:v ads_gba movie.mmstr
```

The bundled front ends expose both compressor generations directly:

```sh
python3 encode.py gba_ads none input.mp4       # LZMA / Dragon Ball GT lineage
python3 encode.py gba_hydrogen none input.mp4  # Inflate / Hydrogen lineage
```

They also appear as separate GBA Video choices in the GUI. Both outputs are
`.mmstr` resources for insertion into a compatible cartridge image; they are
not bootable `.gba` ROMs.

`-chunk_frames` sets how many frames share one codebook, trading size against
sharpness — 30 frames of `testsrc2` span 37.9 KB at `1` and 10.9 KB at `32`,
for about 31 dB either way, because what actually compresses here is
consecutive frames reusing the same entries. `-mode` picks the block geometry:
the even modes keep one chroma sample for a whole block, the odd ones one per
block row. Mode 5 is refused rather than encoded, because it declares two
chroma samples for a three-row block and the decoder reads the third off the
end of the entry into its neighbour — a ROM quirk the decoder reproduces
faithfully and that nothing can sensibly target. Modes 2, 3 and 4 have
three-pixel-tall blocks, so they need a height divisible by three and cannot
cover the GBA's 160 lines.

Blobs go through one of two compressors, picked with `-compression`:

* `0` (default) — the LZMA the ADS-era / Dragon Ball GT lineage runs
  (`adslzmaenc.c`), stock LZMA with `lc=0, lp=0, pb=2` and no end marker, the
  exact inverse of the bundled decoder. Every probability-coded decision
  mirrors the decoder's bit-read one for one, which is what lets two
  independently-run range coders track one adaptive model without either side
  ever transmitting it. The match finder is a plain greedy hash-chain search
  with repeated-distance and short-rep preference, not a multi-pass optimal
  parse — that costs ratio, not correctness.
* `1` — the Hydrogen-era compressor (`majescoenc.c`) that Dora the Explorer and
  the rest of that lineage use instead: DEFLATE-shaped, but bits run most
  significant first out of little-endian halfwords, a block header is a bare
  2-bit type with no BFINAL, and the uncompressed size in each blob's prefix is
  the only thing that ends the stream. Stored, fixed-Huffman and
  dynamic-Huffman encodings are all built and the cheapest kept, which bounds
  the incompressible case.

Both write the same 8-byte `[uint32 uncompressed_size][uint32 params]` prefix,
and reading a bare `.mmstr` infers which compressor was used from the first
blob rather than needing to be told.

One caveat: this writes `.mmstr` resource files, not cartridges — nothing here
rebuilds a ROM. Audio is decode-only, since the cart's ADPCM has no encoder
yet.

### Wii Photo Channel and Nintendo Channel

`wii_photo` writes an ordinary Motion-JPEG AVI for the Wii Photo Channel. It
defaults to 640x480 (the Channel accepts up to 848x480), with 32 kHz PCM audio
when audio is selected. Nintendo only guarantees the video component, so use
`none` if playback of an AVI's sound track is unreliable on a particular Wii.

```sh
python3 encode.py wii_photo pcm input.mp4
```

`wii_photo_m4a` writes AAC-LC M4A for Photo Channel 1.1. `3ds_sound` writes
the same broadly compatible 44.1 kHz / 128 kb/s stereo AAC-LC profile; it is
within Nintendo 3DS Sound's 16-320 kb/s and 32-48 kHz limits.

```sh
python3 encode.py wii_photo_m4a aac input.wav
python3 encode.py 3ds_sound aac input.wav
```

`nintendo_channel` reproduces the delivery profile found in the UK Nintendo
Channel 2009 archive: `3gp6`, H.264 constrained-baseline level 2.1 at 378x284
and 25 fps, and stereo AAC-LC at 32 kHz. It requires the same `--enable-libx264`
build configuration as MobiClip video encoding.

```sh
python3 encode.py nintendo_channel aac input.mp4
```

`3ds_camera` writes a 2D-compatible Camera AVI: 480x240, 20 fps, Motion JPEG,
and 16 kHz mono IMA ADPCM. `3ds_camera3d` takes left and right input files and
writes the Camera's native two-track stereoscopic MJPEG layout (left eye first).
Copy either file to a DCIM camera folder and name it in the Camera's
`HNI_####.AVI` style.

```sh
python3 encode.py 3ds_camera adpcm input.mp4
python3 encode.py 3ds_camera3d adpcm left.mp4 right.mp4
```

### Topos 3DS video player

[Topos](https://github.com/mockmodular/topos_3ds-video-player) is a 3DS
homebrew player for Matroska files. It plays 800x240 side-by-side frames as 3D
and 400x240 frames as 2D. Four presets follow the player author's encoding
settings:

| `fmt` | Video | Audio (default) | For |
|---|---|---|---|
| `topos` | H.264 High@3.1, 400x240 | AAC 128k | New 3DS, decoded in hardware |
| `topos3d` | H.264 High@3.1, 800x240 SBS | AAC 128k | New 3DS, decoded in hardware |
| `topos_mpeg2` | MPEG-2 Main, 2 Mbit/s CBR, 400x240 | MP2 96k | Old 3DS, software decode |
| `topos3d_mpeg2` | MPEG-2 Main, 2 Mbit/s CBR, 800x240 SBS | MP2 96k | Old 3DS (the player's author reports 3D does not keep up there yet) |

Each eye is centre-cropped to the screen's 5:3 shape by its display aspect, so
nothing is stretched, then scaled with a spline kernel into BT.709. The frame
rate stays the source's unless `--fps` is given. The 3D presets take a left and
a right input file, or a single packed stereoscopic source like `moflex3d`;
half-resolution side-by-side (1920x1080 holding two 960x1080 eyes) and
top-bottom sources are recognised by their shape and get their aspect back
before cropping. `--quantizer` sets the H.264 CRF (default 14) and
`--bitrate` replaces the rate on either codec. Audio is 48 kHz stereo `aac`,
`mp2` or `none`.

```sh
python3 encode.py topos aac input.mp4
python3 encode.py topos3d aac left.mp4 right.mp4
python3 encode.py topos3d aac movie_sbs.mkv --stereo sbs
python3 encode.py topos3d_mpeg2 mp2 movie_sbs.mkv --stereo sbs
```

### MIVF 3DS movie player

[MIVF](https://github.com/Oldhimaster1/MIVF) is a 3DS homebrew movie player
with its own container and codecs. `mivf` writes its files natively:

- M2Y1 video at 400x240, keeping the source frame rate. The encoder
  (`-c:v m2y1`) is a port of the project's own native encoder and writes the
  same bytes. `--quantizer` sets its QP (default 34, 1-51). `--m2y2` range-codes
  the frames as M2Y2, which gives the same pictures in a file about 25-30%
  smaller.
- Sound as `ia4m` (mono IMA ADPCM, the default) or `pc16` (16-bit PCM,
  stereo), or `none`. Each frame carries exactly rate / fps samples, so the
  rate is picked to divide evenly: 44.1 kHz for 25 and 30 fps, 48 kHz for
  24000/1001, and 30 kHz for 29.97 fps (`--audio-rate` picks another one
  that fits).
- A seek index at the end of the file, which the player reads instead of
  scanning.

Pictures are centre-cropped to the screen's 5:3 shape and scaled into
BT.601, which is the matrix the player converts with. The video is encoded
in parallel chunks of whole 240-frame keyframe intervals, one per CPU core,
so the file is byte-for-byte the one a single encoder would write. A
3-minute clip took 6.2 s on 10 cores, against 21.6 s in one piece.

```sh
python3 encode.py mivf ia4m input.mp4
python3 encode.py mivf pc16 input.mp4 --m2y2 --quantizer 30
```

`decode` reads `.mivf` files, including M2Y2 video.

### Bink 1 movies

`bink` writes Bink 1 movies (revision `BIKi`), the RAD Game Tools format many
PC games play their cutscenes in. The encoders
and the muxer started as a port of LibBink's `binkenc`, and the files decode
the same in FFmpeg's Bink decoder and in LibBink, pixel for pixel and sample
for sample. They have not been tried in RAD's own player.

- Video (`-c:v binkvideo`) keeps the source size and frame rate. Each 8x8
  block is coded as whichever of skip, motion copy, fill, two-colour pattern,
  intra DCT, motion-compensated inter DCT or raw costs least for the picture
  it gives (rate-distortion choice, motion search up to 15 pixels).
  `--bink-quality 0-10` sets the quality (default 3); `--quantizer 1-15` sets
  Bink's DCT quantiser directly (lower is finer). A keyframe every 120 frames.
- Audio (`-c:a binkaudio_dct`) is Bink DCT audio, mono by default (`bink`),
  since LibBink plays mono only; `bink_stereo` writes stereo, which FFmpeg
  decodes. The source rate is kept up to 48 kHz. `--bink-audio-quality
  0-10` (default 6) sets how finely each band is quantised: 6 is about
  140 kbit/s for mono 44.1 kHz.

The encoder is fast (a 400x240 60 fps clip encodes at about 350 frames a
second), so it is not split into chunks.

```sh
python3 encode.py bink bink input.mp4
python3 encode.py bink none input.mp4 --scale 640x480 --bink-quality 6
```

The FFmpeg side is plain `ffmpeg -i in.mp4 -c:v binkvideo -c:a binkaudio_dct
-ac 1 out.bik`; `-quality:v`, `-quant`, `-lambda` and `-motion_range` tune the
video, `-quality:a` the audio. `decode` reads `.bik` files.

### Bink 2 movies

`bink2` writes Bink 2 movies (revision `KB2j`). The video encoder is
[bink2enc](https://github.com/SBerda/bink2enc) by Simon Berda, ported from
Rust into FFmpeg as `-c:v bink2video`. Its picture reconstruction was then
changed to follow RAD's own decoder exactly (transform, motion compensation,
DC smoothing and deblocking), so the files decode pixel for pixel the same in
RAD's `bink2w32.dll` as in FFmpeg. bink2enc is AGPL-3.0, so the encoder needs
an FFmpeg built with `--enable-gpl --enable-version3` (see
[Building](#building)), and binaries that include it are AGPL-licensed.

- Video keeps the source size and frame rate, converted to full-range YUV.
  32x32 macroblocks are coded as intra, skip, motion (one or four half-pel
  vectors) or motion plus residual, whichever costs least for the picture it
  gives. `--quantizer 1-36` sets the quantiser (default 12, lower is finer;
  `--hq` uses 6). `--bitrate` turns on rate control instead, which moves the
  quantiser between 6 and 30 to meet the target. A keyframe every 125 frames,
  and another at each scene cut. An input frame identical to the one before
  is coded as a skipped frame.
- Audio is the same Bink DCT audio as `bink`, mono by default.

The Bink 2 decoder (`bink2video`, revisions `KB2a` to `KB2n`) started as a
port of the one in [NihAV](https://nihav.org) by Kostya Shishkov and was then
matched against RAD's `bink2w32.dll` under Wine: `KB2n` files from RAD's
current Bink 2 encoder decode the same as in RAD's player. Older revisions
are close but not exact: on sample files of `KB2f`, `KB2g` and `KB2i` the
first 60 frames come within 41 to 50 dB of RAD. Frames without flag
0x40000, which RAD's current encoder always sets, go through extra in-loop
filtering in RAD's decoder that FFmpeg does not do yet. The one difference left is in the
part of the last macroblock row or column past the picture rounded up to 16
pixels, which RAD's decoder never stores and reads back from uninitialised
memory; FFmpeg takes it as zero, and the encoder never codes an intra
macroblock there in an inter frame, so its own files are not affected. The
decoder needs the same `--enable-gpl --enable-version3` build, and `decode`
reads `.bk2` files. The slices are encoded in parallel: a 1280x720 clip
encodes at about 100 frames a second.

```sh
python3 encode.py bink2 bink input.mp4
python3 encode.py bink2 none input.mp4 --bitrate 4M
```

The FFmpeg side is `ffmpeg -i in.mp4 -c:v bink2video -c:a binkaudio_dct -ac 1
out.bk2`. `-quant` (or `-q:v`) sets the quantiser and `-b:v` or `-rate` (KiB/s)
the rate control target. `-qmin_b2`/`-qmax_b2` bound it, and `-g` sets the
keyframe interval. `-lambda`, `-deadzone`, `-search`, `-mv4`, `-rowflag` and
`-flags_b2` (whose low 2 bits pick 2, 3, 4 or 8 slices) are bink2enc's own
tuning switches.

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

`encode.py decode` does this automatically — it reads the layout from the side
data and writes `name_left.mp4` / `name_right.mp4`, or just one eye with
`--eyes left|right`, or the untouched stream with `--eyes packed`.

### Playback

`encode.py play <file>` opens any supported input in a window without writing
anything to disk, which is the quickest way to check a file:

```sh
python3 encode.py play movie.mods                   # DS clip, in a window
python3 encode.py play movie.moflex                 # 3D: both eyes side by side
python3 encode.py play movie.moflex --eyes left     # 3D: one eye, full window
```

Playback uses the bundled `ffplay`, which links the same decoders as `ffmpeg`,
so nothing is written to disk and no external player is involved. `ffplay` is
only built when the tree is configured with SDL2 (`--enable-sdl2`, the default
when SDL2 is installed); without it, `play` says so rather than falling back to
a system player that cannot read these formats.

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

## Building

Mobiclip **video encoding** (`-c:v mobiclip`, and therefore every `.mo` /
`.moflex` / `.mods` output) is implemented as a fork of x264, so it is only
compiled in when FFmpeg is configured with `--enable-libx264`. Configure
without it and the `mobiclip` encoder is simply absent — decoding and the
non-Mobiclip formats still work, which makes the omission easy to miss.

The fork is required; stock x264 does not implement the Mobiclip bitstream:

```sh
git clone https://github.com/quatric/x264 && cd x264
./configure --prefix="$PWD/../x264-install" --enable-static --enable-pic --disable-cli
make -j && make install
cd .. && PKG_CONFIG_PATH="$PWD/x264-install/lib/pkgconfig" \
  ./configure --enable-gpl --enable-libx264 && make -j
```

Confirm with `ffmpeg -encoders | grep mobiclip`.

The Bink 2 encoder (`bink2video`) is AGPL-3.0, so it is only built when
`--enable-version3` is added to the configure line as well.

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

The Bink 2 encoder (`libavcodec/bink2enc.c`) is ported from bink2enc and is
licensed under the GNU Affero General Public License, version 3 or later.

## Contributing

Patches should be submitted to [Forgejo](https://code.ffmpeg.org/FFmpeg/FFmpeg/pulls) or the [ffmpeg-devel mailing list](https://lists.ffmpeg.org/mailman3/lists/ffmpeg-devel.ffmpeg.org/) using `git format-patch` or `git send-email`. Github pull requests should be avoided because they are not part of our review process and **will be ignored**.

See [https://ffmpeg.org/developer.html](https://ffmpeg.org/developer.html) for more information.

Copyright (c) 2026 quatric
