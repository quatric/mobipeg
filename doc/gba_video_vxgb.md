# VXGB — the original ActImagine GBA Video codec

State as of 2026-08-08. `VXGB` is the video revision in
`Game Boy Advance Video - Shrek (USA) (Rev 5)`; Rev 6 replaces it with
`VX++`. Kostya described the format as a simplified H.264 relative of `VXDS`
and noted that the player called unidentified code in fast RAM. Both parts are
now explained by the retail ROM and a live mGBA capture.

Reference ROM SHA-256:

```
f6d4207be3cc87a8d0aa833fd26a251d78a8f659451361021c5b705210c18e5a
```

The original public note is
[A quick glance at another bunch of codecs](https://codecs.multimedia.cx/2025/08/a-quick-glance-at-another-bunch-of-codecs/).

## Container and audio

The stream at ROM `0x20200` uses the same 0x38-byte container header as
`VX++`: relative video/audio/chapter/seek offsets, a chapter list and
`{frame, video_bit, audio_byte, 0}` seek entries. The reference stream is
240x160, 37,837 frames at 7 fps with 16,384 Hz mono audio and 20 chapters.
The important revision-specific fields are:

```
magic       VXGB
quantizer   32
audio       +0x0261d200
chapters    +0x032cfa00
seek        +0x032cfc00 (196 entries, including the sentinel)
```

Audio is unchanged. Its 3,124-byte trained codebook and raw variable-size
AFrames obey the existing VX audio grammar; all 195 non-sentinel seek segments
tile exactly, and the Python decoder produces valid PCM.

`tools/gba_video/gbavx_extract.py` recognizes both magics and extracts the
VXGB stream plus its decoder image. `tools/gba_video/vxgb_validate.py` checks
the video grammar against the container's own seek-bit oracle.

## Where the player and decoder come from

The previously mysterious fast-RAM code is copied from the cartridge. There
are two phases:

- boot/interrupt helper: ROM `0x37a8` to IWRAM `0x03000000`;
- active VXGB decoder: ROM `0xb548` to IWRAM `0x03000000` (32 KiB).

A dump taken while the DreamWorks intro was visibly playing matches the second
ROM window in 32,516 of 32,768 bytes. The 252 differing bytes are runtime
state, relocated pointers and self-modified width/height immediates. This is
why static searches for a conventional function-by-function initialization
miss the code: the movie player replaces essentially the whole IWRAM image.

The surrounding player is another direct mapping: ROM `0x8a00` to EWRAM
`0x02000000`. Under that mapping the `VXGB` literal is `0x020019d4` and
`File Format Error` is `0x02002ab4`, exactly where the live image references
them.

## Relationship to VX++

The 32 KiB VXGB and VX++ decoder images are about 71% byte-similar. Sequence
matching maps all sixteen recursive block dispatchers exactly:

| role | VX++ | VXGB |
|---|---:|---:|
| top 16x16 | `0x03001dac` | `0x03001ee0` |
| first horizontal child | `0x03002184` | `0x030023c0` |
| first vertical child | `0x030030f4` | `0x03003510` |
| deepest dispatcher | `0x030055fc` | `0x03005e70` |
| residual driver | `0x03005650` | `0x03005eb8` |

The complete address map is in `tools/gba_video/vxgb_sim.py`. The prediction
grammar is the same recursive rectangular tree already solved for VX++:
Exp-Golomb modes, paired split children, H.264-style intra signalling and the
same per-mode signed side values.

## The real syntax break: residuals

VX++ stores a proprietary coefficient VLC codebook elsewhere in cartridge ROM.
VXGB instead uses H.264-style CAVLC at `0x030062e0`:

- `TotalCoeff`/`TrailingOnes` selected by the rounded mean of left/top
  nonzero counts;
- trailing-one signs, adaptive level suffixes, `total_zeros` and `run_before`;
- four 4x4 luma blocks plus one paired Cb/Cr bit per 8x8 quadrant;
- the standard ActImagine/H.264 VLC tables embedded in IWRAM.

Those tables are byte-for-byte the family already used by the DS VX decoder in
`libavcodec/vx.c`, so the new parser shares the generated definitions in
`libavcodec/vx_cavlc_vlc.h`. VXGB's 32-entry coded-block permutation is taken
directly from IWRAM `0x03005e90`.

Two framing differences are just as important:

- header `+0x14` is the active quantizer (`32`), not zero;
- a seek segment begins directly with its first macroblock—there is no VX++
  per-segment quantizer delta and no one-bit inter-frame marker.

With those rules, the offline parser walks all 37,837 frames in all 195 seek
segments. Every one of the 194 independently recorded next-segment offsets is
bit-exact; the first interval, for example, consumes frames 0–154 and lands at
bit `499357`, exactly the target for frame 155. This checks the partition
grammar, every CAVLC decision and both framing differences without relying on
emulator timing.

`tools/gba_video/vxgb_decode.py` also connects those symbols to the solved VX++
prediction/reconstruction primitives. A 20-second 7 fps decode stays coherent
through the DreamWorks intro, and decoded frame 103 spatially matches the live
mGBA screenshot at emulator frame 900 (cloud, moon, sparkle and block edges).
The visible brightness difference is the final GBA YUV-to-BGR555 display path,
which the Python output leaves as native NV12.

## Remaining work

The remaining validation step is a byte-for-byte comparison of a live mGBA YUV
frame buffer, analogous to the VX++ frame suite; the visual frame-103 match is
strong but not a substitute for that. A native implementation can share the
CAVLC machinery already in `libavcodec/vx.c`; it still needs the GBA recursive
mode mapping, reference-ring packet contract and YCbCr output adapter.
