# VX++ (GBA Video) — handoff

State as of 2026-08-08. The full reverse-engineering narrative is in
`gba_video_vxpp.md` (24 sections); this is the short version for picking the work back up.

## Where things are

- Repo: `/Volumes/SSD/dlz/Folders/mobipeg` (moved here in a `~/Downloads` reorg — it is **not**
  under `~/Downloads` any more). Pushes to `quatric/mobipeg` as `quatric`, no assistant trailer.
- Cart: `/Volumes/SSD/dlz/Folders/Documents/Game Boy Advance Video - Shrek + Shark Tale (USA) (Rev 5).gba`
  (md5 `ff7d14c12b6b006f44e224daa0c8857c`).
- Working data: `build_gbavx/` at the repo root, listed in `.git/info/exclude` (not `.gitignore`).
  Everything in it is regenerable from the ROM.

## Rebuild and check

```
python3 tools/gba_video/gbavx_extract.py "$ROM" -o build_gbavx   # streams, codebook, IWRAM image
python3 tools/gba_video/vx_validate.py "$ROM"                    # must print 353/353 segments exact
python3 tools/gba_video/vx_audio_validate.py                      # must print 353/353 audio segments exact
python3 tools/gba_video/vx_native_packet_validate.py "$ROM"      # must print 353/353 native packets exact
python3 tools/gba_video/vx_decode.py -n 150 --raw /tmp/out.yuv   # decode to raw NV12
python3 tools/gba_video/vx_audio_decode.py build_gbavx/stream02.audio -o stream02.wav
./ffmpeg -resource 2 -i "$ROM" -map 0:a:0 -c:a pcm_s16le stream02-native.wav
ffmpeg -f rawvideo -pix_fmt nv12 -s 240x112 -r 7 -i /tmp/out.yuv -c:v mpeg4 -q:v 2 out.mp4
```

`vx_validate.py` is the regression gate. It parses all four streams against the container's own seek
table and must come out **353/353**; the complete Python 3.10 run takes roughly six minutes. Run it
after touching anything upstream of `decode_unit`. `vx_audio_validate.py` checks all audio boundaries
in under a second. `vx_native_packet_validate.py` independently checks every native `GVX1` prefix and
byte-swapped payload against the ROM.

Note the repo's own `./ffmpeg` is the mobiclip-patched x264 build and cannot encode normally (it has a
stray `dump_yuv` set). Homebrew's `libx264` also SIGBUSes in this tree — hence `mpeg4` above.

## The files

| file | role |
|------|------|
| `gbavx_extract.py` | pulls streams, the codebook and `iwram.bin` out of the cart |
| `vx_dis.py` | disassembles any range of the IWRAM image (`./vx_dis.py 0x03000ba4 0x60`) |
| `vx_grammar.py` | generated per-dispatcher mode grammar |
| `vx_sim.py` | bitstream parser; `decode_unit(..., sink=)` emits one dict per node |
| `vx_reconstruct.py` | pixel primitives, all read off the ARM |
| `vx_validate.py` | the 353/353 gate |
| `vx_decode.py` | driver: symbols → frame buffer → PGM or raw NV12 |
| `vx_audio_decode.py` | raw AFrames → mono 16-bit WAV |
| `vx_audio_validate.py` | the independent 353/353 audio framing gate |
| `vx_native_packet_validate.py` | the native demuxer's 353/353 video packet gate |

## What is known

Everything below was read out of the decoder image, not guessed.

- **No emulator needed.** The IWRAM image is copied from ROM at boot: **ROM `0xbcc8` = `0x03000000`**.
  Every `0x03xxxxxx` address in the docs indexes straight into `build_gbavx/iwram.bin`.
- **Codebook at ROM `0x9ae4`** — not the `0xa000` that sections 3–5 assumed.
- Bitstream grammar is complete: 16 dispatchers over a recursive rectangular split, exactly the 4×4
  grid of {2,4,8,16} sides. Mode 1 halves height, mode 2 halves width. Modes 12–23 are modes 0–11
  plus a residual.
- Intra: 16×16 modes 0–3, four chroma modes, all nine 4×4 modes (H.264's, in H.264's numbering, but
  with **no neighbour substitution**). Mode 3 is a recursive midpoint subdivision, not plane
  prediction; its centre axis is vertical iff `log2(w)+log2(h)` is even.
- Inter: **full-pel only** — the four-entry table at `0x03001568` is `ldm` alignment fixups, not
  filter phases. Vectors are predicted by the median of left/above/above-right, packed as the
  *arithmetic* sum `mv_x + (mv_y << 8)`. Three references = the last three decoded frames, from a
  four-buffer ring.
- Mode 4 is midpoint with a coded corner; mode 5 is motion compensation with a per-component DC
  correction (a fade mode).
- Residual: 6-bit CBP per 8×8 quadrant; six "reconstruct variants" are 3 planes × {full, DC-only}.
- Residual quadrants are raster ordered using the coded leaf's width. In particular, an 8×16 leaf's
  two groups are vertical; the old driver incorrectly placed them horizontally (§22).
- Chroma is **one interleaved plane**, two bytes per sample at the luma pitch — i.e. NV12's UV plane.
  Colour is **YCbCr**, not the YCgCo the DS-era ActImagine variants use.
- Context base is `ctx + 0xec` (`FUN_03000520` re-bases what the frame driver hands it).
- Whole-block luma DC rounds the top and left edge means separately before averaging them. Intra-4×4
  availability tests use a plane-relative offset, not the Python buffer's synthetic safety margin.
- `vx_decode.py` reads geometry and seek entries from the extracted header, reloads the per-segment
  quantiser, and uses the hardware's contiguous four-slot frame arena.
- Fresh hardware dumps at frames 0, 1, 5, 7, 8, 9, 10, 100 and 325 of stream 0 are byte-exact in
  both Y and UV. Frame 100 covers the old busy-frame artifacts and frame 325 crosses the first seek.
- Video is **7 fps**, not `0x30c3/1024 = 12.19 fps`. The audio cadence proves it: `128/7` AFrames per
  video frame × 7 fps × 128 samples/AFrame = 16384 Hz.
- Audio is the existing mono 128-sample VX LPC/pulse codec. The GBA difference is only framing: one
  3124-byte codebook followed by raw variable-size AFrames. All 353 seek segments frame exactly, and
  `vx_audio_decode.py` writes a valid 16384 Hz mono WAV (§24).
- Native audio is complete. `libavformat/gbavx.c` scans the ROM, exposes the selected movie's audio,
  and builds exact sample indexes from all container seek offsets. `-ss` output matches continuous
  decode byte-for-byte at tested positions through second 4000. The Python and native decoders also
  produce identical complete stream-2 PCM (SHA-256
  `b4c973965726440e038274804924d068e12028d7a5da29d6d8961c1e0f64edf6`).
- Native video packetization is complete. The demuxer exposes `gba_vx`, one independently decodable
  packet per seek interval, with the exact leading skip/valid-bit/frame counts defined in
  `libavcodec/gba_vx.h`. All 353/353 native packet extents match the container table. Fresh Python
  decoding at seek frames 325 and 612 matches continuous decoding for every tested frame. There are
  deliberately no fake per-frame packet boundaries.

## What is left

1. **Native `gba_vx` video decoder.** The ROM demuxer, segment packet contract and native audio path
   are done. Port the hardware-exact Python video reference into `libavcodec`; one decoder packet is a
   complete self-contained seek segment and may emit many frames. The decoder should validate the
   `GVX1` prefix, skip the recorded leading bits, read the per-segment quantiser and emit the prefix's
   frame count while retaining its four-slot reference arena within the packet.
2. **The earlier `VXGB` GBA revision.** Kostya documented a Majesco-published GBA stream beginning
   with `VXGB` and described it as a simplified H.264 relative close to `VXDS`. The actual
   `Shrek (USA) (Rev 5)` ROM confirms a `VXGB` stream at `0x20200`, while `Shrek (USA) (Rev 6)` has
   `VX++` at the same offset and a very similar-looking header. Neither the native demuxer nor the
   Python extractor accepts `VXGB` yet. Start by teaching the extractor to classify both magics and
   compare the Rev 5 stream/seek layout and decoder image against the now-solved Rev 6 `VX++` path.
   Source: [Kostya's codec note](https://codecs.multimedia.cx/2025/08/a-quick-glance-at-another-bunch-of-codecs/).
3. **Optional audio hardware capture.** Framing is exact and the codec core was already validated on
   DS/SX data, but dumping GBA PCM would provide the same byte-for-byte final check used for video.

## Traps worth remembering

- Do not put project inputs in `/tmp` — a previous session lost the codebook and streams that way, and
  only the ROM offsets written into the doc made them recoverable.
- Capstone stops at the first non-instruction word; `vx_dis.py` decodes word by word for that reason.
- `strb` truncates, it does not clamp.
- The midpoint fill takes its corner in a register, not by reading the byte back — it matters for
  mode 4, where the stored byte is truncated and the register value is not.
- Halving both sides only terminates on squares; degenerate blocks keep bisecting along the long side.
