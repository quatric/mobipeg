# Caimans GBA Video — handoff

State as of 2026-08-09. This is the next non-retail GBA video family to
reverse after the five Game Boy Advance Video lineages. It is a separate
commercial codec, not an ADS, Hydrogen, VX, or FVMV variant. No Caimans
demuxer or decoder has been added to FFmpeg yet.

## Repository state

- Repository: `/Volumes/SSD/dlz/Folders/mobipeg`
- Branch: `master`
- Current commit: `c1b360dcb14` (`Revert "avformat/moflex: default mo_block to 2048"`)
- The GBA Video GUI listing was committed earlier as `d78a14d24ac` and is an
  ancestor of the current branch.
- The 32 KiB IWRAM dump the Ghidra analysis is based on now lives at
  `build_caimans/caimans_iwram2.bin` (it was previously only in a session
  scratchpad). Load it as raw binary, `ARM:LE:32:v4t`, image base
  `0x03000000`.
- Preserve unrelated dirty files: `checkasm_config_generated.h`,
  `checkasm_header_config_generated.h`, `mobipeg-gui.spec`, and the untracked
  `tools/mobiclip/` material.

## Reference ROMs

Inputs are intentionally untracked in `build_caimans/`; re-fetch them from the
publisher rather than committing them.

| generation | sample | local ROM | SHA-256 | publisher archive |
|---|---|---|---|---|
| Caimans 2.2 | Bad Boys 2 trailer | `build_caimans/roms/caimans_badboys2.gba` | `7f6b2a01cb0ba2328ef01bfb067b9b75c021f7db081cc450cae69c0966a39a1d` | `https://www.caimans.net/gbavideo/caimans_badboys2_hi.zip` |
| Caimans Pro | Pooh's Heffalump Movie trailer | `build_caimans/roms/caimans_pro_pooh_hq.gba` | `e81ed048fe1c07e9dd00cf9d0814808dea5386b2861fdbb3ae728fbc551679f4` | `https://www.caimans.net/gbavideo/caimans_pro_pooh_hq.zip` |

The archives were downloaded directly from the publisher's demo pages:

- `https://www.caimans.net/gbavideo/demos2.shtml` (version 2.2)
- `https://www.caimans.net/gbavideo/demos.shtml` (Pro)
- `https://www.caimans.net/gbavideo/tech.shtml` (publisher's technical claims)

## What is established

- The two samples are distinct player/data generations. Do not build a shared
  parser merely because both are Caimans.
- Version 2.2's ROM has no ordinary title string in the GBA header; Pro uses
  the title `CAIMANSH` at ROM `0xA0`.
- Both boot from Thumb code at ROM `0x08000100`.
- In the Pro sample, after startup the IWRAM trampoline at `0x03000000` is ARM
  `LDR r0,[pc]; BX r0` and transfers to `0x08000219` (Thumb). This gives a
  stable debugger waypoint and confirms that the player is not one of the
  existing cartridge decoder images.
- Tracing forward from `0x08000218` (Thumb: `push {lr}; ldr r3,[pc,#12];
  bl ...; bx r3; pop {r0}; bx r0`), control immediately leaves ROM and runs
  from IWRAM: `0x03006630`→`0x03004a98`→`0x03004ad4` in the first ~60
  instructions. The player's hot code is already resident in IWRAM
  (`0x03004a00`–`0x03006690`+ observed so far) by the time the ROM's own
  Thumb entry point runs, so it was copied there by the cartridge header /
  BIOS-adjacent boot stub, not decompressed on demand. Reproduced with
  `mGBA -g`, breaking first at `0x03000000` (ARM, ordinary breakpoint) then
  at `0x08000218` (Thumb, kind=2 breakpoint over the RSP `Z0` packet — the
  kind byte matters, an ARM-mode breakpoint will not fire on a Thumb
  address), then single-stepping with `s`.
### The IWRAM player is a verbatim uncompressed copy of a ROM region

The resident IWRAM image is **not** decompressed or relocated — it is a
straight `memcpy` from ROM. Established by dumping live IWRAM and searching
the ROM for the dumped bytes; every probe landed at a constant delta.

```
IWRAM 0x03000000  <->  ROM file offset 0x4F6C  (ROM address 0x08004F6C)
rom_file_offset = (iwram_addr - 0x03000000) + 0x4F6C
```

One contiguous 28024-byte run covers the whole live code region:

| IWRAM | ROM address |
|---|---|
| `0x030002d8`–`0x03007050` | `0x08005244`–`0x0800bfbc` |

This means **the player can be analysed statically from the ROM alone** — no
emulator needed to recover code. For correct branch targets, import the IWRAM
dump (or that ROM slice) at base `0x03000000` as `ARM:LE:32:v4t`.

### Audio is IMA ADPCM with a modified index table (Pro sample)

The routine at IWRAM `0x03004a98` is the audio decoder, recovered via Ghidra.
Its prologue clamps a value to `[0, 0x58]` (0–88) and indexes a 16-bit table —
the IMA ADPCM step-index idiom. Confirmed by reading the tables directly:

| item | IWRAM address | finding |
|---|---|---|
| step table | `0x030004d8` | 89 × `int16`, **byte-identical to FFmpeg's `ff_adpcm_step_table`** |
| index-adjust table | `0x0300058a` | 16 × `int8` = `{-1,-1,-1,-1,2,4,7,12}` (×2) |
| codec state | `0x03007050` | 2 × `int32`, loaded via `ldmia r8,{r12,lr}` = predictor, step index |

The index table lands exactly where the step table ends
(`0x030004d8 + 89*2 == 0x0300058a`), confirming the layout.

Decompiling the routine gives the complete sample kernel. Signature is
`(byte *src, int8 *dst, u16 nsamples)`. Per sample: clamp step index to
`[0, 88]`, look up `step`, take a nibble, then

```c
diff = step >> 3;
if (mag & 1) diff += step >> 2;
if (mag & 2) diff += step >> 1;
if (mag & 4) diff += step;
if (mag == 7) diff += step >> 1;   /* deviation (1) */
if (nibble & 8) diff = -diff;
predictor = clamp(predictor + diff, -0x8000, 0x7fff);
step_index += index_table[mag];    /* deviation (2) */
*dst++ = predictor >> 8;           /* signed 8-bit output */
```

**Two deviations from standard IMA, both required for bit-exactness:**

1. The extra `diff += step >> 1` when the low three magnitude bits are all
   set (`mag == 7`). Standard IMA has no such term.
2. The index-adjust table is `{-1,-1,-1,-1,2,4,7,12}`, not FFmpeg's
   canonical `ff_adpcm_index_table` `{-1,-1,-1,-1,2,4,6,8}`. A grep of
   `libavcodec/` found no existing FFmpeg IMA variant using `2,4,7,12`.

Consequently an `ADPCM_IMA_*` decoder **cannot** be reused as-is; Caimans
needs its own kernel. The 89-entry step table *can* be reused verbatim.

Other notes: the magnitude index is `nibble & 7`, so only the low 8 entries
of the 16-entry index table are ever reached. Output is signed **8-bit** PCM
(only the predictor's high byte is stored), matching GBA DMA audio. Nibble
order is low-then-high, keyed on the parity of the remaining-sample
countdown — which implies `nsamples` is always even, since an odd count
would make the first sample read a stale byte latch.

`tools/gba_video/caimans_adpcm.py` is a direct transcription of this kernel.

So the publisher's "proprietary compressed audio" claim is, for the Pro
sample, IMA ADPCM with one extra reconstruction term and one altered table.
Note this was established for **Pro only** — the 2.2 sample has not been
checked and must not be assumed to match.

Still unverified: block/packet framing, channel count, and how the codec
state is initialised per stream. Until those are known the kernel above
cannot be pointed at a raw file, so it is **not yet validated against real
audio** — only against the disassembly it was transcribed from.

### Pro video is an H.263 derivative (strong evidence, not yet decoded)

`FUN_03005a00` (IWRAM `0x03005a00`, the largest routine at ~3.3 KB) is the
picture-header parser. It drives three bitreader callbacks held in the
literal pool, all of which match functions Ghidra independently discovered:

| literal | value | role |
|---|---|---|
| `DAT_030065e0` | `0x03003ec4` | `get_bits(n)` (called with 0x16, 0x10, 0xc) |
| `DAT_030065e4` | `0x03003e3c` | `get_bits(n)` (called with 8, 4, 3, 2) |
| `DAT_030065e8` | `0x03003f68` | `get_bit()` — no args, returns 0/1 |
| `DAT_030065ec` | `0x030070b4` | frame-size table |
| `DAT_030065dc` | `0x03000024` | bit-position / state struct |

Two independent things identify this as H.263-derived:

**1. The frame-size table** at `0x030070b4` is indexed by a 3-bit code, with
code 7 escaping to explicit 12-bit width and height — H.263's `source_format`
field exactly. Its contents:

| code | size | |
|---:|---|---|
| 0 | 160x120 | custom |
| 1 | 128x96 | **SQCIF** (H.263) |
| 2 | 176x144 | **QCIF** (H.263) |
| 3 | 352x288 | **CIF** (H.263) |
| 4 | 704x576 | **4CIF** (H.263) |
| 5 | 240x180 | custom |
| 6 | 320x240 | custom (QVGA) |
| 7 | escape | explicit 12-bit w/h |

Codes 1–4 are bit-for-bit H.263's standard source formats in their standard
slots. Caimans replaced H.263's 16CIF and reserved slots (5, 6) with sizes
that suit the GBA's 240x160 screen.

**2. The picture start code.** The routine opens with a 22-bit read — H.263's
PSC width — and rejects anything outside `{0x20,0x30,0x40,0x50,0x60,0x70}`.
`0x20` is the standard 22-bit H.263 PSC. When the value is *not* `0x20`, the
parser first descrambles the next 32 bytes in place:

```c
for (i = 0; i < 4; i++)
    buf[i] = rot16(buf[i]) ^ buf[7 - i];   /* rot16 = swap halves */
```

i.e. the non-`0x20` start codes select Caimans' proprietary obfuscation
layer over an otherwise H.263-shaped header.

### Macroblock layer: the MV VLC is bit-for-bit H.263

`FUN_03005848` (IWRAM `0x03005848`) is the motion-vector decoder, and it
confirms the H.263 relationship below the header.

Structure: a 2-iteration loop (MV x then y) that reads one VLC per
component, applies **median-of-three prediction** from neighbouring MVs, and
sign-extends the result with `(v << 26) >> 26` — a **6-bit** wrap to
`[-32, +31]`, exactly H.263's half-pel MV range and wraparound rule.

The VLC uses a two-tier flattened lookup (magnitude and length byte tables,
selected on whether the peeked window is below `0x6000000`):

| literal | value | role |
|---|---|---|
| `DAT_030059fc` / `DAT_030059f8` | `0x03001c20` / `0x03001bc0` | magnitude / length, 12-bit index |
| `DAT_030059f4` / `DAT_030059f0` | `0x03001cc0` / `0x03001c80` | magnitude / length, 7-bit index |
| `DAT_030059ec` | `0x03000024` | bitreader state |

Reconstructing the codeword for every magnitude from those tables and
diffing against `ff_mvtab` in `libavcodec/h263data.c` gives an **exact match
on all 33 entries** — same codewords, same prefix lengths, in the same
magnitude order. Magnitude 0 is handled by a short-circuit (`if the peeked
window's MSB is set: value 0, consume 1 bit`), which is `ff_mvtab[0] =
{code 1, 1 bit}`. The stored length includes the trailing sign bit, so the
VLC prefix is `length - 1`, matching FFmpeg's `put_bits(len + 1,
(code << 1) | sign)` on the encoder side.

### But the texture layer is *not* H.263 — Pro is a hybrid

Following the coefficient path revised the picture, so don't read the above
as "Pro is H.263".

`FUN_03004cd4` is the block/texture layer, and it does not look like H.263's
8x8 DCT + CBP at all. It dispatches through three function pointers selected
by a **block size of 4 or 8** (`DAT_03005844`, `DAT_03005820`,
`DAT_030057e0`), passing `(dst, count, value, stride)` with stride `0xf0`
= 240 — the GBA screen width. Alongside it, it maintains a coded/dirty block
map at **4x4 granularity** with a 0x40 (64-entry) row stride.

Two checks confirm the absence of a DCT:

- **No 8x8 integer IDCT exists in the player.** The classic W1–W7 constant
  set (2841, 2676, 2408, 1609, 1108, 565) appears **zero** times in the
  32 KiB IWRAM image. (These constants do appear in the raw ROM, but only at
  chance frequency — any given 16-bit value occurs ~26 times by accident in
  1.7 MB, and the observed counts are 15–47, so those are noise, not code.)
- The block dispatch is size-4/size-8 with a 4x4 coded-block map, which is
  the shape described by the independent notes ("recursive 8x8/8x4/4x4/4x2
  blocks with motion compensation") — and is *not* H.263, which has only
  16x16 and 8x8.

**Revised conclusion.** Pro is a hybrid: H.263's picture header and motion
vector syntax (the latter bit-exact) bolted onto a proprietary,
non-DCT texture layer, all behind an obfuscation layer on the non-`0x20`
start codes. So FFmpeg's H.263 machinery can be reused for **header and MV
parsing only** — `ff_mvtab` and the median-prediction/wraparound logic carry
over directly — while the texture layer needs a decoder written from
scratch. An earlier revision of this document said the port should reuse
`ituh263dec.c` wholesale; that was premature and is wrong.

### The three block callbacks are flat fills (one of several leaf codings)

Note these three callbacks are only the *flat-fill* leaf path. The richer
leaf coding — a sum of codebook patterns — is inline in `FUN_03004cd4` and is
described under "Leaf modes 1–6" below.

### The three block callbacks (detail)

All three resolve to real functions and all three do the same thing at
different sizes: replicate an 8-bit value across a word
(`v |= v<<8; v |= v<<16`) and store it over a rectangle, with the `count`
argument selecting full or half height.

| callback | function | width | heights | shapes |
|---|---|---:|---|---|
| `DAT_03005844` | `FUN_03004438` | 4 px | 2 or 4 | 4x2, 4x4 |
| `DAT_03005820` | `FUN_03004468` | 8 px | 4 or 8 | 8x4, 8x8 |
| `DAT_030057e0` | `FUN_030044c4` | 16 px | 8 or 16 | 16x8, 16x16 |

So the texture layer is a **recursive subdivision into solid-colour blocks**:
16x16 → 16x8 → 8x8 → 8x4 → 4x4 → 4x2, each leaf painted a single 8-bit
value. That extends the independent notes' "8x8/8x4/4x4/4x2" description
upward with a 16x16/16x8 tier they did not mention. It also explains the
absence of any IDCT: there are no transform coefficients to invert, only
block values.

Combined with the 4x4-granularity coded-block map and the `0xf0` (240)
stride, the natural reading is that these write directly into a **GBA Mode 4
framebuffer** (240x160, 8bpp paletted) — consistent with the publisher's
"runtime dithering" claim, since 8bpp indexed output from higher-precision
internal colour is exactly what needs dithering. Treat the Mode 4
identification as inference, though: it follows from the stride and pixel
size, and has not been confirmed against an actual `DISPCNT` write.

### Motion compensation is H.263-style bilinear half-pel

The inter path is a separate family from the fills, and it is textbook
H.263. All of it operates SWAR-style on 4 packed 8-bit pixels per word.

| function | operation | width | notes |
|---|---|---:|---|
| `FUN_0300429c` | full-pel copy | 16 px | unrolled; separate aligned / unaligned paths on `ptr & 3` |
| `FUN_03004568` | horizontal half-pel | 8 px | 8 rows |
| `FUN_03004608` | vertical half-pel | 4 px | 4 rows |
| `FUN_03004718` | diagonal half-pel | 4 px | row count in `param_4` |

The two-tap cases use the rounding-average idiom

```c
avg = (a | b) - (((a ^ b) & 0xfefefefe) >> 1);   /* = (a + b + 1) >> 1 */
```

with the `0xfefefefe` mask preventing cross-byte borrow. The horizontal
variant forms its second operand as `(byte)src[1] << 24 | src[0] >> 8`, i.e.
the source shifted one pixel right.

The diagonal case computes the 4-tap `(a + b + c + d + 2) / 4` by splitting
each word into its low two bits (`& 0x03030303`) and high six bits
(`& 0xfcfcfcfc) >> 2`), summing the parts separately and recombining — the
standard trick for averaging four packed bytes without overflow. The
`+ 0x02020202` is the per-byte rounding term.

Rounding-up averages and `(a+b+c+d+2)/4` are precisely H.263's half-pel
interpolation. So **prediction is H.263 through and through** — header, MV
coding, and now interpolation — and only the texture layer is proprietary.

### The subdivision is an alternating binary split tree

`FUN_03004cd4`'s outer loop is the subdivision walker, and it is a **bintree**
(alternating horizontal/vertical binary splits), not a quadtree. A level
counter starts at 5 and counts down; each node reads **one split-flag bit**
via `DAT_030057c0` → `FUN_03003f68` (the 1-bit reader). On a split, the two
children are placed at `offset` and `offset + delta`, where

```c
shift = (level >> 1) + 1;
delta = (level & 1) ? (stride << shift)   /* vertical: split top/bottom */
                    : (1 << shift);       /* horizontal: split left/right */
```

Node offsets live in the array at `DAT_030057bc` (`0x030000d0`). Working the
levels down gives exactly the six fill shapes, and the two per-level size
tables at `DAT_030057c8` (`0x030000b4`, height) and `DAT_030057cc`
(`0x030000bc`, width) confirm it independently:

| level | split | width | height | leaf shape |
|---:|---|---:|---:|---|
| 5 | vertical, dy=8 | — | — | 16x16 (root) |
| 4 | horizontal, dx=8 | 16 | 8 | 16x8 |
| 3 | vertical, dy=4 | 8 | 8 | 8x8 |
| 2 | horizontal, dx=4 | 8 | 4 | 8x4 |
| 1 | vertical, dy=2 | 4 | 4 | 4x4 |
| 0 | — | 4 | 2 | 4x2 |

The width column selects which fill callback runs (4/8/16), and the height
column is passed as the callback's `count`. So a 16x16 macroblock is
recursively bisected, alternating axis each level, down to a minimum of 4x2.

### Leaf coding: a mode VLC, then a value VLC

`FUN_03004cd4`'s fifth argument set is `(base, stride, intra, x, y)`. **`param_3`
is the intra flag**: `!= 0` means write absolute pixels, `== 0` means add a
residual on top of whatever prediction is already in the buffer. Confirmed at
the three call sites in `FUN_03005a00` — `0x03005cfc` passes `r2 = 1` with no
motion compensation ahead of it, while `0x03006038` and `0x030062c8` pass
`r2 = 0` immediately after the MC copy loops.

Each leaf reads two VLCs. **Both VLCs come in an intra and an inter variant**,
and the earlier revision of this document had the two labelled backwards.

**1. Mode** — a per-level pair of parallel value/length byte tables indexed by
a peek of the bitstream.

| path | peek | per-level stride | value table | length table |
|---|---:|---:|---|---|
| intra (`param_3 != 0`) | 7 bits | `0x80` | `DAT_030057d0` = `0x030015c0` | `DAT_030057d4` = `0x030012c0` |
| inter (`param_3 == 0`) | 6 bits | `0x40` | `DAT_030057e4` = `0x03001a40` | `DAT_030057e8` = `0x030018c0` |

The four tables are one contiguous block, `0x030012c0 .. 0x03001bc0`, in
length/value pairs — which independently confirms both strides. Symbols are
`0..6` plus `-1`. All twelve tables reconstruct to clean prefix-free codes:

```
intra  lv0  0=1     2=010   1=011   6=0001  4=0010  3=0011  5=00000   -1=00001
       lv1  0=11    6=001   4=010   3=011   2=100   1=101   5=0000    -1=0001
       lv2  0=1     4=010   1=011   6=0001  5=0010  3=0011  2=00000   -1=00001
       lv3  0=1     1=01    6=0001  4=0010  3=0011  5=00001 2=000000  -1=000001
       lv4  0=1     1=01    6=001   5=00001 3=00010 2=00011 4=000000  -1=000001
       lv5  0=1     1=01    2=001   3=0001  6=00001 4=000001 5=0000000 -1=0000001
inter  lv0  -1=11   4=001   3=010   2=011   1=100   0=101   6=0000    5=0001
       lv1  (same as lv0)
       lv2  -1=1    1=010   0=011   4=0001  3=0010  2=0011  6=00000   5=00001
       lv3  (same as lv2)
       lv4  (same as lv2)
       lv5  -1=1    0=01    1=001   4=00001 3=00010 2=00011 6=000000  5=000001
```

**2. Value** — a second, deeply tiered VLC giving the block's DC term. The
inter path uses the six-tier set (`DAT_030057f0`/`f4`, `DAT_03005810`/`14`,
`DAT_03005824`/`28`, `DAT_03005834`/`38`, `DAT_0300583c`, `DAT_03005840`) and
reads its values as **signed** (`char`/`short`) — a residual DC. The intra path
uses `DAT_030057d8`/`dc` plus `DAT_03005818`/`1c`, `DAT_0300582c`/`30`,
`DAT_0300580c`, and reads them as **unsigned** bytes — an absolute 0..255
level. Neither set has been inverted to codewords yet (next-work item 2).

Mode `-1` **skips the value VLC entirely**: intra fills the leaf with 0, inter
leaves the prediction untouched (a skipped block). Mode `0` on the intra path
is the flat fill with the decoded value. When the stride is `0xf0` the fill
paths also mark the 4x4-granularity coded-block map at `DAT_030057ec`
(`0x03007060`, row stride `0x40`) — a dirty-block map, so untouched regions can
be skipped downstream.

### Leaf modes 1–6: additive codebook layers, not motion compensation

The earlier guess that modes `1..6` were the motion-compensated block types was
**wrong**. The mode symbol is a *count*: mode `N` means the leaf is the sum of
**N codebook patterns** plus a DC. (Mode `0` on the inter path falls into the
same code with `N = 0`, i.e. DC-only residual.) There is no MV and no
interpolation call anywhere in this path — motion compensation happens in the
caller, before `FUN_03004cd4` is invoked with `param_3 = 0`.

Setup, at `0x030050a0`:

```c
base = ((int32_t *)(intra ? 0x03001d20 : 0x03002640))[level];  /* codebook for this level */
*(uint32_t *)0x03000020 = base;          /* running pointer, DAT_03005800 */
bits = read_bits(N * 4);                 /* DAT_03005804 = FUN_03003ec4   */
for (i = 0; i < N; i++)                  /* index array at 0x030000c4     */
    idx[i] = (int16_t)(((bits >> (4*(N-1-i))) & 0xf) + i*16) << (level + 1);
dc = value - 0x80 * N;
```

So each layer `i` spends exactly **4 bits** selecting one of 16 patterns, and
layer `i` draws from pattern slots `16*i .. 16*i+15` — six disjoint banks of
16, 96 patterns per level.

Then, per 4-pixel word of the leaf, walking the block linearly:

```c
acc = dc replicated into two 16-bit lanes;      /* 0x0000ffff | 0xffff0000 */
if (!intra) acc += the four existing pixels, split even/odd bytes;
for (i = 0; i < N; i++)
    acc += (*(uint32_t *)(running + idx[i]*4)) ^ 0x80808080;   /* SWAR, 2 lanes */
clamp each lane to 0..255;                      /* the 0x7f007f00 saturate trick */
store; running += 4;
```

`running` advances one word per output word and is **never reset per row**, so
a pattern's `idx*4` byte offset is a whole-block stride, not a pixel offset.
That makes the pattern stride `(nib + 16i) << (level+1)` words — and
`1 << (level+1)` words is exactly one leaf's worth of pixels at every level
(level 0: 2 words = 4x2; level 1: 4 = 4x4; level 2: 8 = 8x4; level 3: 16 =
8x8). **Each codebook entry is one full block-sized pattern of signed 8-bit
samples**, stored two's-complement and converted to offset-128 by the
`^ 0x80808080`; the `- 0x80*N` in `dc` cancels the resulting `+128` per layer
exactly, so the net reconstruction is simply

```
out = clamp(pred + value + sum of the N pattern samples)
```

with `pred = 0` on the intra path.

Three independent facts confirm the 96-entry layout. The level-1 and level-0
intra codebooks are `0x03001d30` and `0x03002330`, `0x600` apart — 96 x 16
bytes for a 4x4 block. The inter level-1 and level-2 codebooks are `0x03003250`
and `0x03002650`, `0xc00` apart — 96 x 32 bytes for 8x4. And the level-0 intra
codebook at `0x03002330` runs to `0x03002630`, 96 x 8 bytes for 4x2, where the
hexdump shows the data stop and zero padding begin.

### The codebooks are a zero-mean multi-stage VQ

`tools/gba_video/caimans_codebooks.py` extracts all eight codebooks (four
intra, four inter) from the IWRAM dump and the ROM, and self-checks the layout
claims above. Running it confirms each of them:

```
intra level 0  0x03002330 + 0x0300  (iwram)     inter level 0  0x03003850 + 0x0300  (iwram)
intra level 1  0x03001d30 + 0x0600  (iwram)     inter level 1  0x03003250 + 0x0600  (iwram)
intra level 2  0x08001d6c + 0x0c00  (rom)       inter level 2  0x03002650 + 0x0c00  (iwram)
intra level 3  0x0800056c + 0x1800  (rom)       inter level 3  0x0800296c + 0x1800  (rom)
```

The three ROM codebooks **tile one contiguous run with no gap or overlap**,
`0x0800056c .. 0x0800416c` (intra level 3, then intra level 2, then inter
level 3), and the data visibly changes character at `0x0800416c`. Since the
sizes were derived independently — 96 patterns times the leaf's own pixel
count — three books tiling exactly is a strong check on both the pattern count
and the one-pattern-per-leaf claim. The level-0 intra codebook likewise ends
exactly where zero padding begins in the IWRAM image.

Two properties of the extracted data confirm the reconstruction formula:

- **Every layer is zero-mean.** Averaged over all 16 patterns, each layer's DC
  is 0.00 ± 0.06 at every level, on both paths. The codebooks carry no DC at
  all — it is entirely the job of the separate value VLC, exactly as
  `out = pred + value + Σ patterns` requires.
- **Amplitude falls monotonically with layer**, e.g. intra level 1 goes
  20.90 → 6.93 → 4.39 → 3.54 → 2.87 → 2.42 mean |sample| across layers 0–5.
  So this is **successive-refinement (multi-stage) VQ**: layer 0 is the coarse
  approximation and each further layer is a smaller correction. Mode `N` is
  simply how many refinement stages the encoder decided to spend, which is why
  the mode VLC's cheapest codes are the low-`N` ones.

Amplitude also falls with block size (intra level 0 averages 11.93, level 3
averages 1.75), which is what a texture codebook should do — larger leaves are
only chosen for flatter regions.

**Only levels 0–3 use this path.** Both pointer tables are exactly four words
long: `0x03001d20` holds `{0x03002330, 0x03001d30, 0x08001d6c, 0x0800056c}` and
`0x03002640` holds `{0x03003850, 0x03003250, 0x03002650, 0x0800296c}`, with
codebook data starting immediately after (at `0x03001d30` and `0x03002650`,
which are the tables' own level-1/level-2 entries). The level-2 and level-3
codebooks live in **ROM** at `0x08xxxxxx` rather than IWRAM, so they are absent
from the IWRAM dump and must be read out of the cartridge image. Levels 4 and 5
(the 16-wide leaves) never reach the codebook path at all — they are flat fill
or split only.

Also still open: **anything about the 2.2 sample**, which may be a different
codec generation entirely. No frame has been decoded end to end.

- The publisher describes version 2.x as 24-bit internal colour with runtime
  dithering and proprietary compressed audio. It describes Pro as its
  low-bitrate successor. Independent format notes describe the older family
  as 4x4-codebook YUV and Pro as recursive 8x8/8x4/4x4/4x2 blocks with motion
  compensation. Treat the latter as a hypothesis to verify against the ARM
  routines, not as a decoder specification.

### The leaf value VLCs are complete prefix codes

`tools/gba_video/caimans_valuevlc.py` transcribes both ladders and inverts
them to codewords by driving each decoder over every candidate prefix and
keeping the prefixes it consumes whole -- the same emulate-then-invert method
that cracked the MV table.

| path | codewords | value range | code lengths | Kraft sum |
|---|---:|---|---:|---:|
| inter (signed residual DC) | 512 | -256 .. +255 | 1..22 | **1.000000** |
| intra (unsigned level) | 256 | 0 .. 255 | 4..20 | **1.000000** |

Both are prefix-free with a **Kraft sum of exactly 1.0**, which is the real
check on the transcription: a misread comparison bound or a table offset that
was one entry out would essentially never yield a complete code. The intra
table is a permutation of 0..255, each level exactly once.

The inter code lengths grow monotonically with `|value|` -- 1 bit for 0, 3 and
4 bits for +/-1, 5 bits for +/-2..3, up to 22 bits past +/-100 -- which is what
a trained residual-DC code should look like:

```
1        ->  0        00110    -> +3       001000   -> -5
011      -> +1        00111    -> +2       001001   -> -4
0101     -> -1        01000    -> -2       001010   -> +5
                      01001    -> -3       001011   -> +4
```

**One anomaly, flagged rather than explained.** `-128` has no codeword: the
two codes that should be the `+/-128` pair both decode to `+128`, so the value
table covers -256..+255 with -128 missing and +128 duplicated. Everything
around it is orderly, so this is most likely a quirk of the shipped table
(an encoder that never emits -128), but a one-entry transcription error cannot
be ruled out until a real bitstream is decoded. Do not "fix" it by assumption.

### The bit reader and its per-picture reset

`FUN_03003ec4` (n bits), `FUN_03003e3c` (n bits, halfword path) and
`FUN_03003f68` (1 bit) all share the two-word state at `DAT_03003fa4` =
`0x03000024`: `{const uint8_t *base; uint32_t bitpos}`. Reads are MSB-first,
assembled by byte-swapping a little-endian load, with aligned and unaligned
paths that differ only in how they gather the bytes.

`FUN_03005a00`'s prologue is where a picture is bound to the stream:

```c
FUN_03005a00(hdr_out, data, limit)   /* r0, r1, r2 */
    bitstate->bitpos = 0;
    bitstate->base   = data;
    first = get_bits(0x16);          /* the 22-bit start code, checked == 0x20 */
```

So **the bit position is zeroed per picture** and each frame's bitstream
begins byte-aligned at `data`. The caller passes `limit = 0x4e20` (20000), a
fixed maximum frame size. This is the seam between the container and the
bitstream: find who produces `data` and the packet framing follows.

### The container: an index table at ROM 0xc4b0 over data at 0xc768

The player's top-level frame routine is at IWRAM `0x03006af0` (Ghidra will not
auto-create it; `disassemble_at` works). It takes a pointer to a descriptor
struct in `r0` and scatters its fields into globals at `0x03007058`+, then
sets `WAITCNT` (`0x04000204`) to `0x4317` and runs the frame loop that calls
`FUN_03005a00` at `0x03006f98`. Nothing in IWRAM references `0x03006af0`, so
the descriptor is built by the ROM's own Thumb code -- not yet traced.

The stream tables were instead found by structure, directly in the ROM:

| ROM file offset | contents |
|---|---|
| `0xc4b0` | table A: 87 x `uint32`, strictly increasing `0x10 .. 0x138a00`, terminated by a zero word |
| `0xc610` | table B: 86 x `uint32`, strictly increasing `60 .. 1146` |
| `0xc768` | video data base (16 zero bytes, then the first picture at `0xc778`) |

**Table A holds byte offsets relative to `0xc768`, confirmed decisively.** A
picture starts with a 22-bit start code of `0x20`, which constrains the first
three bytes to `00 00 8x`-`00 00 bx`. Sweeping candidate bases and testing all
87 entries against that pattern gives **87/87 hits at base `0xc768`, and 0
hits at every other base tried**. Only about 3280 positions in the whole
1.7 MB ROM have that byte shape, so 87 independent hits is not chance. The
base also lands exactly where table B ends, so the two tables and the data are
contiguous.

Table A is a **seek/chapter index, not a frame index**: its last entry
(`0x145168` absolute) is followed by another 379 KiB of ROM that begins
`00 00 81 b0 17 0f 00 80` -- the same `17 0f 00 80` field that follows the
first picture's start code at `0xc778`, so the stream plainly continues past
it. Table B's 86 values (60, 81, 100, 111, 141, ... 1146) are one per interval
of table A and look like frame or time indices, but the implied bytes-per-frame
swings from 14 to 8472, so **do not treat that reading as established**.

### A reference block-layer decoder

`tools/gba_video/caimans_blocks.py` puts the recovered pieces together: the
MSB-first bit reader, the subdivision walker, both mode VLCs, both value VLCs
and the codebook accumulation, reading every table out of the IWRAM image
rather than hardcoding it. It decodes one macroblock given a bit position and
a prediction buffer.

It is **not** a frame decoder: the picture header, the motion vectors and the
motion compensation are not implemented, and no real bitstream has been run
through it. Its self-tests are structural, but two of them are worth having:

- With every split flag set, the walker produces exactly 32 4x2 leaves that
  tile the 16x16 macroblock **once each, with no overlap and no gap**.
- 200 random split patterns each tile the macroblock exactly. A wrong `delta`,
  a wrong axis parity or wrong level bookkeeping shows up here immediately.

It also re-derives the ladder from the IWRAM size tables (4x2, 4x4, 8x4, 8x8,
16x8) and checks that the `(nib + 16i) << (level+1)` pattern stride equals the
leaf size at every level on both paths.

One observation from the walker: a root that never splits yields a single
level-5 leaf, and level 5's entry in the size tables is 0x0, so it draws
nothing. That makes "no split at the root" a natural **skipped macroblock**.
It is consistent with the code but has not been confirmed against a stream.

### The picture header, decoded and hand-checked

`tools/gba_video/caimans_frame.py` implements `FUN_03005a00`'s header parse.
The first picture in the ROM (`0xc778`) reads as **240x128, intra, a 75-bit
header**, and all 87 index entries parse identically. The bits were checked by
hand rather than trusted:

```
00000000000000001000000000000000 00010111 00001111 00000000 10000000 ...
|--------- 22 bits = 0x20 ------|
                       TR = 0 --||ptype = 0 (intra)
                                  |--5--||fmt=7 (escape)
                                          |-- 12 bits = 240 --||-- 12 = 128 --|
```

The frame-size table at `0x030070b4` is 8 x `{uint16 w, uint16 h}` and does
hold H.263's formats in H.263's own slots (1 = SQCIF 128x96, 2 = QCIF 176x144,
3 = CIF 352x288, 4 = 4CIF 704x576), with custom entries at 0, 5, 6 and 7 as the
`0xffff` escape. This sample uses the escape, not a table slot.

The non-`0x20` descramble is 8 words at `data + 4`, of which the first four are
rewritten in place: `w[i] = rot16(w[i]) ^ w[7 - i]` for `i` in 0..3.

Buffer geometry, straight from the routine's setup: luma is the frame size
padded to a multiple of 16; chroma is **quarter resolution on both axes**
(`((dim + 3) >> 2)` padded to 16), and there are two chroma planes. For 240x128
that gives Y 240x128, U and V 64x32. The plane loop runs three times, and an
intra picture's macroblock loop calls the block layer directly with
`param_3 = 1` and **no per-macroblock header at all** -- the MB-type VLC and
motion vectors exist only on the inter path.

### Intra decoding is validated pixel-exact against the real player

`tools/gba_video/caimans_frame.py` decodes an intra picture end to end, and it
is **byte-identical to the hardware's own output**:

```
python: 240x128, 136 macroblocks, 17402 bits
hardware: picture at 0x0800c778, framebuffer at 0x02002000
  plane Y 240x128   30720 bytes  IDENTICAL
  plane U  64x32     2048 bytes  IDENTICAL
  plane V  64x32     2048 bytes  IDENTICAL
```

All **87 indexed pictures** decode without error, and the first one renders as
the MPAA green-band card that opens the trailer. Two tools do the checking, and
both should be re-run after any change to the block layer:

- `caimans_trace_blocks.py` breaks on `FUN_03004cd4` in mGBA and compares the
  bit-reader position at every macroblock against the Python decoder. It
  reports **no disagreement over 136 macroblocks**, which validates the parse
  independently of the reconstruction.
- `caimans_validate_frame.py` runs the player to the end of the first picture,
  reads its framebuffer out of EWRAM and compares all three planes byte for
  byte. This is the same bar the other five lineages had to clear.

Three bugs had to be fixed to get there, and the first is the interesting one.

**1. The leaf decode is inline in the subdivision walk.** The original decodes
each leaf *at the moment the walker reaches it*, so a leaf's mode/value/pattern
bits sit in the bitstream between the split flags either side of it. Collecting
the leaf shapes first and decoding them afterwards reads exactly the same bits
in a different order, and desynchronises at the first macroblock that has both
a split and a leaf. This is why `BlockDecoder.walk` takes a callback, and why
`subdivide` is now documented as shape-only, for the structural tests.

The failure this produced was maximally misleading: the desync surfaced as a
leaf at level 4 or 5 decoding to mode >= 1, which needs a codebook that does
not exist -- so it looked like a missing table rather than a bit-order bug. The
earlier conclusion that levels 4 and 5 never take the codebook path was
**correct all along**; the ROM sweep that found no level-4/5 codebook was
finding nothing because there is nothing to find.

**2. The fill callbacks take a selector, not a row count.** Each is fully
unrolled and compares its count against half its width to choose half or full
height: `FUN_03004438` does `cmp r1,#2`, `FUN_03004468` `cmp r1,#4`,
`FUN_030044c4` `cmp r1,#8`. So the level-5 entry of `(width 0, height 0)` --
which is genuinely `(0, 0)` in the live tables, not an artefact of the dump --
means the 16-pixel callback painting a **full 16x16**. An un-split root is
therefore a solid 16x16 fill, not a skipped macroblock as previously written
here. The real ladder is 4x2, 4x4, 8x4, 8x8, 16x8, 16x16.

The codebook path does *not* go through those callbacks: it loops
`height[level]` rows of `width[level]/4` words using the raw table values,
which equal the true leaf size at levels 0-4 and are zero at level 5 -- so a
level-5 codebook leaf would draw nothing, which is exactly why level 5 needs no
codebook entry.

**3. The chroma plane offsets are fixed constants**, `0x9600` and `0xa200`
(`param_1[0xc]` and `param_1[0xd]`, literals in the routine), sized for a full
240x160 luma plane. They are *not* derived from the picture's dimensions, so
for this 240x128 sample the U plane starts well past the end of the luma data.

### Audio: framing recovered, kernel validated bit-exact

The audio handler is the interrupt routine at IWRAM `0x03006630`. It stops
DMA1, decodes one block into whichever of two 64-byte ping-pong buffers is
next, and restarts the FIFO transfer:

```c
DMA1CNT = 0;
dst = toggle ? 0x03000070 : 0x0300002c;      /* two 64-byte buffers */
DMA1SAD = dst;
decode(src = *(uint8_t **)0x03007080, dst, 0x40);   /* 64 samples */
toggle = 1 - toggle;
IF |= IF;                                     /* acknowledge */
*(uint8_t **)0x03007080 += 0x20;              /* 32 bytes consumed */
DMA1CNT = 0xb6400000;   /* enable | IRQ | FIFO start | 32-bit | repeat | dest fixed */
```

**32 bytes in, 64 samples out** is exactly 4 bits per sample, and the codec
state at `0x03007050` is never reset between calls. So the stream is **flat
ADPCM with no block headers, no packets and no per-block re-initialisation** --
which is why looking for a block structure earlier found nothing. It is
**mono**: one decode call, one FIFO.

| property | value |
|---|---|
| stream base | ROM `0x081473e1` (file offset `0x1473e1`) |
| length | 379584 bytes to the end of ROM |
| block | 32 bytes -> 64 samples, decoded on each FIFO interrupt |
| sample rate | 24564 Hz (TM0 reload `0xfd55`: 16777216 / 683) |
| channels | 1 |
| output | signed 8-bit PCM |
| state | `{int32 predictor, int32 step_index}` at `0x03007050`, persists across blocks |

`tools/gba_video/caimans_trace_audio.py` breaks either side of the decode call
in mGBA and replays each captured block through `caimans_adpcm`, comparing both
the 64 PCM bytes and the resulting codec state. Deep into the stream, where the
signal is loud:

```
300 audio calls, source pointer 0x0815ddc1 .. 0x08160321, deltas [32]
nibble magnitudes exercised: {0: 5058, 1: 4473, 2: 3641, 3: 2557,
                              4: 1518, 5: 874, 6: 752, 7: 327}
PCM mismatches:   0
state mismatches: 0
```

The magnitude histogram is the part that matters. The kernel's two deviations
from standard IMA only fire at high magnitudes -- the extra `diff += step >> 1`
needs `mag == 7`, and the altered index table entries `{2,4,7,12}` need
`mag >= 4`. A capture from the start of the trailer validates cleanly but is
**worthless**, because the opening is near-silent and only reaches magnitude 2;
`--skip-frames` exists to get past it. With 327 magnitude-7 samples in the run
above, both deviations are exercised and both are bit-exact.

### The ROM is two contiguous regions

The audio base settles the layout of the whole cartridge:

| region | range | contents |
|---|---|---|
| player | `0x004f6c .. 0x00bfbc` | the IWRAM image, copied verbatim |
| tables | `0x00c4b0 .. 0x00c768` | index table A, then table B |
| video | `0x00c768 .. 0x1473e1` | 1.23 MB of pictures |
| audio | `0x1473e1 .. 0x1a3ea1` | 379 KiB of flat ADPCM |

This corrects an earlier note here. The 379 KiB "tail" past table A's last
entry is *not* all more video -- it is the last stretch of video followed by
the whole audio stream. Counting picture-start-shaped byte triples gives 1202
in the video region (one per ~1073 bytes) against 2012 in the audio region
(one per ~189), so that pattern is only meaningful on the video side; the
audio's high count is an artefact of how many zero bytes quiet ADPCM contains.

1202 candidate picture starts against table B's final value of **1146** is a
good match once chance hits are allowed for, which is the first real support
for reading table B as a picture count.

**Open, and deliberately not asserted:** the frame rate does not quite close.
Skipping 220 pictures advances the audio pointer by 92640 bytes = 7.54 s,
implying about 29 pictures/second, but 1146 pictures at that rate is 39 s
against only 30.9 s of audio. Either some pictures are decoded without being
displayed, `FUN_03005a00` is entered more than once per displayed frame, or the
stream does not play straight through. Worth settling before writing a demuxer
that has to produce timestamps.

### Inter pictures: macroblock types, MVs, and MC implemented and parsed bit-exact

`tools/gba_video/caimans_inter.py` implements `FUN_03005a00`'s inter branch:
the macroblock-type VLC, `FUN_03005848`'s MV reader (`caimans_mvvlc.py`), and
the four half-pel interpolations, wired to the existing block layer for the
residual. `tools/gba_video/caimans_frame.py:decode_picture` now decodes a
sequence of pictures of either type, carrying width/height and a reference
framebuffer forward exactly as the original does (an inter header carries no
size of its own).

**Macroblock types** -- a 3-bit peek into `0x03001d00` (stride 4: `int16`
value, `int8` length at +2) gives a complete prefix code: `'1'`->type 0,
`'01'`->type 1, `'001'`->type 2, `'000'`->type 3.

| type | meaning |
|---:|---|
| 0 | copy the co-located 16x16 block from the reference frame; no motion, no residual |
| 1 | one 16x16 motion vector (median-of-three predicted), then a residual through the block layer (`intra=False`) |
| 2 | four independent 8x8 motion vectors, then one residual call over the full 16x16 |
| 3 | an intra macroblock inside an inter picture -- no motion, block layer runs `intra=True` |

Types 0 and 3 have no MV, and the original clears both the running "left" MV
register and this macroblock's slot in the "top row" MV cache to zero when it
sees them, so a neighbouring predicted macroblock treats a copy/intra
neighbour as zero motion rather than "no data".

**Motion vectors** are half-pel: integer displacement is `mv >> 1`
(arithmetic), the fractional flag is `mv & 1`. `caimans_mvvlc.py` reconstructs
the per-component VLC the same way the leaf value VLC was cracked --
emulate-then-invert -- and gets 65 codewords covering -32..+32 (33 magnitudes
x 2 signs, minus one for zero's own single-bit code), Kraft sum 0.999512. The
~0.0005 gap is a codeword the encoder apparently never emits, the same shape
as the leaf value VLC's missing -128 -- not chased further.

**Half-pel interpolation** reduces to per-pixel scalar arithmetic once the
SIMD packing is stripped out (verified against the ARM disassembly of all
four routines): full copy, `(a+b+1)>>1` horizontal/vertical, and
`(a+b+c+d+2)>>2` diagonal.

**A bug found and fixed while validating this:** `decode_leaf`'s "does this
level have a codebook" check was unconditional, but it should only apply when
`mode > 0` -- mode 0 never indexes the codebook array regardless of level
(the loop over layers is simply empty), and this was masked on the intra path
by the mode-0 fill shortcut. Inter macroblocks route mode 0 through the
generic codebook-sum code (with zero layers), which reaches levels 4-5 and
was raising `Desync` for a case that isn't actually invalid.

**Validated against real hardware, two ways:**

- **Bit consumption**, across 99 consecutive pictures (5 intra, 94 inter),
  captured by breaking on `FUN_03005a00` in mGBA and recording the bitstream
  position at each picture boundary: **99/99 exact matches**. This validates
  the entire parsing skeleton -- mb-type VLC, MV VLC (both magnitude ladders,
  both components, for both 1-MV and 4-MV macroblocks), and the residual VLC
  -- independent of whether the reconstructed *pixels* are right, since VLC
  codeword length depends only on the bits, never on which predictor was
  used.
- **Pixels**, for the first ~6 pictures of the sequence (one intra keyframe
  plus 5 inter pictures, heavily type-0/type-1 dominated): **byte-identical**
  across all three planes, confirmed by reading the player's own framebuffer
  out of EWRAM at `0x02002000` mid-decode.

**Open, and not resolved this session: a second intra keyframe (picture index
60, ROM `0xd8bc`) decodes with the exact right bit count (1195, matching
hardware) but produces pixels that do not match hardware's framebuffer,
despite the *entire* macroblock-type and block-layer parsing pipeline being
independently proven correct by the 99-picture bit-position match.** This
was chased at the individual-instruction level:

- The first macroblock's mode decode was checked against hardware directly by
  breaking at the ARM instruction that loads the mode byte -- **hardware
  computes mode -1 (skip/fill-zero), identical to the Python decoder.**
- The resulting fill callback call was checked directly -- **hardware calls
  it with fill value 0, identical to the Python decoder** -- and reading
  EWRAM immediately after the callback returns confirms **pixel (0,0) is
  genuinely 0 right after this fill**, matching Python.
- But by the time the *next* picture's decode reads this buffer as its
  reference, pixel (0,0) reads back as **80**, not 0. Something writes to it
  in between that has not been identified.

Two follow-up traces attempting to catch that write gave **inconsistent
results between runs** (one showed the value still 0 after 60 single-stepped
instructions; a separate run showed it already 80 by the second macroblock).
Given the mGBA GDB-stub pitfalls already on record below, this inconsistency
is more likely a tooling reliability limit at single-instruction granularity
than genuine hardware non-determinism, but that has not been confirmed
either. **Do not trust single-instruction-level mGBA traces across separate
script invocations without first ruling out stub flakiness at that
granularity** -- the picture-boundary and macroblock-boundary traces used
everywhere else in this document are coarser and were not observed to have
this problem.

Practical implication: intra-picture pixel correctness is only established
for the *very first* picture of a ROM (where the reference buffer is
irrelevant, since nothing has ever written to it). Later intra keyframes are
unverified and, per the above, at least one is actively wrong. Inter-picture
pixel correctness is established only for the short, low-motion opening
stretch. **Do not claim general pixel-exactness for this decoder** until this
is resolved.

### Two real bugs found and fixed in the inter MV predictor

Picking this back up, the type-2 (four-MV) predictor wiring documented
earlier turned out to be guesswork from the decompile alone, and it was
wrong. It was replaced with the literal addressing pattern recovered by
capturing `FUN_03005848`'s three input addresses and their live values
against real hardware across many consecutive calls (not inferred, captured).
That surfaced two separate bugs:

**1. The median-of-3 formula was simply wrong.** The "obvious" implementation
(`_median3` as originally written) does not compute a median at all for
several input orderings -- `_median3(5, 3, 0)` returned `0` instead of `3`.
It was replaced with a literal transcription of `FUN_03005848`'s comparison
chain:

```python
def _median3(left, top, topright):
    b1 = topright <= top
    if left < top: b1 = not b1
    if b1: return top
    b2 = top < topright
    if left < topright: b2 = not b2
    if b2: return topright
    return left
```

This alone was responsible for the vast majority of the pixel error --
every type-1 and type-2 macroblock's predicted MV was wrong whenever the
three neighbours weren't trivially equal.

**2. The type-2 (four-MV) predictor wiring.** Captured directly from
hardware (break on `FUN_03005848`, log `arr`, the three pointers it derefs,
and their live values, for many consecutive macroblocks):

```
mv_a: reads [CARRY, top(c), top(c+2)]         writes ROLL
mv_b: reads [ROLL,  top(c+1), top(c+2)]        writes CARRY
mv_c: reads [ROLL,  CARRY,   top(c-1)]         writes top(c)
mv_d: reads [ROLL,  CARRY,   top(c) (fresh)]   writes top(c+1)
```

where `c = x/8` is the 8-pixel column of the macroblock's left edge, `CARRY`
is a single persistent (x,y) pair shared with type 1 and carried macroblock
to macroblock (hardware's fixed low address in the MV array), `ROLL` is a
type-2-only scratch pair alive for one macroblock's four sub-block reads
only, and `top(n)` is the per-8px-column cache carried row to row. Argument
*positions* don't carry compass-direction meaning -- what matters is which
three addresses feed which call, and this is that pattern verbatim. `MVCache`
and `InterDecoder._decode_four_mv` in `tools/gba_video/caimans_inter.py` now
implement exactly this.

Confirmed against later, non-trivial macroblocks too (not just the first): a
second hardware capture at macroblocks 50+ pictures deep into the sequence
reproduces the identical predictor-input pattern, so this isn't fitted to the
first few examples.

### Where pixel accuracy stands now

The picture-63 case that exposed both bugs went from **~11,000 wrong bytes
across all three planes to 25**, out of 30720+2048+2048 -- a small,
localized remainder now, not a systemic wiring error. The 25 remaining
mismatches are small in magnitude (mostly off by 1-4, a few larger) and
column-clustered within specific type-2 sub-blocks (one 8-pixel-wide
sub-block's *single column*, not the whole sub-block) rather than spread
across the frame -- a shape that doesn't fit a wiring bug and hasn't been
run down further this session.

**Aggregate across all 21 captured checkpoints (pictures 6-99): 96.88%** of
compared bytes now match (22,842 wrong out of 731,136), up from a much worse
baseline before these fixes. But this aggregate is not uniform: pictures 60-63
are at or near 100%, while some later checkpoints (65+) still show
3,000-8,000 wrong bytes per plane. Given picture 63's own error is now tiny,
the more likely explanation is a **second, separate bug** that these two
fixes didn't touch, rather than simple drift from picture 63's own residual
error compounding through the reference chain -- but this has not been
confirmed either way.

## Useful commands

```sh
# Fetch and unpack the two baseline samples.
mkdir -p build_caimans/roms
curl -L --fail -o build_caimans/caimans_badboys2_hi.zip \
  https://www.caimans.net/gbavideo/caimans_badboys2_hi.zip
curl -L --fail -o build_caimans/caimans_pro_pooh_hq.zip \
  https://www.caimans.net/gbavideo/caimans_pro_pooh_hq.zip
unzip -jo build_caimans/caimans_badboys2_hi.zip -d build_caimans/roms
unzip -jo build_caimans/caimans_pro_pooh_hq.zip -d build_caimans/roms

# Start mGBA paused with its GDB remote stub.
/Applications/mGBA.app/Contents/MacOS/mGBA -g \
  build_caimans/roms/caimans_pro_pooh_hq.gba
```

`tools/gba_video/gdbrsp.py` is the small client already used for the VX work.
Set an execution breakpoint at `0x03000000`, continue from reset, and dump
registers/IWRAM after the breakpoint. mGBA reported PC `0x03000000` and CPSR
`0x20000092` at that waypoint.

## Tooling pitfalls hit this session (read before repeating this work)

- **mGBA's GDB stub resets the core on every new client connection.** Each
  fresh `RSP()` socket connect causes mGBA to reboot the ROM from power-on
  and re-run to whatever breakpoints are still installed (breakpoints
  persist server-side across disconnects; register/PC state does not — a
  reconnect is not a resume). Do all stepping/dumping for one investigation
  inside a single unbroken connection/script; a second script attaching
  later starts over from reset, not where the first one left off. This
  invalidated one exploratory script this session (an EWRAM dump taken from
  what was assumed to be a stepped-forward state turned out to be captured
  at the very first `0x08000218` breakpoint instead).
- **`Z2`/write watchpoints are accepted (`OK`) but never fire** on this
  mGBA build (0.10.5). Tested with a 2-byte watch and later a full
  96 KiB VRAM-range watch, each left running 40–90s of wall time against a
  ROM that must touch VRAM well within that window; neither ever stopped.
  Don't rely on watchpoints here — use execution breakpoints (`Z0`, which do
  work, confirmed repeatedly) or single-stepping instead.
- **The resident IWRAM code is ARM, not Thumb — decode it as ARM.** An
  earlier revision of this document blamed a garbage disassembly on
  capstone accepting Thumb-2 encodings the ARM7TDMI lacks. That diagnosis
  was **wrong** and has been removed. The real cause was simply decoding ARM
  code in Thumb mode: the ROM entry at `0x08000219` is Thumb (odd `BX`
  target), but the IWRAM player it transfers to is ARM. Decoded as ARM,
  `0x03004a98` is an unambiguous ARM function prologue
  (`stmdb sp!,{r4-r10,lr}`) and the whole region reads cleanly. Capstone
  itself was never the problem.
- **No `DISPCNT`/DMA/VRAM/palette base address appears as a raw 32-bit
  literal anywhere in the 1.7 MB Pro ROM or the full 32 KiB IWRAM.** A
  4-byte-aligned word scan for the standard MMIO constants came back empty
  in both. Three apparent `0x04000000` hits in a 256 KiB EWRAM dump did not
  hold up — the surrounding bytes are a smoothly incrementing 16-bit ramp,
  so the match is almost certainly coincidental (three zero bytes followed
  by a table entry that happens to equal `0x04`), not a real reference.
  Don't treat that as a lead. Hardware register access here is reached
  through literal-pool pointers loaded via PC-relative `ldr` (the idiom
  used throughout this player — see the audio decoder below), not via
  inline immediates, so a plain constant scan will not find it.

## Recommended next work

Work statically in Ghidra from here -- the ROM<->IWRAM mapping above means no
emulator is needed to read code. When something needs settling against the
real player, capture *values*, not just bit positions, the way the MV
predictor bug was found: bit-position matching alone did not catch either of
the two bugs above, since VLC codeword length never depends on predictor
values.

1. **Find the second pixel bug** (see "Where pixel accuracy stands now").
   Start from a checkpoint that's still bad (e.g. picture 65 or 68) and
   bisect the same way picture 63 was localized: find the first mismatching
   picture, then the first mismatching macroblock and its type, then compare
   its actual reads (MV predictors, or block-layer mode/value) against a
   targeted hardware capture. The `mv-call skip count` technique used here
   (count calls needed to reach a specific macroblock in Python, then use
   that exact count of `FUN_03005848` breakpoint hits in GDB) is much faster
   than tagging every call with its macroblock coordinates.
2. **Chase the remaining 25-mismatch shape in picture 63** if time allows --
   the fact that it's a single *column* within an 8x8 sub-block rather than
   the whole sub-block suggests an off-by-one somewhere specific (a half-pel
   edge condition, or a residual leaf boundary), not a wholesale wiring
   error, and might be a quick, separate fix once found.
3. **Nail down the audio framing** -- block size, channel count, and
   per-stream initialisation of the 2-word codec state at `0x03007050`. The
   sample kernel is already transcribed in `tools/gba_video/caimans_adpcm.py`.
4. **Finish the container**: how a picture's length is known, and whether
   anything indexes the audio. Tracing the ROM Thumb code that builds the
   descriptor struct passed to `0x03006af0` would settle it.
5. **Then** the FFmpeg port, following the integration plan below -- only
   after pixel-exactness is re-established generally.
6. Keep version 2.2 and Pro as separate targets -- everything established so
   far is Pro-only.

## Scope boundary

The current FFmpeg tree supports all five known retail GBA Video lineages:
ADS, Hydrogen (including its LZMA prototype revision), VXGB, VX++, and FVMV.
Caimans and METEO/Avi2GBA are separate aftermarket/commercial GBA video
families and should not be folded into the retail GBA Video demuxers.

## Integration plan (once the bitstream is understood)

Follow the FVMV precedent (`libavformat/fvmv.c` + `libavcodec/fvmvdec.c`),
not the shared `libavformat/gbavideo.c` used by the five retail lineages —
the scope boundary above rules out folding Caimans into that file.

1. **RE stage** (`tools/gba_video/`, Python, no FFmpeg changes): a
   `caimans_dis.py`/`caimans_sim.py` pair that locates stream tables and
   reproduces frame decode against mGBA-captured reference framebuffers,
   mirroring `vx_dis.py`/`vx_sim.py` and `vx_reconstruct.py`'s validate-then-
   port structure. Land only after byte-exact match against real hardware
   output, same bar as the other lineages.
2. **Doc**: keep `doc/caimans_handoff.md` growing into the permanent format
   used by `doc/gba_video_fvmv.md` — magic/probe bytes, stream header
   layout, record layout, retail sample inventory table.
3. **Demuxer** (`libavformat/caimans.c`): probe function scanning for
   whatever fixed byte(s) plays the role of `p->buf[0xb2] != 0x96` in
   `fvmv_probe`, `read_header` parsing the stream table into an `AVStream`,
   `read_packet` slicing one record per call. Register in
   `libavformat/allformats.c` + `libavformat/Makefile`.
4. **Decoder** (`libavcodec/caimansdec.c`): since the publisher's own tech
   notes describe two genuinely different block/motion schemes (2.2:
   4x4-codebook YUV; Pro: recursive 8x8/8x4/4x4/4x2 with motion
   compensation — both still unverified hypotheses per "What is
   established" above), expect two decoders or one decoder gated on a
   version field, not one shared code path. Register in
   `libavcodec/allcodecs.c` + `libavcodec/Makefile`.
5. **GUI listing**: add the family to the encoder GUI's supported-decoder
   list, same as `d78a14d24a` did for the other five.

Do not start step 3 until step 1 has validated frame output against captured
hardware reference frames — every prior lineage in this repo was ported only
after a Python reference decoder matched real output exactly.
