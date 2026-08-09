# ADS-era GBA Video (Majesco) — format documentation

Reference sample: `Game Boy Advance Video - Dragon Ball GT - Volume 1 (USA).gba`
(No-Intro), 32 MB, game code `MDBE`, internal title `DRAGONBALL01`.

Playback is native: ffmpeg reads a retail cartridge directly via the
`gbavideo_rom` demuxer (SFCD archive included), or an unpacked `.mmstr` via
`gbavideo`. The Python under `tools/gba_video/` is reverse-engineering
scaffolding, not part of the decode path: `ads_extract.py` (+ `ads_lzma.py`)
unpacks a ROM for inspection, `ads_audio.py` is the ported audio decode loop,
`ads_ctab.py` the context-table decoder and `gdbrsp.py` the mGBA GDB client.

## Status

| Layer | State |
|---|---|
| SFCD archive | **solved**, validated |
| `.mmstr` resource container | **solved**, byte-exact on every file |
| Compression (LZMA) | **solved**, byte-exact consumption |
| SFCD archive (ROM → resources) | **solved**, demuxed natively from the cartridge |
| Type-4 still images | **solved**, renders pixel-correct |
| Video stream framing (header/chapters/chunks) | **solved**, tiles exactly |
| Video frame geometry | **solved**, verified visually |
| Video output stage (clamp/brightness) | **solved** |
| Video codebook layout + colour transform | **solved** — recovered from the ROM |
| Video prediction (residual → pixels) | **solved** — both logo and movie streams render clean |
| Type-2 audio codec (decode loop) | **solved** — decodes clean end to end |
| Type-2 audio framing | **solved**, byte-exact on every audio resource |
| Type-2 audio context table | **solved**, verified bit-exact against hardware |

## Which GBA Video is which

Retail carts encountered so far split into five lineages or revisions:

| Lineage | Size | Codec |
|---------|------|-------|
| **ADS** | 32 MB | Majesco in-house stack using LZMA — this document |
| **Hydrogen** | 32 MB | Majesco derivative using Inflate — this document |
| **VXGB** | 64 MB | Earlier ActImagine GBA revision — native, hardware-exact decoder |
| **VX++** | 64 MB | ActImagine GBA revision — native, hardware-exact decoder; see `gba_video_vxpp.md` |
| **FVMV** | 32 MB | Nintendo / Pokemon stack — complete offline decoder; see `gba_video_fvmv.md` |

`VXDS` belongs to the later DS container and is not a reliable classifier for
these cartridges. Dragon Ball GT has an `SFCD` archive at offset 3720 (0xE88);
Dora is recognized by its Hydrogen resources; and the ActImagine carts carry
literal **`VXGB`** or **`VX++`** stream headers. See the sections at the end of
this document and `gba_video_vxpp.md`. The Nintendo-published Pokemon carts use
literal **`FVMV`** headers and a separate player described in
`gba_video_fvmv.md`.

Much of the No-Intro "Video" folder is homebrew (Sonic Boom, Super Mario
World episodes, Dinosaur Office, Eek! The Cat, Legend of Lofi, Night Trap) —
fan-made carts unrelated to the retail lineages.

## Container chain

    ROM
     +- bootstrap code   3668 bytes
     +- SFCD archive     @ 0xE88
         +- movie_1.mmstr, movie_2.mmstr, jingle.mmstr,
            menuresources.mmstr, commonresources.mmstr,
            logo majesco.mmstr, main.bin

`SFCD`: 4-byte magic, `u16 data_offset` (+8), `u16 data_length`,
`u32 entry_count`, then per entry `u32 size`, `u32 offset`, NUL-terminated
name padded to 4 bytes.

### `.mmstr`

    u32 total_size          (equals the file length)
    u32 entry_count         (only the low byte is used)
    repeat:
        u32 size_words:28 | type:4
        u8  data[size_words * 4]

There is no name field in the entry header and no entry table — the next
entry sits at `data + size_words*4`. Every retail `.mmstr` tiles to exactly
its file length, which is what confirms the layout.

Resource types: **1 = video, 2 = audio, 4 = image, 5 = text**.

### Obfuscated string encoding

Names and on-screen text use a substitution alphabet, not ASCII:
`1..10` → digits, `11..36` → lowercase, `37..62` → uppercase, `0xFF` → space,
`0xFE` → newline, plus a punctuation table (see `ads_extract.py`). Text
resources embed their strings this way, which is why a naive parse of
`movie_1.mmstr` appears to yield a filename `credits_ep1` — that is payload
content of a type-5 resource, not an entry name.

## Compression: LZMA

Every compressed blob is stock **LZMA with lc=0, lp=0, pb=2**, no end
marker, prefixed by 8 bytes:

    u32 uncompressed_size
    u32 params              (0x00011002 / 0x00011402 / 0x00011802 observed;
                             low byte 0x02 == pb, remainder not yet decoded —
                             decoding does not depend on it)

The decoder lives in IWRAM at `0x03001ca4` in `main.bin` — the function
abahbob reversed to 1123 lines of pseudocode believing it bespoke. It is
LZMA, hand-optimised for ARM, with the output position register-packed as
`pos << 15`. `FUN_03001304` is the streaming wrapper (circular dictionary).

Range-coder tells: `bound = (range >> 11) * prob`; `prob += (2048-prob) >> 5`
/ `prob -= prob >> 5`; renormalise below `1 << 24`; rep0–3 rotation; length
choice → choice2 → low(8)/mid(8)/high(256); 6-bit posSlot with a `< 14`
split; 4-bit reverse align tree; matched literals.

The probability model matches the canonical layout **byte for byte**:

| segment | offset | | segment | offset |
|---|---|---|---|---|
| IsMatch | 0x000 | | PosSlot | 0x360 (0x80 stride) |
| IsRep | 0x180 | | SpecPos | 0x560 (indexed base-1) |
| IsRepG0 | 0x198 | | Align | 0x644 |
| IsRepG1 | 0x1b0 | | LenCoder | 0x664 (choice2 0x666, high 0x868) |
| IsRepG2 | 0x1c8 | | RepLenCoder | 0xa68 (choice2 0xa6a, high 0xc6c) |
| IsRep0Long | 0x1e0 | | Literal | 0xe6c |

1846 probs precede the literal coder — exactly canonical.

The IWRAM entry points form the decoder's API: `0x03001150` init,
`0x030013b0` read size, `0x030012e0` set source, `0x030011e0` set
destination, `0x030013a8` set size, `0x030011a8` run. They are reached
through the **IWRAM vtable at `0x03002B28`**, whose nine entries are signed
offsets relative to the vtable base (one entry, `-0x1001CD7`, resolves into
THUMB EWRAM at `0x02000E51`). The veneer block at `0x03002AE0..0x03002B24`
runs the other way -- six `LDR R12,[PC,#0]; BX R12` stubs into EWRAM, the
last of which is `strlen` at `0x0200720D`.

## Type-4 images — fully solved

    u32 width:12 | height:12 | flags:8      (flags = 0x17 on every sample)
    obfuscated name, NUL-terminated, padded to align4(len+4)
    LZMA blob -> width * height * 2 bytes of raw RGB555

Verified: every image in the reference ROM decodes with
`declared_size == width*height*2` and renders correctly (the Game Boy Player
logo, the Dragon Ball GT menu screens, font sheets at 1472x16 and 736x8).

## Type-1 video

### Stream header

    u32  chapter_count:8 | frame_count:16 | 0x20:8
    u32  magic 0x0a03c0c7   (identical in every video resource seen)
    u32  chunk_count
    obfuscated title, NUL-terminated, padded to align4(len+4)
    chapter_count records:
        u16 chunk_index
        obfuscated label, NUL-terminated
        record size = 2 + align4(strlen+4)
    (align to 4)  -> chunk stream
    ... chunks ...
    u32 0x00000000                          terminator

Frame counts read straight out of the header and match an independent chunk
walk exactly: logo 59, jingle 793, movie_1 15733, movie_2 13770. At ~22
minutes per episode that is ~12 fps. Chapter `u16` values are **chunk**
indices (movie_1: 1, 60, 158, 208, 262, 316 against 379 chunks).

### Chunks

    u32 w0      frame_count = w0 >> 16      (low half is a constant per file)
    u32 w1      sizeA = (w1 & 0x1FFF) * 4
                sizeB = (w1 >> 13)   * 4
    u8  blobA[sizeA]        LZMA, the chunk's codebook
    u8  blobB[sizeB]        LZMA, the chunk's index planes

blobA is **not** a fixed 3072 bytes -- that is only true of streams using 4x2
blocks. It is always 256 entries; the entry size follows the block geometry
(3072 = 256x12 for the logo, 4608 = 256x18 for `movie_1`). Likewise blobB is
`grid_w * grid_h` bytes per frame, which is 4800 for the logo but 3180 for the
movies. The player decompresses blobA straight into a fixed **6144-byte**
codebook slot (`slot_base + stream_index * 6144`), 6144 being the largest
geometry's 256 x 24.

Chunks tile the resource exactly, ending on the 4-byte terminator. This is
the "two objects called per frame" that abahbob observed.

### Frame geometry, codebook layout and colour — solved

Frames are a raster of **one-byte block indices**, `grid_w x grid_h`, with
`grid_w` = 60 throughout. Block geometry is chosen by a **mode nibble**,
`video_obj[0x650 + stream_index*12] & 7`, and the same switch appears in every
IWRAM routine that walks pixels (`0x030001C8`, `0x030004F8`, `0x03000B24`):

| mode | block | luma/entry | chroma/entry | entry size |
|---|---|---|---|---|
| 0, 1 | 4x4 | 16 | 4 + 4 | 24 |
| 2, 3 | 4x3 | 12 | 3 + 3 | 18 |
| 4, 5 | 3x3 | 9 | 2 + 2 | 13 |
| 6, 7 | 4x2 | 8 | 2 + 2 | 12 |

So a **codebook entry is planar**: `blk_w*blk_h` luma bytes, then `blk_h` Cb,
then `blk_h` Cr. Chroma is subsampled **4:1 horizontally** -- exactly one Cb
and one Cr per block row. Every observation lines up: `logo majesco` is mode
6/7, 60x80 blocks of 4x2 = 240x160, 4800 index bytes and 256x12 = 3072;
`movie_1` is mode 2/3, 60x53 blocks of 4x3 = 240x159, 3180 index bytes and
256x18 = 4608. Dumping a `movie_1` index plane at 60x53 shows the episode's
title card cleanly, which confirms the movie geometry visually.

There is also a low-resolution path: `0x030001C8` blits a 60x40 image straight
to the framebuffer through a 256-entry `u16` LUT held in spare VRAM at
`0x06012C00`, and `0x03000B24` builds that downscaled plane by averaging each
block's luma and chroma. This is what all those `0x06012C00` literals are.

### Output stage and colour transform — solved exactly

`FUN_020042fc` builds the 256-entry brightness/clamp table at video-object
`+0x34c` with 0x200-byte guard bands on both sides, as previously documented.
The consumer is the IWRAM converter at **`0x030004F8`**, whose inner loop
(`0x030005B8`) reads three *planar* byte streams -- luma at `r1`, Cb at
`r1 + luma_plane_size`, Cr after that -- and emits BGR555:

    Y  = luma[i]                 (unsigned)
    Cb = cb[i]                   (SIGNED, ldrsb)
    Cr = cr[i]                   (SIGNED, ldrsb)

    R = five[ clamp[ Y + 2*Cr ] ]          -> bits 0-4
    G = five[ clamp[ Y - Cr - (Cb >> 1) ] ] -> bits 5-9
    B = five[ clamp[ Y + 2*Cb ] ]          -> bits 10-14

`clamp[]` is the guard-banded table at `obj+0x34c` (which is why it must accept
negative indices) and `five[]` is a second table at `obj+0x71c` reducing the
clamped 8-bit value to 5 bits. This replaces the BT.601-with-128-offset guess
that made earlier renders come out the wrong colour.

### Prediction — solved

The predictor was recovered not from Dragon Ball GT but from **Dora the
Explorer Volume 1** (`MDRE`, maker `5G`), a second ADS-era cart built on
Majesco's "Hydrogen Library". That build ships with asserts intact, so its
`__FUNCTION__` and `__FILE__` literals name the codec's routines outright:

    StreamBase::UnPredictLuminance(unsigned char*)     file 0x1ffb1ec
    StreamBase::UnPredictChrominance(unsigned char*)   file 0x1ffb2f0
    StreamBase::DecompressCodebook()
    hyCompressionManager::Inflate::CoreExpand_Static()

The codec overlay is uncompressed ARM at the tail of the ROM (up to file
0x1ffc66c) and runs from IWRAM around `0x03002xxx`, the same arrangement as
Dragon Ball GT. Its block-geometry table at ROM `0xea70` is byte-identical to
the one recovered from Dragon Ball GT, which is what justifies carrying the
result across to the other cart.

Both routines are the same loop, unrolled four ways: a **running sum modulo
256** over a byte range, with no clamping.

    acc = 0
    for each byte p:  p = (p + acc) & 0xff;  acc = p

The decisive detail is the length. It is `count * blk_w * blk_h` where
`count = (desc[0] >> 22) & 0x3ff`, and ten bits cannot hold a frame's 4800
blocks -- but they hold 256 exactly. **The prediction is over the codebook,
not over the frame.** `UnPredictChrominance` confirms the layout: it starts at
`base + count * blk_w * blk_h`, so a codebook is planar *across* entries:

    [256 * blk_w*blk_h luma][256 * crows Cb][256 * crows Cr]

not planar within each entry as previously documented. Cb and Cr are summed as
one contiguous run, Cb flowing into Cr. `crows` comes from the mode's low bit:
odd modes carry one Cb/Cr per block row, even modes a single pair per block --
a distinction the earlier write-up missed because both Dragon Ball GT streams
are odd modes.

**Frames are intra.** Once the codebook has been un-delta'd its entries are
absolute pixels, so a frame is a plain lookup with no temporal prediction at
all. There is no accumulator, and no keyframe/interframe distinction.

Three of the old assumptions were wrong at once: prediction was applied to the
frame rather than the codebook, the codebook was read with the wrong layout,
and there was a temporal accumulator that does not exist. The accumulator went
unnoticed for so long because the Majesco logo *opens on a run of uniform white
frames* -- frame 0 is 4800 copies of one entry whose luma is -1 everywhere, and
frames 0 and 1 have identical index planes -- so summing deltas happened to
approximate the right answer there while destroying every movie frame. Movie
streams are the case that settles it: their indices change completely between
frames and their common entries are large positive values (+10, +20), which
saturate within a few frames if accumulated.

The mode nibble is not something to infer: it sits in the low bits of every
chunk header's first word (`0x4037` for `logo majesco`, mode 7 = 4x2;
`0x4033` for `movie_1`, mode 3 = 4x3).

With all of this the **Majesco logo stream renders cleanly end to end** -- the
fade-in, the correct blue, the "TM", and the "DIGITAL" tagline strip -- and
reading the cartridge directly and reading an unpacked `.mmstr` give
byte-identical frames.

Both streams now decode clean end to end: the logo fades in correctly and the
movie renders sharp footage (title cards are legible) for its whole length. A
small residual remains -- colour speckle at the left and right edges of the
logo's "DIGITAL VIDEO" tagline strip -- which is the only known artifact left.

### Chunk driver

`0x0200457C` is the per-chunk driver, and it confirms the chunk arithmetic from
the code: `sizeA = (w1 & 0x1FFF) * 4` via `lsl #0x13; lsr #0x11`, and
`sizeB = (w1 >> 13) * 4`. `0x020046FA` is the codebook loader, a two-state
machine: state 1 LZMA-decodes blobA into `codebook_base + idx*6144`, state 2
pushes 6144 bytes from IWRAM `0x03002FCC` with **DMA3** (`DMA3SAD 0x040000D4`,
control `0x84000600` = enable, 32-bit, 1536 words).

### SFCD archive

Resources live in an SFCD archive, so a cartridge can be demuxed without
unpacking it first:

    'SFCD' | uint16 data_off | .. | uint32 count
    count * { uint32 size, uint32 offset, NUL-terminated name, pad to 4 }
    file data at sfcd + data_off + 8 + offset

Names here are plain, unlike the substitution-alphabet names inside a `.mmstr`.
The `gbavideo_rom` demuxer scans the first megabyte for the magic (it sits at
0xE88 in Dragon Ball GT), lists the members at `-v verbose`, and plays the
largest one unless `-resource <name>` picks another; the suffix is optional, so
`-resource jingle` finds `jingle.mmstr`. Reading the cartridge this way gives
byte-identical frames to reading an extracted `.mmstr`.

## Type-2 audio

The audio codec's decode loop is an **uncatalogued ARM routine in IWRAM at
`0x03002B4C`** (0x03002B7C under the old, wrong split), with a second entry
point at `0x03002DA4` that presets the bit count to 32 and falls into the same
loop. It sits in the gap after the LZMA core and was missed on the first pass
because Ghidra did not auto-detect it; `create_function` at `0x03002B4C`
recovers all 348 bytes. The 8-entry VLC table it indexes is at **`0x03002DB8`**
(`01 01 01 01 12 12 23 33`, low nibble = bit length, `byte >> 3` = the shift
applied to `ctx_table[ctx]`); the literal at `0x03002DE4` points at it, which
is one of the proofs of the corrected base.

`tools/gba_video/ads_audio.py` is a direct Python port of this loop.

It is an **adaptive predictive 8-bit PCM codec**, not ADPCM in the IMA sense.
Per output sample:

    code   = VLC, 1-3 bits, via an 8-entry table indexed by the top 3 bits
             of the bit accumulator: lengths {1,1,1,1,2,2,3,3}, selector
             {0,0,0,0,1,1,2,3}
    sym    = (ctx_table[ctx] >> (2 * selector)) & 3
    ctx    = sym | (ctx & 0x0f) << 2          -- a 6-bit shift register of
                                                 the last three symbols
    step   = adapted from sym bit 0, clamped into [0x220, 0x1400] or
             [.., 0x3c00] depending on branch
    mag    = ((step & 0x7f | 0x80) << 7) >> (14 - (step >> 7 & 0xf))
             negated when sym bit 1 is set
    pred   = ((h2 * (f1 >> 2)) >> 11) + ((h1 * (f0 >> 2)) >> 11)
             + ((prev * f2) >> 11), then >> 1
    out    = clamp(round(mag + pred), -128, +127)      -- signed 8-bit

Three filter coefficients (`f0,f1,f2` at struct offsets 0x10/0x14/0x18) leak
toward zero every sample (`f -= f >> 8`, `f -= f >> 7`) and are nudged by the
sign history, so the predictor adapts continuously. Two one-byte sign
histories live at struct offsets 0x7c and 0x7d. Signed 8-bit output is
exactly what GBA DirectSound wants.

Decoder context struct (built by the not-yet-located init):

    0x00 step        0x04 prev       0x08,0x0c predictor history
    0x10,0x14,0x18 adaptive filter coefficients
    0x1c bit accumulator             0x20 bits available
    0x24,0x28 pending word           0x2c input pointer (word-stepped)
    0x34 samples remaining           0x38 context index
    0x3c ctx_table[64]               0x7c,0x7d sign histories

### Framing — solved

A type-2 resource is:

    u32  unknown (0x00000000 / low byte then a signature-ish word)
    u32  unknown
    u16  0x000C                 header size
    u16  block_count
    u16  block_size[block_count]     -- in WORDS, not bytes
    (pad to 4)
    u8   payload[]

Block sizes are **word counts**. This is exact, not inferred:
`sum(block_size) * 4` equals `resource_length - align4(0x10 + 2*block_count)`
byte for byte on all three audio resources in the reference ROM --
`logo majesco` (5 blocks, 16756 bytes), `jingle` (66 blocks, 245784) and
`movie_1` (1311 blocks, 4757496).

### The context table — still open

`ctx_table` is **not** a constant in `main.bin`: a search for 64 consecutive
bytes drawn from the 24 permutation-of-{0,1,2,3} values finds nothing.

It is **not in the stream header either.** An earlier revision proposed a
64-byte region at offset 0x40 of a type-2 resource, whose repetitive
`aa aa aa aa 02 00 08 aa ...` shape looked like a 2-bit-packed table. With the
framing above that offset falls *inside audio block 0* (payload starts at 0x1C
for that resource, block 0 runs 712 words), so it is coded audio, not a table
-- and its regularity is just what silence looks like at the head of the logo
jingle. That candidate is refuted.

Running the ported loop over correctly-framed payload with a constant
`ctx_table` yields full-scale noise for all 256 constants (mean absolute
sample-to-sample delta ~96-115 out of 255; a smoothness sweep finds no
constant that stands out). A constant table was never plausible anyway -- the
table's whole purpose is to reorder four symbols *per context*. So the table
is built by an initialisation routine that has not been located, and finding
it is the one remaining blocker on audio.

## Memory map of `main.bin` (needed for any further RE)

`main.bin` carries two images at different bases, which is why a flat Ghidra
import stalls. Split at file offset **0x8C30**:

    main.bin[0x0000 : 0x8C30]  -> 0x02000000  (EWRAM)
    main.bin[0x8C30 : 0xBA34]  -> 0x03000000  (IWRAM, 0x2E04 bytes)
    (BSS)                      -> 0x03002E04, 0x1B00 bytes, zero-filled

This is not inferred — a **copy-descriptor table sits at file offset 0x8C14**
and states it outright: `{src 0x02008C30, dst 0x03000000, len 0x2E04}` followed
by `{0, 0x03002E04, 0x1B00}` (the BSS clear). Independently, 0x03000000 then
starts on `push {r4,r5,lr}`, and the literal at 0x03002DE4 resolves to the
audio VLC table at 0x03002DB8 only under this base.

**An earlier revision of this document used 0x8C00, so every IWRAM address in
it was 0x30 too high.** Corrected anchors: LZMA core `0x03001CA4` (was quoted
as 0x03001CD4), ARM/THUMB veneers `0x03002AE0..0x03002B24`, audio decode loop
`0x03002B4C` (was 0x03002B7C). The decoder API entry points were already
correct because they were read out of the IWRAM vtable at `0x03002B28` as
offsets relative to the vtable base.

Import the tail as raw `ARM:LE:32:v4t` at `0x03000000`; it disassembles into
14 clean functions. Confirmations: the image length equals the highest IWRAM
address referenced from EWRAM (`0x03002e34`); the veneers at
`0x03002b10`..`0x03002b4c` are `LDR R12,[PC,#0]; BX R12` and their stored
targets are all real EWRAM functions; and the IWRAM font blitter calls
`0x03002b4c`, which lands on EWRAM `0x0200720c`, which EWRAM itself uses as
`strlen`.

ADS C++ vtables store **signed offsets from the vtable address**, not
absolute pointers (the video player's vtable is at `0x02008374`; entry 0 is
`-20305` → `0x02003423`, the constructor, with the THUMB bit set).

## Relationship to Gericom's "Majesco" code

`LibMobiclip/Codec/Majesco/MajescoInflater.cs` (MobiclipDecoder, commit
c88b67d) implements US Patent 7353233 — assigned to Majesco Entertainment,
inventor Alexandre Ganca — a DEFLATE-shaped Huffman scheme, and it is
unfinished (`Inflate()` returns `null`). **That is a different algorithm
from what this ROM runs.** No DEFLATE-style Huffman decoder appears anywhere
in the IWRAM image. `libavcodec/majesco.c` is an unverified port of the
patent scheme and is not on any decode path.

### Bit-reader corrections (2026-08-05, later pass)

Three transcription errors in the port of the IWRAM reader at `0x03002d50`
have been fixed in `tools/gba_video/ads_audio.py`:

* **Shift by 32.** ARM register-specified shifts of 32 produce 0. The first
  refill (`nbits == 0`) does `LSL pend, word, #32` and must yield 0, not the
  word itself.
* **Refill fall-through.** When `pendn + nbits <= 32` the ROM does *not*
  return: it falls through from `0x03002d84` into `0x03002d88` and pulls a
  fresh word, merging it at the new bit position. The earlier port returned
  early and desynchronised after the first partial refill.
* **Range test.** `0x03002cd4`-`0x03002ce0` builds `0x3fffffff`
  (`mov fp,#64,#24` then `sub fp,#1`) and compares `r4 + 0x1fffffff` against
  it; the earlier port used `0x1fff`/`0x3fff`.

These are real fixes against the disassembly, but they do **not** yet yield
audio: with any constant context table the output is still full-scale noise
(mean |x| near 100, high-frequency/total ratio ~1.35, i.e. white).

### Two negative results that narrow the search

* **No 64-byte permutation table exists anywhere in the 32 MB ROM.** Every
  byte of a rank-to-symbol table should be one of the 24 permutations of
  `{0,1,2,3}`; scanning the whole cartridge for a 64-byte run drawn only from
  those 24 values returns nothing. The table is therefore computed, not
  stored, or the entries are not bijective.
* **Nothing in `main.bin` points at `0x03002b4c`** - not as a word, not as a
  half-word, and no `BL`/`BLX` from either image reaches it. The same is true
  of the video converter at `0x030004f8`. EWRAM refers to IWRAM *data*
  (`0x03002dc0`-`0x03002e0c`, 66 literals) but almost never to IWRAM *code*.
  So both IWRAM entry points are reached through a dispatch mechanism that has
  not been identified, and finding it is now the single blocker shared by both
  the audio context table and the video reconstruction routine.

A coordinate-descent search over the 24 permutations for all 64 contexts,
scored on spectral tilt, starts at 1.391 from the identity table `0xe4` and
converges at 1.278 - still noise. That the search cannot do better is evidence
the remaining error is *not* in the context table.

### The dispatch is not statically resolvable

Following the "nothing points at `0x03002b4c`" result to the end, the search was
widened until it was exhaustive, and every avenue is negative:

* No word or half-word equal to the target anywhere in the **32 MB cartridge**,
  not just in `main.bin`.
* No `BL`/`BLX` reaches it. Brute-forcing every 4-byte-aligned ARM `BL` in the
  IWRAM image and every THUMB `BL`/`BLX` half-word pair in the EWRAM image (not
  relying on linear disassembly alignment) finds **zero EWRAM-to-IWRAM control
  transfers of any kind**, to any address.
* No self-relative offset entry: for no word `v` at address `a` does `a + v`,
  `a + v + 8`, or `0x03000000 + v` equal any codec entry point.
* No `ADD/SUB Rd, PC, #imm` forms the address. The 121 hits are all
  `ADD PC, PC, #imm` inside the jump-threaded LZMA region
  (`0x03001d50`-`0x030026xx`); none targets the codec.

Two things were confirmed in passing. The IWRAM base is right: the only
internal `BL` targets are the veneers at `0x03002ae0`-`0x03002b1c` and the LZMA
core at `0x03001ca4`, all landing exactly on function starts, and
`0x8c30 + 0x2e04` is precisely the length of `main.bin`. And the EWRAM base is
right for an independent reason - `main.bin` carries its **own GBA header** with
a zeroed title (it diverges from the cartridge at offset 0xa0, so it is a
multiboot-style module, not the cartridge prefix), whose entry branch and
`0x08000200` pointer at 0x104 are consistent with the module being self-copied
to `0x02000000`. The one absolute IWRAM code pointer that does exist,
`0x03001654`, is the **IRQ handler** (it reads `spsr` and IE/IF at
`0x04000200`), installed into `0x03007ffc`.

So the codec entry points are reached through an address computed at runtime -
most likely an `ADD PC, PC, Rx` style dispatch or a pointer table built by the
IRQ/streaming layer. Static analysis cannot recover it. **The next step is
dynamic:** run the ROM under mGBA with a breakpoint on `0x03002b4c` and read
the caller off the stack, which also gives the live decoder struct and therefore
the 64-byte context table at `obj+0x3c` directly, without having to find the
routine that builds it. mGBA 0.10.5 is installed but its Lua console is
GUI-only, so this needs an interactive session.

## The audio context table - SOLVED, verified bit-exact

Going dynamic worked immediately. mGBA's GDB stub (`mGBA -g`, port 2345) plus a
~60-line RSP client (`tools/gba_video/gdbrsp.py`) answered in one breakpoint what
static analysis could not.

Breaking on `0x03002b4c` stops with `lr = 0x02004f25` and `r0 = 0x030047d0`: the
decoder **is** called, from EWRAM, and the struct lives in IWRAM BSS. The call
does not appear statically because it is an indirect `blx` through a pointer in
a structure.

A **write watchpoint** on `obj+0x3c` then caught the builder in one step:
`0x02005046`, inside a 64-iteration loop at `0x02004fe0`-`0x020050f8`.

### The table is transmitted in the bitstream

It is neither a constant nor adaptive: each block codes its own 64 entries with
a tiny VLC over a 7-value palette, all at `main.bin` 0x8a04:

    palette  d8 72 e4 4e 78 d2 e1          (0x02008a04, 7 permutation bytes)
    prefix   00    -> palette[0] = 0xd8
             010   -> palette[1] = 0x72
             011   -> palette[2] = 0xe4
             1000  -> palette[3] = 0x4e
             1001  -> palette[4] = 0x78
             1010  -> palette[5] = 0xd2
             1011  -> palette[6] = 0xe1
             11    -> escape

Every palette entry is a permutation of `{0,1,2,3}` packed 2 bits each, which is
why the earlier cartridge-wide scan for a *stored* 64-byte permutation table
found nothing - only this 7-byte palette exists.

The escape (`0x02005062`) reads two 2-bit fields `a`, `b` and one selector bit -
**seven bits total**, not eleven - then indexes `ESC = [0c 0d 08 06]`
(`0x02008a00`) with a *one-hot* value:

    mask = (1 << a) | (1 << b)
    lo, hi = ESC[mask & 3], ESC[mask >> 2]
    f2, f3 = (hi >> 2, lo & 3) if sel else (lo & 3, hi >> 2)
    entry  = a | (b << 2) | (f2 << 4) | (f3 << 6)

Building `mask` one-hot from the two fields is the step that makes the escape
work; reading the fields as raw bits desynchronises the rest of the table.

### Verification

A matched pair was captured in one session - reader state at builder entry
(`acc`, `nbits`, `pend`, `pendn`, `ptr`) and the finished table at
`0x020050f8`. Reconstructing the bit stream and running `ads_ctab.decode_ctab`
reproduces all **64/64 bytes exactly**. Two framing facts fell out and are now
confirmed dynamically rather than inferred:

* `(word_at_0x94 << 25) & 0xffffffff` equals the live `acc`, so the payload
  really does start at 0x94 and the bit model - **LE 32-bit words, MSB-first
  within the word** - is right.
* Each block begins with a short preamble (25 bits in the case measured)
  *before* the context table, which is why decoding from the payload start
  never aligned.

So a block is `[preamble][64 VLC-coded context entries][samples]`.

### The sample loop — solved by the other cart (2026-08-06)

The loop degraded after a correct silent lead-in, and the bug was found not by
stepping Dragon Ball GT but by reading Dora's copy of the same routine, which
the Hydrogen cart keeps as plain ARM (see below). Three corrections came out of
it, and all three matter:

  * **the `f1` update saturates at ±0x4000, not at ±2^29.** `0x03002c58` builds
    the bound as `mov fp, #64, #24` — that is `64 << 8`, so the test is
    `(unsigned)(t + 0x1fff) <= 0x3fff` and the ±0x100/0xff clamp is reached
    constantly. An earlier pass read this as `0x1fffffff/0x3fffffff`, which
    made the clamp dead code; that is what let the signal run away.
  * **a block's 25-bit preamble is not padding.** It is one flag bit plus, when
    that bit is clear, a **signed 24-bit initial level** for the log-domain
    state. Set means the block continues the previous one instead.
  * **the reset is not to zero.** `prev` and `h0` start at 1, and `f1`/`h1`
    survive across blocks — only `step`, `prev`, `h0`, `f0`, `f2` and the two
    sign bits are reset.

With those, both carts decode to a clean waveform end to end.

## Native support in mobipeg

The format is now implemented in C, not just in the Python tools:

| file | what it does |
| --- | --- |
| `libavformat/gbavideo.c` | `gbavideo` demuxer - reads a `.mmstr`, exposes the type-1 video and type-2 audio resources as streams |
| `libavcodec/adslzma.c` | the LZMA variant (lc=0, lp=0, pb=2, 8-byte size/params prefix) |
| `libavcodec/adsgbavideo.c` | `ads_gba` video decoder |
| `libavcodec/adsgbaaudio.c` | `ads_gba_audio` audio decoder |

Pull the `.mmstr` files out of a cartridge with
`tools/gba_video/ads_extract.py rom.gba outdir`, then:

    ffmpeg -i "logo majesco.mmstr" -map 0:v -fps_mode passthrough out%03d.png

One chunk is one video packet; because a chunk carries several frames the
decoder uses a receive_frame callback and spaces the timestamps itself. Block
geometry is derived from the size of a frame's index plane rather than the mode
nibble - the grid is always 60 wide and a frame 240 wide, so `blk_w` is 4 and
`blk_h` follows from the row count (80 rows -> 4x2 for the logo, 53 -> 4x3 and
240x159 for the movies).

Neither a frame rate nor a sample rate is recorded anywhere in the container,
so the demuxer exposes both as options (`-frame_rate`, `-sample_rate`, with a
16384 Hz audio default). The shared ADS/Hydrogen player decodes exactly 0x404a
(16458) samples per complete audio block. The block count therefore gives the
movie's duration and lets the demuxer derive the nominal 12 fps video rate.
Using a video-length average for ADS blocks is incorrect: it truncates about
67 samples from every Dragon Ball GT block and produces an audible gap roughly
once per second.

The GBA display itself runs at about 59.7275 Hz, and these movies advance one
frame every five refreshes (about 11.9455 fps). Exports that need tight A/V
alignment can retain the fixed audio clock and use that hardware cadence for
the video timestamps.



## The Hydrogen lineage (Dora the Explorer) — solved

Dragon Ball GT is not the only ADS-era stack. **Dora the Explorer Volume 1**
(`MDRE`, maker `5G`, 32 MB) is a third lineage, built on Majesco's **Hydrogen
Library**, and it is the cart that gave up the video predictor above. It has
**no SFCD and no VXDS**, and it compresses with `hyCompressionManager::Inflate`
-- the US Patent 7353233 Huffman scheme -- rather than LZMA. So
`libavcodec/majesco.c`, previously recorded as being on no decode path, is the
right decompressor for *this* cart; it is only a partial port, because the C#
prototype it came from never reversed the literal/length/distance loop.

Dora ships that loop as uncompressed ARM at the ROM tail with asserts intact,
so it can be read directly. What is established so far:

**Layout.** The codec overlay occupies file `0x1ff8000`..`0x1ffc66c` and runs
from IWRAM around `0x03002xxx`. Functions of interest:

| file offset | routine |
|---|---|
| `0x1ff96d8` | `Inflate::Initialize(const void*, void*, bool)` |
| `0x1ff9788` | `Inflate::CoreExpand_Static()` |
| `0x1ff997c` | fixed-Huffman table builder |
| `0x1ff9ad4` | stored-block copy |
| `0x1ff9c0c` | `Inflate::UncompressBlock(unsigned long)` |
| `0x1ffc388` | read-n-bits helper |

**Stream header.** `[uint32 uncompressed_size]` then the coded data as
**little-endian halfwords**, starting at `src + 4`.

**Bit reader.** A 32-bit accumulator held **MSB-justified**, refilled 16 bits
at a time: when `nbits < n`, `acc |= halfword << (16 - nbits); nbits += 16`.
Reading is `result = acc >> (32 - n); acc <<= n; nbits -= n`. Note this is
MSB-first, the opposite of stock DEFLATE.

**Object layout.** `+0x04` flag, `+0x08` output cursor, `+0x0c` bit
accumulator, `+0x10` bit count, `+0x14` halfword pointer, `+0x18` table
descriptor, `+0x38` bytes produced, `+0x3c` state, `+0x40` output base,
`+0x44` output end, `+0x48` bytes remaining.

**State machine.** `+0x3c` selects one of eight states through a jump table at
`0x1ff9cc0`. State 0 reads a 2-bit block type and dispatches: `0` -> stored
(state 5), `1` -> fixed (state 2), `2` -> dynamic (state 1), `3` -> state 7 --
a fourth block type that DEFLATE does not have.

**Symbol decode.** A 9-bit primary lookup indexes a halfword table; an entry is
`length << 9 | symbol`. Symbols below `0x100` are literals, `0x100` ends the
block, and above that the symbol indexes an 8-byte-per-entry table holding
`{dist_base, dist_extra, len_base, len_extra}` -- exactly the interleaved
layout already sitting unused in `majesco_dist_len_table`. In the static path
the distance code is a fixed 5 bits.

Structurally it is **DEFLATE** -- same block types, same code-length alphabet
and permutation, same length and distance ladders -- with exactly two
deviations: bits are read **most significant first**, and a blob carries a
plain `uint32` size instead of a zlib header. `libavcodec/majesco.c` implements
this and is verified against every chunk in the cart: 187 codebooks and 187
index planes across nine streams decompress to their declared sizes with no
failures. The fourth block type is never used by any of them.

### Container

Dora has no archive and no resource directory has been found -- everything past
the string block is compressed, and no pointer table turned up. But the video
streams themselves sit in the ROM **uncompressed**, in the same chunk format as
Dragon Ball GT, and every chunk header states its own length. So the demuxer
finds them by walking: take the first header that validates, follow the chain to
its end, and that span is one stream. Streams are laid out back to back, so the
scan stays linear. Nine streams turn up, the largest 91 chunks and 7216 frames.
`-resource <n>` selects one by index.

### What this cart forced fixing

Dora uses mode 5, which Dragon Ball GT never does, and it exposed two bugs in
the shared video path:

  * the grid is **not always 60 blocks wide** -- it is `240 / blk_w`, so mode 5
    (3x3) is an 80x53 grid, not 60-wide;
  * chroma is **not simply one sample per block row for odd modes**. The switch
    in `UnPredictChrominance` gives `{1, 4, 1, 3, 1, 2, 1, 2}` per mode, and
    mode 5 keeps **two** Cb/Cr for a 3x3 block rather than three.

Both were invisible on Dragon Ball GT, whose streams only use modes 3 and 7.

### Audio — the same codec, and the key to the ADS one (2026-08-06)

`SoundPlayer_ADPCM.cpp` is a misleading name: Dora's audio is **not** ADPCM and
**not** a different codec. It is bit-for-bit the same adaptive predictive PCM
that Dragon Ball GT uses, down to the three-bit sample VLC table
(`{01 01 01 01 12 12 23 33}`) and the four-entry escape table `0c 0d 08 06`.
The single difference is how the 64-entry context table is coded: ADS uses a
VLC over a seven-value palette with `11` as an escape, Hydrogen always sends
the escape form, a flat **five bits** per entry (`a:2, b:2, selector:1`).

The reason this cart matters is that its decoder is readable. `SetSoundFile` is
plain THUMB in the ROM at `0x08006600`, the block loop is ARM in IWRAM at
`0x03002ad0` (reached through the veneer at `0x0800d420`), and the bit-reader
refill sits at `0x08005cb8`. Dumping IWRAM under mGBA and disassembling that
loop is what fixed the shared sample loop; see "The sample loop — solved by the
other cart" above.

**Block length is a constant.** `0x08005f54` reloads the sample counter with
`0x404a`, so a block is exactly **16458 samples** — which is also what the
block-size table in the audio resource covers, to within the word padding.
Since nothing in the container states a frame rate, that constant dates the
movie: `frames * sample_rate / (16458 * nb_blocks)` gives **12 fps** for every
Dora stream.

### Container — Dora does have `.mmstr` after all

The earlier conclusion that this cart has no resource directory was wrong. The
game's own lookup is at `0x08005c82` and it walks an ordinary `.mmstr` table.
Two things hid it:

  * a Hydrogen resource states its size **in bytes**, where an ADS one states
    it in words;
  * the resource count is the low **nibble** of the count word, not the low
    byte, and the rest of that word is junk.

So the ROM demuxer now scans for `.mmstr` tables instead of chasing loose chunk
chains. A candidate is accepted only when its audio resource's block table adds
up to the resource size exactly, which is an arithmetic identity over several
thousand bytes and does not fire on unrelated data. Dora yields **four**
movies, not the nine the chunk-walk reported — six of those nine were false
starts inside two long streams, which also showed the walk's `nb <= 300`
frames-per-chunk guard was too tight.

    [0] 0x109b8    51 KB    splash, 59 frames,  5 blocks
    [1] 0x20ca4    15.9 MB  15964 frames,    1331 blocks (22 min)
    [2] 0xf4390c   16.7 MB  1339 blocks
    [3] 0x1f3216c  685 KB   502 frames,        42 blocks

`-resource <n>` picks one by index; the default is the largest. A movie with no
chapters carries no title string either, so its chunks start straight after the
three header words.


## The VX lineage on GBA (Shrek + Shark Tale) — container solved

`Game Boy Advance Video - Shrek + Shark Tale (USA) (Rev 5)`, 64 MB, game code
`MSTE`, maker `5G`. A third stack: no `SFCD`, no `.mmstr`, no Hydrogen asserts.
Its container magic is **`VX++`** — ActImagine, but neither the DS `.vx`
(`VXDS`) container nor either Majesco stack.

(An earlier note here called the tag `VX++42`. That was a misread: `42` is the
first two bytes of the pointer `0x02003234` that follows it in the literal
pool, and that pointer is what makes the tag certain — it addresses the string
`File Format Error`, so `VX++` is what the player compares a stream against.)

### Where the code lives

Both RAM images are straight copies out of the ROM, which is what makes static
work possible: **EWRAM `0x02000000` ← ROM `0x8a00`** (`0x32c8` bytes) and
**IWRAM `0x03000000` ← ROM `0xbcc8`**. Under those bases the player's strings
resolve — `File Format Error` is EWRAM `0x02003234`, `Unable to init YUV
conversion code` is `0x02003248` — and both are referenced from literal pools
in the IWRAM image, so the codec and its error handling are IWRAM code.

### Header

Four streams, all at `0x200`-aligned offsets. The header is `0x38` bytes:

| off | field |
|---|---|
| `+00` | `'VX++'` |
| `+04` | frame count |
| `+08` | width |
| `+0c` | height |
| `+10` | frame rate, `0x30c3` on every stream = **12.19 fps** (value / 1024) |
| `+14` | quantizer — **0**, so it is not the DS quantizer field |
| `+18` | sample rate, 16384 |
| `+1c` | audio offset |
| `+20` | chapter table offset |
| `+24` | first frame of the last chapter |
| `+28` | seek table offset |
| `+2c` | seek table entries |
| `+30` | same as `+24` |
| `+34` | unknown (33259 / 32540; zero on the short streams) |

All offsets are relative to the stream. The chapter table is
`{uint32 count, uint32 first_frame[count]}` and ends the stream. The seek table
is four words per entry: **`{frame, video bit offset, audio byte offset, 0}`**.

That middle column is the important one. Its last entry reaches
180,659,843 against a video region of 180,665,920 bits, and the audio column
reaches 9,713,678 against an audio region of 9,719,808 bytes — both land within
a rounding of the end, which is what identifies them. So **the video is one
continuous bitstream with no per-frame sizes**, bit-addressed; there is nothing
to walk, which is why every framing hypothesis tried against this cart failed.
The audio region opens with **3124 bytes** of extradata — exactly the DS
`AudioExtraData` block (3·64·8 int16 codebooks, 8 uint16 scale modifiers,
8 int32 LPC bases, 1 uint32 initial scale), byte-identical between streams —
and the first seek entry's audio column is `3124`, i.e. the audio data proper.

    [0] 0x00020200  240x112  34874 frames  47:40  20 chapters
    [1] 0x01eef800  240x112  34261 frames  46:50  20 chapters
    [2] 0x03dd3600  240x160    595 frames   0:48   1 chapter
    [3] 0x03e76a00  240x160    707 frames   0:57   1 chapter

`tools/gba_video/gbavx_extract.py` lists and unpacks all of this.

### The bitstream is not the DS bitstream

`libavcodec/vx.c` cannot decode frame 0. It was tried at both plausible data
starts, under all four byte orders (raw, swapped 16, swapped 32, swapped 32
then 16 — the decoder itself applies a `bswap16`, so those cover every
combination), and across every legal quantizer 12..161: 600 attempts, no
frame. That is consistent with what the header already says — the DS decoder
takes its quantizer from the container and starts straight into macroblocks,
whereas here `+14` is zero and every frame opens with a near-constant halfword
(`0x84dd` on the 240x112 streams, `0x84dc` on the 240x160 ones) that the DS
format has no place for. This is an earlier generation of the codec and needs
its own reverse engineering, from the IWRAM code at ROM `0xbcc8`.

### The audio is the DS codec, and it works

The audio needed no decoder work at all. `libavcodec/vx_audio.c` already has a
mode where the packet carries no leading video bits — `width == 0` skips the
video replay, which is how `.mods` is handled — so `libavformat/gbavx.c` just
declares that shape and hands over the cart's own 3124-byte codebook block.

The one thing the demuxer has to do is cut packets on AFrame boundaries, and
that is free: an AFrame is 32 header bits plus 16 per pulse word, so it is
always a whole number of 16-bit words, and its length is stated by its own
first two words (`4 + 2 * {8,5,4,3}[(word2 >> 12) & 3]` bytes). Walking that
over the reference cart tiles all 9,719,808 bytes of the audio region exactly,
with nothing left over, and lands on **every one of the 181 audio offsets the
seek table states** — 181 independent confirmations that the walk is right.

That walk is also what dates the movies. Between consecutive seek entries the
ratio is 128 AFrames per 7 video frames, everywhere along the stream; at 128
samples per AFrame and 16384 Hz that is exactly **7 fps**, not the 12.19 fps a
naive reading of `+10` suggests. Which makes stream 0 run 1:23:02 rather than
47 minutes — a feature-length film, as it should be. So `+10` is *not* a frame
rate; it is 0x30c3 on every stream and remains unidentified.

All four streams decode end to end with no errors:

    ffmpeg -f gbavx -i rom.gba out.wav          # the longest movie
    ffmpeg -f gbavx -resource 2 -i rom.gba ...  # pick one by index

The chapter table comes out as real chapters, and the stream duration comes
from the frame count at 7 fps.

### The video bitstream — structure recovered

The decoder is IWRAM code, and the way in was a **write watchpoint**. Break the
YUV blitter at `0x03000370` (the only `bl` to it is at `0x03007cc8`), read its
argument — a three-word struct `{Y plane, UV plane, VRAM}` — then set a write
watchpoint inside that Y plane. The next time the rotating buffer comes back
round, the watchpoint fires *inside the decoder*, at `0x0300190c`. Everything
below follows from disassembling outward from there.

**Frame buffers.** Four of them, each a 256-stride Y plane of 0xa000 bytes
followed by an **interleaved UV** plane of 0x5000 (`0x020302c8` / `0x0203a2c8`
in one run). So 4:2:0 with U and V byte-interleaved, both planes at stride 256.

**Bit reader** (`0x030007a8`). A 32-bit MSB-justified accumulator refilled
**16 bits at a time from little-endian halfwords** — the same shape as the
Hydrogen Inflate reader. Everything is Exp-Golomb:

  * `0x0300076c` — **ue(v)**: count leading zeros `n`, take `n` more bits,
    value = `(1 << n) + suffix - 1`;
  * `0x0300081c` — **se(v)**: same, then the H.264 zig-zag mapping
    (`k` odd → `(1-k)>>1`, else `k>>1`).

There is no CAVLC anywhere, which is the clean break from the DS codec.

**Motion vectors** are predicted by the **component-wise median of three
neighbours** — left, above, above-right — exactly as the DS decoder's
`mid_pred3` does (`0x03000548`). They are stored packed as two `int8` in a
halfword, in a per-row array of stride 0x24, and are **full-pel**: the luma
byte offset is `mvx + mvy*256`, and chroma is `(mvx & ~1) + (mvy & ~1)*128`.

**The inter macroblock** (`0x03001854`) reads **five se(v) values**: `mvx`,
`mvy`, and then a DC offset for Y, U and V. It copies **16 luma pixels wide by
H rows** from reference + MV, adding `2*dcY` to every byte with a clamp to
0..255, then the matching 8 chroma pairs by H/2 rows with `2*dcU` / `2*dcV`.
That is the whole inter path: full-pel copy plus a per-plane DC correction, no
transform.

**Intra prediction** is dispatched through two function-pointer tables, both
indexed by a ue(v):

    0x03000864  6 entries   0x30009b8 0x3000a04 0x3000a54 0x3000ba4 0x3000c08 0x3000d90
    0x030008d8  9 entries   0x3000ecc 0x3000eec 0x3000f90 0x3000fe0 0x30010c4
                            0x30011ac 0x30012b0 0x30013b4 0x30014b0

and they are H.264-shaped: `0x030009b8` replicates the row above down the block
(vertical), `0x03000a04` replicates the left column (horizontal), and the
nine-entry set is the same idea at 4x4. `0x03000884` reads two ue(v) and calls
one handler from each table, so a macroblock's two halves take independent
modes. Six macroblock-level handlers sit between `0x03001b00` and `0x03002900`,
each calling the same five workers (`0x03001578`, `0x030015a4`, `0x03001854`,
`0x03000884`, `0x030008fc`).

**Output** (`0x03000370`) converts YUV to BGR555 through three lookup tables
and writes VRAM at stride 480, centred for the movie's geometry. Its width and
height are **self-modified into the code** as `mov` immediates by the open
routine at `0x03006df8`, which is also where the `VX++` tag is checked.

**How the bitstream reaches the decoder.** A 64 MB cartridge does not fit the
GBA bus, so the cart pages ROM into a **4 KB window at `0x08001000`** and the
decoder reads its bits straight from there (`r1 = 0x08001096` when the
watchpoint fired); the reader tests the pointer against the window end after
every refill and kicks a DMA when it crosses. That is also why the open routine
appears to compare `VX++` against `0x08001000` — it is comparing against the
window, not a fixed ROM address.

This was the initial static picture. The frame grammar, all sixteen recursive
dispatchers, proprietary residual VLC, reconstruction and native decoder are
now complete; see `gba_video_vxpp.md` for the hardware-validated result.

### Driving the cart under mGBA

Worth recording, because the obvious routes do not work: synthetic keystrokes
never reach the emulator (`KEYINPUT` stays `0x3ff`), and `KEYINPUT` will not
take a `M` packet write. What works is intercepting the game's own read:

  1. launch `mGBA -g` and **continue first** — the stub boots halted, so a scan
     before the first `c` sees a dead machine at `pc = 0`;
  2. find the key poll by searching EWRAM for `b0 30 d3 e1` (`ldrh r3, [r3]`,
     `r3 = 0x04000130`); it lands at `0x020003c0`, though not until the cart is
     a few seconds past boot;
  3. breakpoint the *next* instruction and set `r3` to `0x3ff ^ key` with a
     `P3=` packet — the following `eor r3, r1, r3` (r1 is `0x3ff`) yields the
     press. Four hits per press, then drop the breakpoint.

`Start, A, A, Start, A, A` reaches playback. Note the stub wedges if a script
exits while the target is halted, so restart mGBA per attempt and do everything
in one script.
