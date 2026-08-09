# FVMV — Nintendo / Pokemon GBA Video

State as of 2026-08-08. `FVMV` is the previously unsupported format in the
Nintendo-published Pokemon GBA Video cartridges.  It is not ADS, Hydrogen,
VXGB or VX++.  Pokemon Volume 1 (`MPAE`, maker `01`, 32 MiB) contains two FVMV
streams, one for each episode.

Public GBA Video summaries distinguish the Nintendo-published Pokemon carts
from Majesco's catalog, but no public codec specification or source indexed by
the `FVMV` magic was found.  The implementation here was recovered from the
player in the cartridge itself.

## Volume 1 stream inventory

| stream | ROM offset | data size | frames | geometry | duration |
|---|---:|---:|---:|---:|---:|
| A Hot Water Battle | `0x1fbfc` | `0xf2b1a8` | 9936 | 240x160 | 20:42.000 |
| For Ho-Oh the Bells Toll! | `0xf4add4` | `0xf42d58` | 9963 | 240x160 | 20:45.375 |

Video is 8 fps.  Audio is mono signed 8-bit PCM at 65536 Hz after decoding.
The native framebuffer representation is packed little-endian BGR555.

## Stream header

All integers are little-endian.

| offset | meaning |
|---:|---|
| `+0x00` | `FVMV` |
| `+0x04` | frame/record count |
| `+0x08` | width (240) |
| `+0x0c` | height (160) |
| `+0x10` | player clock value (1704 in both Volume 1 streams) |
| `+0x14` | byte size of the record area following the 0x20-byte header |
| `+0x18` | player/audio configuration, not yet named |
| `+0x1c` | player/audio configuration, not yet named |

Records begin at `header + 0x20`; the last record ends exactly at
`header + 0x20 + data_size`.

## Record layout

| offset | meaning |
|---:|---|
| `+0x00` | record bytes following this 16-byte record header |
| `+0x04` | signed player timing/control value |
| `+0x08` | player timing/control value |
| `+0x0c` | sequence number |
| `+0x10` | video payload size |
| `+0x14` | video payload |
| `+0x10 + video_size` | 8-byte audio header |
| `+0x14 + video_size` | compressed-audio byte count |
| `+0x18 + video_size` | compressed audio |

The audio byte count is a multiple of 40.  One 40-byte block reconstructs 256
32-bit intermediate samples and the cartridge's synthesis stage produces 1024
signed 8-bit PCM samples.  Ordinary records contain eight blocks, or exactly
1/8 second at 65536 Hz.  Each stream carries 1.25 seconds of audio priming past
the video duration; muxing stops at the video end.

## Video payload

The first two little-endian words give the lengths of the first and second
substreams.  The payload is therefore:

```
u32 control_size
u32 literal_size
u8  control[control_size]
u8  literals[literal_size]
u8  indices_and_motion[video_size - 8 - control_size - literal_size]
```

The decoder walks a bit-coded recursive block grammar over 8x8 cells.  Leaves
can copy from the previous BGR555 frame, apply indexed motion, interpolate
neighbours or reconstruct literal colors.  It alternates two 76800-byte frame
buffers.  Unlike the ActImagine codecs, FVMV reconstructs the GBA's packed
BGR555 display image directly rather than producing YUV planes.

## Cartridge decoder image and offline implementation

Volume 1 copies 32 KiB from ROM `0x1ef3008` to IWRAM `0x03000000`.  Relevant
entry points in that image are:

| IWRAM address | role |
|---:|---|
| `0x03005fe8` | decode one FVMV video payload |
| `0x03001b88` | expand one 40-byte audio block to 256 intermediate samples |
| `0x03001d18` | synthesize 1024 signed 8-bit PCM samples |

`tools/gba_video/fvmv_decode.py` finds the decoder image by its video-entry
signature, executes these self-contained ARM routines offline with Unicorn and
feeds the decoded streams to FFmpeg.  This is deterministic codec execution;
it does not run the game, render an emulator window or record emulator audio.

For an end-to-end decode:

```
python3 -m pip install unicorn
tools/gba_video/fvmv_decode.py pokemon-volume-1.gba
tools/gba_video/fvmv_decode.py pokemon-volume-1.gba -s 0 -o episode-1.mp4
tools/gba_video/fvmv_decode.py pokemon-volume-1.gba -s 1 -o episode-2.mp4
```

The parser has synthetic regression coverage in
`tools/gba_video/test_fvmv_decode.py`.  More importantly, decoded frame 25 from
stream 0 has SHA-256
`b79e3fb765a468e8528da30f8d87c3ac107e6436d3c98ad40166eda8ad569d73`,
byte-for-byte identical to the destination framebuffer immediately after the
same decoder call in mGBA.

## Scope still open

The offline implementation recognizes the decoder image used by Pokemon Volume
1.  The other three Pokemon volumes should be tested before claiming the entry
signature is universal.  A clean C translation of the ARM routines would allow
the codec to move from the RE tool into `libavcodec`; the current implementation
already provides complete episode extraction and a hardware-exact reference for
that translation.
