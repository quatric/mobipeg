# VX++ (GBA Video) — handoff

State as of 2026-08-08. The full reverse-engineering narrative is in
`gba_video_vxpp.md` (21 sections); this is the short version for picking the work back up.

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
python3 tools/gba_video/vx_decode.py -n 150 --raw /tmp/out.yuv   # decode to raw NV12
ffmpeg -f rawvideo -pix_fmt nv12 -s 240x112 -r 12.19 -i /tmp/out.yuv -c:v mpeg4 -q:v 2 out.mp4
```

`vx_validate.py` is the regression gate. It parses all four streams against the container's own seek
table and must come out **353/353**; it catches a broken parser within a second. Run it after touching
anything upstream of `decode_unit`.

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
- Chroma is **one interleaved plane**, two bytes per sample at the luma pitch — i.e. NV12's UV plane.
  Colour is **YCbCr**, not the YCgCo the DS-era ActImagine variants use.
- Context base is `ctx + 0xec` (`FUN_03000520` re-bases what the frame driver hands it).

## What is left

1. **Residual block damage in busy frames.** Quiet frames are clean; frames around 100 of stream 0
   still show block artifacts. Prediction, residual, MV prediction and references are all implemented,
   so the likely suspects are the chroma residual's placement within a quadrant (`_residual` in
   `vx_decode.py` maps quadrant `q` and CBP bit `b` to offsets — the luma mapping is confirmed against
   the ARM's `+4 / +0x3fc / +4` stepping, the chroma one is not) or a detail of a less-common mode.
   Start by isolating modes the way §20 did: replace one mode's output with flat grey and re-render.
2. **Quantiser per segment.** The driver reads the opening `ue(v)` of one segment. Decoding across a
   segment boundary needs the next segment's own quantiser (doc §13).
3. **Audio.** Untouched. The `.audio` blobs carry the trained codebooks; `libavcodec/vx_audio.c` in
   this tree already handles the DS-era format and is the obvious starting point.
4. **An ffmpeg decoder.** The eventual goal was `libavcodec/vx.c`. The Python is the reference.

## Traps worth remembering

- Do not put project inputs in `/tmp` — a previous session lost the codebook and streams that way, and
  only the ROM offsets written into the doc made them recoverable.
- Capstone stops at the first non-instruction word; `vx_dis.py` decodes word by word for that reason.
- `strb` truncates, it does not clamp.
- The midpoint fill takes its corner in a register, not by reading the byte back — it matters for
  mode 4, where the stored byte is truncated and the register value is not.
- Halving both sides only terminates on squares; degenerate blocks keep bisecting along the long side.
