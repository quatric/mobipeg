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
| Type-2 audio codec (decode loop) | **solved** — algorithm recovered, ported |
| Type-2 audio framing | **solved**, byte-exact on every audio resource |
| Type-2 audio context table | **solved**, verified bit-exact against hardware |

## Which GBA Video is which

Retail carts split into two unrelated lineages:

| Era | Size | Codec |
|-----|------|-------|
| **GCC** | 64 MB | ActImagine **VX** (`VXDS` magic) — see `libavcodec/vx.c` |
| **ADS** | 32 MB | Majesco in-house stack — this document |

Scan for the ASCII string `VXDS` to classify. Dragon Ball GT has none; it has
`SFCD` at offset 3720 (0xE88). `Shrek (USA) (Rev 6)` (64 MB) has *neither*
magic and remains unclassified.

Much of the No-Intro "Video" folder is homebrew (Sonic Boom, Super Mario
World episodes, Dinosaur Office, Eek! The Cat, Legend of Lofi, Night Trap) —
fan-made carts unrelated to either retail lineage.

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

### What is still wrong

Decoding jingle block 0 with its recovered table now produces a correct silent
lead-in (40 samples of exact zero, matching the jingle's fade-in) and a signal
at a plausible amplitude (sd 35 rather than the full-scale 100 of noise), but
the body still degrades. The context table is no longer the suspect - it is
verified - so a bug remains in the sample loop transcription. The emulator route
is now the way to find it: break on `0x03002b4c`, single-step the ARM loop, and
diff the struct against `ads_audio.py` sample by sample.

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
so the demuxer exposes both as options (`-frame_rate`, `-sample_rate`,
defaulting to 30 and 16384). Nothing records how many samples an audio block
carries either, so blocks are paced off the movie's own length - video frame
count over frame rate - since the two streams cover the same wall clock.

Two caveats carry over from the analysis above and are not implementation
bugs: video drifts because the predictor is still unknown, and audio degrades
because the sample loop is not yet an exact transcription. The container, the
LZMA layer, the colour transform and the audio context table are solid.

