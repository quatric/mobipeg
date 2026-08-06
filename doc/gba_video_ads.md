# ADS-era GBA Video (Majesco) — format documentation

Reference sample: `Game Boy Advance Video - Dragon Ball GT - Volume 1 (USA).gba`
(No-Intro), 32 MB, game code `MDBE`, internal title `DRAGONBALL01`.

Tooling: `tools/gba_video/ads_extract.py` (+ `ads_lzma.py`) implements
everything marked solved below and runs end to end on a retail ROM.

## Status

| Layer | State |
|---|---|
| SFCD archive | **solved**, validated |
| `.mmstr` resource container | **solved**, byte-exact on every file |
| Compression (LZMA) | **solved**, byte-exact consumption |
| Type-4 still images | **solved**, renders pixel-correct |
| Video stream framing (header/chapters/chunks) | **solved**, tiles exactly |
| Video frame geometry | **solved**, verified visually |
| Video output stage (clamp/brightness) | **solved** |
| Video codebook (index → 8 pixels) | **partial** — entry unit known, transform not |
| Type-2 audio | **unsolved** |

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

The decoder lives in IWRAM at `0x03001cd4` in `main.bin` — the function
abahbob reversed to 1123 lines of pseudocode believing it bespoke. It is
LZMA, hand-optimised for ARM, with the output position register-packed as
`pos << 15`. `FUN_03001334` is the streaming wrapper (circular dictionary).

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

The IWRAM entry points form the decoder's API, called from EWRAM through
ARM/THUMB veneers: `0x03001150` init, `0x030013b0` read size,
`0x030012e0` set source, `0x030011e0` set destination, `0x030013a8` set
size, `0x030011a8` run.

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
    u8  blobA[sizeA]        LZMA, decodes to 3072 bytes, always
    u8  blobB[sizeB]        LZMA, decodes to 4800 * frame_count bytes

Chunks tile the resource exactly, ending on the 4-byte terminator. This is
the "two objects called per frame" that abahbob observed.

### Frame geometry — solved

`blobB` yields exactly **4800 bytes per frame**, which is a **60 x 80 raster
of one-byte block indices**. Each block is **4 x 2 pixels**, so a frame is
**240 x 160** — the full GBA screen (and 240*160*2 / 4800 = 16, i.e. 8
pixels per index).

Confirmed visually: dumping `blobB` frames of `logo majesco.mmstr` as 60x80
greyscale shows the Majesco logo, blank on frame 0 and fading in through
frames 5–20, exactly as a logo intro should.

### Confirmed against the code

A second RE pass validated the container reading above directly against the
decompiled player rather than by inference from file layout:

- `FUN_02005110` is the generic resource decompressor and confirms the
  `.mmstr` entry header: size is `(*p & 0x0fffffff) * 4`, and the name is
  skipped with `strlen` rounded up to 4.
- `FUN_020040c4` walks chunks with exactly the arithmetic used here:
  `frames = w0 >> 16`, `next = cur + 8 + (w1 & 0x1fff)*4 + (w1 >> 13)*4`.
- `FUN_0200414a` parses chapter records as `u16` + string with record size
  `2 + align4(strlen+4)`, and resolves each `u16` by *walking chunks*,
  confirming chapter values are chunk indices.
- `FUN_0200434a` copies a 0x4c-byte header, reads the title with `strlen`,
  and takes the chapter table at `align4(len+4) + 0xc`.
- `FUN_02005484` fetches resource type **2** and type **1** for one movie,
  confirming 1 = video and 2 = audio.

Display is **BG Mode 3** — `FUN_020031fe` calls `FUN_02000350(3)` and stores
`0x06000000` — so the target is a 240x160 RGB555 framebuffer.

### Output stage — solved

`FUN_020042fc` builds, at video-object offset `0x34c`, a 256-entry table
`table[i] = min(255, ((base + brightness) * i) >> 8)`, with **0x200-byte
guard bands on both sides**: the low guard is filled with a constant and the
high guard with the table's last value. A table that is indexed from -512 to
+767 is a saturation table, so the final pixel stage is

    pixel = clamp_table[prediction + residual]

on 8-bit values, with the player's brightness setting folded into the curve.

### Codebook — partially characterised, transform still unsolved

`blobA` is a fixed 3072 bytes per chunk regardless of frame count, so it is
a per-chunk table, and it is what maps an index to its 8 pixels. What it is
*not*:

- not a codebook of raw RGB555 pixels — no entry stride (4/6/8/12/16/32)
  makes the block for index 49 uniform, yet frame 0 is uniformly index 49
  and displays as flat;
- not palette + palette-index codebook — no 512-byte window in the blob
  looks like an RGB555 palette (88–126 of 256 entries have bit 15 set, where
  a real palette would have ~0).

- not raw luma in any block ordering — rendering bytes 0–7 as the eight
  pixels (row-major 4-across, or as two 2x2 halves, row- or column-major,
  signed or unsigned) always leaves a period-2 checkerboard, and the flat
  background never resolves;
- not stored planar either — with `entry_component[k][i] = A[k*256 + i]`,
  **0 of 256** entries have a near-flat first eight values, versus 1 of 256
  for the interleaved `A[i*12 .. i*12+12]` layout. Since frame 0 is
  uniformly index 49 and displays flat, a correct pixel interpretation must
  make entry 49 flat; none does.

What it looks like instead: signed residual data. Byte values cluster near 0
and 0xFF/0xFE/0xFD (entry 49 is `fd ff 04 fd c0 38 08 03 ff 00 01 ff`, i.e.
−3, −1, +4, −3, −64, +56, +8, +3, −1, 0, +1, −1), the three 1024-byte thirds
have signed means of −4.3, +4.5 and −0.00, and the zero-byte histogram has
period 4 (three near-identical 4-byte groups per 12 bytes). Supporting this,
`FUN_020042fc`
in EWRAM builds a 256-entry clamp/brightness LUT,
`table[i] = min(255, (brightness*i) >> 8)`, flanked by 0x200-byte saturation
guards on both sides — the signature of a pipeline that produces
out-of-range values needing clamping. The player's brightness setting
(±2 steps, exposed in the menu) feeds this table.

So `blobA` is a transform-domain / residual table, not a lookup table of
pixels, and reconstruction must apply a transform (and probably a
prediction) rather than a copy. Consecutive frames share 85.7% of their
index bytes, so there is strong temporal structure as well.

**The entry unit is six little-endian `u16`, not twelve bytes.** Across the
whole 3072-byte blob the byte statistics have period 2 — even bytes average
~115 with few zeros, odd bytes average ~86 with roughly three times as many
zeros — and this holds at *every* stride tested, so it is a property of the
stream itself and not of any assumed entry size. 3072 bytes is therefore
1536 `u16` values, i.e. 256 entries x 6 values. Six values for an eight-pixel
block rules out any one-value-per-pixel reading and is consistent with the
residual/transform conclusion above; two of the eight samples would be
predicted rather than coded. Read as signed, entries mix small values with
large ones (index 55 is `221, 21, -10195, -10457, 209, 28`).

**First real pixel output.** Rendering the logo with entries indexed at
`index * 12` and the block laid out 4x2 produces a cleanly recognisable
Majesco logo at correct aspect — the first time this format has produced
actual pixels rather than an index map. The geometry (60x80 raster, 4x2
blocks, 240x160 frame) is therefore confirmed from rendered output, not just
from a greyscale dump of the index plane. The colours are wrong and a
period-2 checkerboard remains, which is exactly what the `u16` finding
predicts: consecutive bytes are halves of one value, not two pixels.

Finishing this needs the block reconstruction routine itself, which has not
been located yet. It is not among the 14 IWRAM functions (those are the LZMA
core and wrapper, the font blitter, a UI box drawer, and the decoder API
entry points), so it is expected to be in EWRAM, reached through the video
object's vtable at `0x02008374`. Decoding that vtable's signed offsets gives
candidate methods at `0x02003422` (constructor, confirmed), `0x02003690`,
`0x02003444`, `0x02004498`, `0x02005b6a`, `0x02005fa8`, `0x02001276` and
`0x02001638`; several were not auto-detected as functions by Ghidra and need
`create_function` before they will decompile. That is the next step.

## Memory map of `main.bin` (needed for any further RE)

`main.bin` carries two images at different bases, which is why a flat Ghidra
import stalls. Split at file offset **0x8C00**:

    main.bin[0x0000 : 0x8C00]  -> 0x02000000  (EWRAM)
    main.bin[0x8C00 : 0xBA34]  -> 0x03000000  (IWRAM, 0x2E34 bytes)

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
