# VX++ GBA Video Codec — Reverse Engineering Handoff

Goal: reverse the ActImagine VX++ video codec used in the GBA "Game Boy Advance Video" carts
(Shrek + Shark Tale USA Rev 5) and implement a decoder in FFmpeg (`libavcodec/vx.c`).
The current `vx.c` implements the DS VX codec (H.264 CAVLC) — that is NOT this target.
Model the decoder on `libavcodec/mobiclip.c` / `tools/mobiclip/decode_ref.c`, swapping in the
ROM-derived VLC table.

---

## 1. Files / resources

- Cart ROM: `<roms>/Game Boy Advance Video - Shrek + Shark Tale (USA) (Rev 5).gba` (67,108,864 bytes = 64 MB multi-ROM)
- Ghidra project loaded with `/tmp/shrek_play_i1.bin` — a live IWRAM RAM dump (address base `0x03000000`, ARM `:LE:32:v4t`). This is IWRAM only; **the VLC table is not in it**.
- Extracted streams: `/tmp/gbavx/stream0{0..3}.video`, `stream0{0..3}.audio`, `stream0{0..3}.hdr.txt`. stream00 = 240×112, header bytes `69 04 | dd 84`.
- Helper: `/tmp/iwtool.py` (capstone wrappers: `dis`, `blrefs`, `litrefs`, `wordrefs`).
- New artifacts this session:
  - `/tmp/gbavx/vlc_rom_0xa000.bin` — raw 7168-byte dump of ROM `0xa000..0xbc00` (LE uint16 cells).
  - `/tmp/gbavx/vlc_blocks.txt` — uniform-block analysis of that dump.
- mGBA GDB stub (`tools/gba_video/gdbrsp.py`, port 2345) was not running this session; not needed for the table anymore.

---

## 2. The VLC codebook — LOCATED (the big win this session)

The residual-coefficient VLC codebook lives in the **ROM at file offset `0x0a000`** (GBA address `0x0800a000`),
NOT in EWRAM/IWRAM. It was extracted statically; no live dump needed.

- Table = `0xE00` halfwords = **3584 cells** at `0x0a000..0x0bc00`, little-endian.
- Cell immediately after the table region is UI string data (`"Brightness -"`, `"Brightness +"`, `"1\0"`, `"2\0"` …) — confirms the table end at ~`0x0bbe4..0x0bc00`.
- Format of each 16-bit entry (confirmed by decoder disasm AND by ROM entries):
  - bits `0..3` = `len` (number of bits to shift the accumulator; **len = code_length + 1**, i.e. includes the sign bit)
  - bits `4..8` = `value` (level; 5 bits, range 0..31; sign applied separately)
  - bits `9..14` = `run` (zero-run before this coefficient; 6 bits)
  - bit `15` = `more` (1 = another coefficient follows in this block)
  - Sign of value: the `len`-th consumed bit (carry out of `lsls r3,r3,len`) → if set, `value = -value`.
- Example entries: `0x0013` → {len=3, value=1, run=0, more=0}; `0x0024` → {len=4, value=2}; `0x8015` → {len=5, value=1, run=0, more=1}; `0x0416` → {len=6, value=1, run=2}.

### Block structure (uniform runs) — cells are indexed by the top 12 bits of the bit accumulator

Index range 0x000–0xd71 are the real VLC blocks (sizes are power-of-2, `size = 2^(12 - code_len)` with `code_len = len-1`):

| idx start | idx end | count | entry | len | code_len | value | run | more | decoded symbol |
|-----------|---------|-------|-------|-----|----------|-------|-----|------|----------------|
| 0x000 | 0x011 | 18 | 0x0e18 | 8 | 7 | 1 | 7 | 0 | run7, val1, eob-ish? |
| 0x012 | 0x031 | 32 | 0x0428 | 8 | 7 | 2 | 2 | 0 | |
| 0x032 | 0x051 | 32 | 0x0238 | 8 | 7 | 3 | 1 | 0 | |
| 0x052 | 0x071 | 32 | 0x0098 | 8 | 7 | 9 | 0 | 0 | |
| 0x072 | 0x0b1 | 64 | 0x8027 | 7 | 6 | 2 | 0 | 1 | |
| 0x0b2 | 0x0f1 | 64 | 0x0a17 | 7 | 6 | 1 | 5 | 0 | |
| 0x0f2 | 0x131 | 64 | 0x8417 | 7 | 6 | 1 | 2 | 1 | |
| 0x132 | 0x171 | 64 | 0x8217 | 7 | 6 | 1 | 1 | 1 | |
| 0x172 | 0x1b1 | 64 | 0x0817 | 7 | 6 | 1 | 4 | 0 | |
| 0x1b2 | 0x1f1 | 64 | 0x0617 | 7 | 6 | 1 | 3 | 0 | |
| 0x1f2 | 0x231 | 64 | 0x0087 | 7 | 6 | 8 | 0 | 0 | |
| 0x232 | 0x271 | 64 | 0x0077 | 7 | 6 | 7 | 0 | 0 | |
| 0x272 | 0x2b1 | 64 | 0x0227 | 7 | 6 | 2 | 1 | 0 | |
| 0x2b2 | 0x2f1 | 64 | 0x0067 | 7 | 6 | 6 | 0 | 0 | |
| 0x2f2 | 0x371 | 128 | 0x0416 | 6 | 5 | 1 | 2 | 0 | |
| 0x372 | 0x3f1 | 128 | 0x0056 | 6 | 5 | 5 | 0 | 0 | |
| 0x3f2 | 0x471 | 128 | 0x0046 | 6 | 5 | 4 | 0 | 0 | |
| 0x472 | 0x571 | 256 | 0x8015 | 5 | 4 | 1 | 0 | 1 | |
| 0x572 | 0x971 | 1024 | 0x0013 | 3 | 2 | 1 | 0 | 0 | largest block |
| 0x972 | 0xb71 | 512 | 0x0024 | 4 | 3 | 2 | 0 | 0 | |
| 0xb72 | 0xc71 | 256 | 0x0215 | 5 | 4 | 1 | 1 | 0 | |
| 0xc72 | 0xd71 | 256 | 0x0035 | 5 | 4 | 3 | 0 | 0 | |

Then an odd "tail" 0xd72–0xdf1 (~128 cells) with tiny/weird entries
(`0x0a1b` len11 val1 run5; `0x0405`; `0x0303`×2; `0x0202`; `0x0101`×2; `0x0001`; `0x0000`×24; `0x0308`; …).
This tail may be the long-code/escape fill region — **unresolved, needs verification**.

### KEY OPEN QUESTION — block ordering & indexing

- Decoder indexes the table with the **top 12 bits of the accumulator** (`r4 = r3 >> 20`, `ldrh [fp + r4*2]`).
- Block sizes fit `2^(12 - (len-1))`, i.e. each block corresponds to a code of `code_len = len-1` bits with
  the `len`-th bit being the sign. Good.
- BUT the blocks are NOT in canonical Huffman order (shortest first). The order goes:
  7,7,7,7 (len-8 blocks), then six 6-bit blocks, then 5-bit, 4-bit, **then the 2-bit code (0x0013, 1024 cells) comes AFTER the 4/5/6/7-bit blocks**, then 3-bit, 4-bit, 4-bit. That is a non-ascending length order.
  - Possible explanations to check:
    1. The table order is by code *value* in a particular code-length assignment (e.g., canonical order computed over a *bit-reversed* or non-minimal Huffman tree), OR
    2. The index is not literally `r3>>20` — maybe the accumulator is pre-rotated/offset so the effective code starts a few bits down, changing which blocks overlap the escape region, OR
    3. The 18-cell `0x0e18` block and the tail are artifacts of a *two-level* table (long codes > ~9 bits go through an escape path), and the "weird tail" cells 0xd72–0xdf1 are markers/secondary-index fill.
  - Resolution path: recompute the exact bit positions by emulating the decoder against real stream00 data,
    or read the escape sub-tables at `fp+0x2000` (value-offset bytes) and `fp+0x2080` (run-offset bytes).

### Escape path — FULLY RESOLVED via decompile of `0x030059b4..0x03005a90`

Trigger: top 7 bits of accumulator `acc>>25 == 3` (bit pattern `0000011...`). This is checked
**before** the primary table is indexed, which explains why codebook indices `0x060–0x07F`
(where `idx>>5==3`, i.e. `acc>>25==3`) are **dead/unreachable** in the block table in §2 — the
escape check intercepts those codes first. That resolves the "non-canonical block order" question:
the order isn't actually wrong, those specific cells are simply never reached.

There are **three** escape variants, selected by up to 2 more bits after the 7-bit `0000011` prefix:

- **Variant 1** — next bit (orig bit 24) `== 0` → 8-bit prefix `00000110`. Do a *second* primary-table
  lookup at the new top-12 bits (`len2/value2/run2/more2`). Then:
  `idx_vt = cell>>9` (7 bits, 0..127 — includes the `more` bit as the index's MSB)
  `value = value2 + value_offset_table[idx_vt]`
  Sign/consume as normal, using `len2`. Total bits consumed = 8 + len2.

- **Variant 2** — bits `10` after prefix → 9-bit prefix `000001101`... (bit24=1, bit23=0). Second
  primary-table lookup (`len3/value3/run3/more3`). Then:
  `ridx = more3*0x40 + value3`
  `run = run3 + run_offset_table[ridx]`
  Sign/consume as normal, using `len3`. Total bits consumed = 9 + len3.

- **Variant 3** — bits `11` after prefix (bit24=1, bit23=1) → 9-bit prefix, then **raw literal fields**,
  no further table lookup: `more` = next 1 bit, `run` = next 6 bits (unsigned), `value` = next 12 bits
  (sign-extended, arithmetic shift). Total bits consumed = 9 + 1 + 6 + 12 = 28.

Bit refill helper at `0x030007a8` (models an infinite bitstream; irrelevant for offline simulation
since we can just re-peek 32 bits fresh at any bit position).

**Escape tables extracted from ROM** (fp = codebook base = ROM `0x0800a000`, confirmed — this is a
flat ROM pointer, not an EWRAM copy):
- `value_offset_table` @ `fp+0x2000` = file offset `0xc000`. Real table is only **48 bytes** (indices
  0–47), **all `0x1f` (31)** — genuine ARM code (`push {r4-r10,lr}; ldr r1,[r0,#4]; ...`, a clean
  function prologue) starts immediately after at `0xc030`, confirming the table's true length. Saved
  to `/tmp/gbavx/value_offset_table.bin` (first 0x30 bytes are the real table).
- `run_offset_table` @ `fp+0x2080` = file offset `0xc080`, 128 bytes dumped to
  `/tmp/gbavx/run_offset_table.bin`. Bytes here do *not* form a clean instruction prologue (unlike
  the value-offset table's tail), consistent with this being real table data across its full needed
  range (index = `more*0x40 + value`, max 95).

**Simulator**: `tools/gba_video/vx_sim.py` (formerly decode_vlc2.py)
implements this exactly (all 3 variants + normal path) and was validated against `stream00.video`
starting at bit 0 — coefficients for the first several blocks decode to small, plausible residuals.
**Caveat**: naive back-to-back block decoding breaks down after ~14 blocks (hits table indices that
don't correspond to a real coefficient) — see new blocker #6 below; this is expected, not a bug in
the VLC logic itself.

---

## 3. Decoder core — FUN_03005794 (residual 4×4 block, disassembled + decompiled)

Signature: `FUN_03005794(ctx, ?, bitcount, accumulator)`. Fully reconstructed:

1. Zero 16 coeff words at `0x030056f0` (16×u32 buffer).
2. `fp = *(ctx + 0x20)` → the codebook base (ROM 0x0800a000 or its EWRAM copy).
3. Loop (per coefficient):
   - `idx = acc >> 20`; `entry = fp[idx*2]` (16-bit)
   - `len = entry & 0xF`; `value = (entry>>4)&0x1F`; `run = (entry>>9)&0x3F`; `more = (entry>>15)&1`
   - `acc <<= len`; if carry set → `value = -value` (sign = last consumed bit)
   - `coeffbuf += run`; `*coeffbuf = value`
   - loop while `more == 1`
4. After loop: if exactly one coefficient was written, jump through `PTR_LAB_03005788[?]`; else run the
   **4×4 inverse transform** inline (`0x0300582c..0x0300598c`), then jump through `PTR_LAB_0300577c[?]`.
   The jump index comes from an un-decoded register (`unaff_r10` / `lr`), likely a block-type/plane selector
   (Y vs Cb vs Cr, or luma/chroma dequant choice) — **needs the caller to resolve**.

Dequant: constants `0x200/0x280/0x320` live at `0x03005770/74/78`; the transform is the standard
Butterfly-8 1D + transpose 4×4, with `>>1` on some products (scaled IDCT). Output written to `DAT_03005730..`.

---

## 3b. MB/8×8-region block-loop — `0x03005650` (decompiled/disassembled this session, resolves blocker #3)

This is the caller that sits between MB dispatch and `FUN_03005794`. Full disassembly-verified logic:

```
sub r4, r0, #0x8
ldmia r4, {r5,r6,r7,r8}     ; r5=luma_ptr0, r6=chroma_ptr0, r7=luma_stride?, r8=chroma_stride?
add r5, r5, r7              ; r5 = luma dest pointer
add r6, r6, r8              ; r6 = chroma dest pointer
outer_loop (r12 = height counter, steps of 8):
  inner_loop (r11 = width counter, steps of 8):
    bl 0x0300076c           ; ue(v) exp-golomb decode -> r6 = code value
    r12b = permtab[r6]      ; adr r11,0x3005610; ldrb r12,[r11,r6]  (6-bit CBP-style mask)
    r11 = r5 (luma dest); r10 = 0 (PLANE = LUMA)
    if r12b & 0x01: bl FUN_03005794   ; top-left 4x4 luma block
    r11 += 4
    if r12b & 0x02: bl FUN_03005794   ; top-right 4x4 luma block
    r11 += 0x3fc                     ; drop to next row (stride - 4)
    if r12b & 0x04: bl FUN_03005794   ; bottom-left 4x4 luma block
    r11 += 4
    if r12b & 0x08: bl FUN_03005794   ; bottom-right 4x4 luma block
    r11 = chroma_ptr0; r10 = 1 (PLANE = Cb)
    if r12b & 0x10: bl FUN_03005794   ; 4x4 Cb block
    r10 = 2 (PLANE = Cr)
    if r12b & 0x20: bl FUN_03005794   ; 4x4 Cr block
    r5 += 8; r6 += 8                 ; advance to next 8x8 luma / 4x4-chroma-pair region
    r11 -= 8 (width counter); loop inner while != 0
  r5 += 0x800; r6 += 0x400            ; next row of 8x8 regions (luma stride "0x100" px/row-group,
                                       ; chroma half that — consistent with 4:2:0 subsampling)
  r12 -= 8 (height counter); loop outer while != 0
```

So each "8×8 luma + 4×4 Cb + 4×4 Cr" region (standard 4:2:0 macroblock-ish unit) is gated by ONE
`ue(v)`-coded CBP index, remapped through the 64-byte permutation table into a 6-bit
per-subblock coded flag. **This directly answers blocker #3**: `unaff_r10` in `FUN_03005794` is the
plane selector (0/1/2 = Y/Cb/Cr), set by this caller immediately before each `bl 0x03005794`.
`PTR_LAB_03005788`/`PTR_LAB_0300577c` (the post-decode jump tables) almost certainly index on
block-position-within-plane (which of the 4 luma slots, or the single chroma slot) combined with
`r10` — still needs decompiling to confirm exactly, but the raster-offset math above (`+4`, `+0x3fc`,
`+8`, `+0x800`/`+0x400`) already gives the effective per-plane geometry regardless.

**CBP permutation table** at `0x3005610` (64 bytes, dumped from the live IWRAM image — this address is
executable-region-resident code/data, present in `/tmp/shrek_play_i1.bin`): a genuine permutation of
`0x00..0x3f` (confirmed, not a compressing lookup):
```
00 0f 1f 08 02 01 04 3f 0a 05 0e 0b 03 0c 10 0d
07 2f 06 09 20 1b 1e 17 1a 1d 15 11 13 12 18 14
1c 37 3b 3e 19 2b 21 27 16 2a 2e 25 22 3d 2d 28
24 35 23 3a 33 2c 29 30 26 31 3c 32 39 36 34 38
```

**`FUN_0300076c` confirmed as the `ue(v)` unsigned Exp-Golomb decoder** (disassembled, not just
decompiled — the earlier Ghidra decompile of this function was misleading/mangled):
```
count N = leading zero bits before the first '1' bit (consumes N zero-bits + the 1 stop-bit)
suffix = next N bits (unsigned)
value  = suffix + (1<<N) - 1        ; standard ue(v) formula, result in r6
consumes 2N+1 bits total; falls through to the shared refill routine at 0x030007a8 if the
local bit counter (r2) goes negative.
```
Notably, **`0x030007a8` is the same refill address used inside `FUN_03005794`'s coefficient loop** —
one shared low-level bit-buffer-refill routine feeds both the `ue(v)` reader and the VLC/escape
decoder, not two separate mechanisms.

---

## 4. Larger pipeline map (from previous sessions, still accurate)

- Main player entry: `FUN_03006df8` (r5=ctx, sl=r1 buffer arg, lit `0x06015f40` → `[ctx,#0x8c]`).
  Writes `sl + 0x08000000` into `[ctx,#0x20]` and `[ctx,#0xbc]` at `0x3006f04/08` (this is the buffer/codebook base the residual decoder reads from `[ctx,#0x20]`).
- EWRAM frame slots: `0x020032c8` / `0x020122c8` / `0x020212c8` / `0x020302c8` (spacing 0xf000),
  loop `0x3006e40..0x3006e9c`, literal at `0x3007924`. Slot struct: Y plane 0xa000 (codebook first 0x2000 + pixels); secondary ptrs `[ctx,#0xf4]=0x0203f314`, `[ctx,#0x10c]=0x0203f5e0`; extra literal `0x020010e4`.
- `FUN_03006d30` = slot rotation (NOT codebook init; disproved `0x3006da0` as init).
- Residual chain: MB dispatch `FUN_03001dac` / table `0x3001d4c` (24 entries) → per-MB handler
  (~31 inter handlers tail-branch to 0x3005650) → mask ue → 64-byte permutation table at `0x3005610`
  → per-4×4 block `FUN_03005794`.
- Output: `clamp(ref + IDCT(coeff·dequant) >> 6, 0, 255)`.
- Bitreader (ue/se): `FUN_0300076c` / `0x300081c` (state `DAT_03000814`, mask `0x3000810`, spin on `[0x3000818]&0x1000`).
- MV: median-of-3 prediction + se MVD; subpixel filter `0x300156c`; 8×8 mirror `FUN_03002910`.
- Intra: intra16 `0x30009b8/0a04/0a54/0ba4/0c08`; intra8×8 `0x3000ecc/0eec/0f90/0fe0` — pure spatial.
- QP: `FUN_03000520` (tables `0x03000680/6c8/6f8`).
- `FUN_03002b9c` = 8×8 inter, 5 se fields.
- Stream00.video header `69 04 | dd 84`, 240×112.

### Ghidra TRAPs (read before trusting tool output)
- Capstone prints pointer-table words as `movweq` junk — `movweq` at an address is often data, not code.
- `litrefs`/`wordrefs` miss register-relative / `ldm`-style references (this hid the quantizer and the codebook write initially).
- To find code refs to a RAM function, you may need to scan for the function's literal address bytes manually.

---

## 5. What is still unresolved (blockers)

1. ~~**Exact VLC code→bitstring mapping.**~~ RESOLVED for the primary table + all 3 escape variants —
   see §3 above. `decode_vlc2.py` implements the full, disassembly-verified logic.
2. ~~**The 0xd72–0xdf1 "tail" cells**~~ PARTIALLY RESOLVED: these are ordinary primary-table cells (not
   escape markers) — `idx=3544` (`0x101`: len=1,value=16,run=0,more=0) is a perfectly valid cell, value
   16 is well within the 5-bit 0–31 range. Nothing wrong with the cell itself. The real open question is
   #6 below: whether the simulator reaches it at a *legitimate* bit position.
3. ~~**Caller/register context of FUN_03005794**~~ RESOLVED — see new §3b below. `r10` = plane selector
   (0=luma, 1=Cb, 2=Cr), set explicitly by the caller at `0x03005650` before each call.
   `PTR_LAB_03005788`/`PTR_LAB_0300577c` jump indices still open (probably block-position/raster-offset
   within the plane — not yet decompiled).
4. **IDCT variants** `0x3005a94..0x3006044` (the jump-targets of the dispatch) — confirm they are the 4×4 transform
   / chroma / DC variants.
5. Then: full frame syntax (MB header, mode ue values, chroma block layout), and `FUN_03002b9c` 8×8 inter DC offsets.
6. ~~**naive back-to-back block decoding is invalid**~~ RESOLVED + VALIDATED BIT-EXACTLY. Simulator
   `decode_vlc3.py` implements the real structure: per 8×8 region, read one `ue(v)` CBP code, permute
   through the 64-byte table at `0x3005610`, decode exactly the masked 4×4 blocks (up to 4 luma + Cb +
   Cr). Against real `stream00.video` bytes, **region 0 decodes perfectly**: `ue=2` (bits `011`, 3 bits)
   → mask `0x1f` (`011111`, Cr not coded) → Y0=[-1,3,...], Y1=[-16,...], Y2=[0,-1,...], Y3=[-1,...],
   Cb=[2,0,2,...], ending exactly at bit 40 — matching the earlier naive block-by-block trace bit for
   bit once the 3-bit CBP-code offset is accounted for. This is strong confirmation the VLC/escape
   table, the `ue(v)` reader, and the CBP permutation table are all correctly understood.
   **NEW BLOCKER**: region 1 desyncs — reading a `ue(v)` CBP code at bit 40 gives `98`, which is
   impossible (permutation table only has 64 entries; verified by hand against the raw bytes, not a
   simulator bug). So `0x03005650`'s simple "read CBP, decode masked blocks, repeat" loop does NOT
   apply uniformly to every 8×8 region back-to-back — something else must sit between regions (most
   likely a per-region mode/type dispatch, matching §4's note that `FUN_03001dac` / the 24-31-entry
   handler table at `0x3001d4c` picks which handler runs, and only *some* modes tail-branch into
   `0x03005650`; others are presumably intra/skip/MV-coded with different bit layouts). **Next step**:
   decompile `FUN_03001dac`'s dispatch table entries at `0x3001d4c` to find the per-region mode code
   that precedes the CBP read, and what the non-`0x03005650` handlers consume.

   **Progress on this (same session):** pulled the 24-entry jump table at `0x3001d4c` directly from the
   IWRAM dump (`/tmp/shrek_play_i1.bin`, offset `addr-0x03000000`; the Ghidra bridge was timing out —
   raw file read is more reliable, see workaround notes) — handler addresses:
   ```
    0: 0x03001b10   1: 0x03001c84   2: 0x03001cb0   3: 0x03001b40   4: 0x03001b70   5: 0x03001b7c
    6: 0x03001b88   7: 0x03001b98   8: 0x03001b1c   9: 0x03001b4c  10: 0x03001b28  11: 0x03001b58
   12: 0x03001ba8  13: 0x03001cdc  14: 0x03001d14  15: 0x03001be4  16: 0x03001c20  17: 0x03001c34
   18: 0x03001c4c  19: 0x03001c68  20: 0x03001bb4  21: 0x03001bf0  22: 0x03001bc0  23: 0x03001bfc
   ```
   Disassembled handler 0 (`0x03001b10`, shared tail with handlers 8/10 at `0x03001b34`): it loads
   **pairs** of values from `[r0-0x20]/[r0-0x1c]`, `[r0-0x18]/[r0-0x14]`, or `[r0-0x10]/[r0-0xc]`
   depending on which of the three entries dispatched here (top-left/top/top-right neighbor slots —
   classic median-of-3 MV-predictor shape, matching §4's existing MV note), sets `r4=0x10`, then calls
   a **different** function (`0x03001578`). Handler 4 (`0x03001b70`) just calls `0x03001ac0`. Handler 5
   (`0x03001b7c`) sets `r4=0x10` and calls `0x03001854`. **None of these tail-branch directly into
   `0x03005650`** — this is a genuine prediction-mode layer (MV selection / intra mode), structurally
   separate from the CBP+coefficient loop, not a thin pass-through. Bigger sub-investigation than
   expected; needs `0x03001578` / `0x030015a4` / `0x03001ac0` / `0x03001854` decompiled next, plus
   the remaining ~20 handler entries, to find where/how control eventually reaches `0x03005650` for a
   given region (possibly indirectly, e.g. a shared continuation after all handlers return).

   **RESOLVED (next checkpoint, same investigation continued):** `xrefs` to `0x03001dac` shows exactly
   ONE caller: `0x0300061c`, inside `FUN_03000520` — the function the doc already knew about as "QP:
   FUN_03000520 (tables 0x03000680/6c8/6f8)". Decompiling it reveals it is actually **the true
   per-frame/per-MB master loop**, not just a QP helper:
   1. Reads bitstream state; if a flag bit (`uVar7 & 0x80000000`) is clear, reads a **QP-delta via
      `FUN_0300076c`** (the same `ue(v)` reader) and rebuilds the dequant constants
      `DAT_03005770/74/78` from tables `0x03000680/6c8/6f8` indexed by the delta. If the flag is set,
      a different (skip/run-length?) path executes instead.
   2. **Double-nested loop over 16×16 MB units** (`piVar5[6]`/`piVar5[7]`, stepped by `-0x10` = 16) —
      this is the whole-frame MB raster scan. **Each inner iteration calls `FUN_03001dac()` exactly
      once per 16×16 MB** (not per 8×8 region as originally assumed).
   3. `FUN_03001dac`'s `ue(v)`-coded mode index (0-23) then either:
      - dispatches to a **motion-compensated-copy-only handler** (handlers 0/3/8/9/10/11, sharing
        the neighbor-pair-load tail at `0x03001b34` → call `FUN_03001578`, a byte-alignment-aware
        block-copy/motion-compensation routine — no filtering/averaging, just realigning a
        possibly-unaligned reference-frame read; matches the doc's median-of-3 MV-predictor note).
        These handlers **return without touching `0x03005650`** — i.e. **INTER-SKIP MBs** (motion
        copy, no residual, no CBP).
      - dispatches to an **intra-style handler** (e.g. handler 5 → `FUN_03001ac0`: averages 2
        neighbor bytes with rounding, reads one more code via `FUN_0300081c`, calls
        `FUN_03001a68`/`FUN_03001a3c` — needs further decompiling).
      - or dispatches to one of the ~31 branch targets (confirmed via `xrefs direction=to` on
        `0x03005650`: call sites `0x03001be0` through `0x03003520`) that tail-jump into `0x03005650`,
        which then runs its own internal loop over the 4 luma 8×8-sub-blocks + Cb + Cr **within that
        one 16×16 MB** (not a whole-frame raster loop as earlier assumed) — each still gated by its
        own `ue(v)` CBP code as already validated in §3b/blocker-6.

   This resolves the region-1 desync's root cause at the architecture level: `decode_vlc3.py`'s loop
   treated **every** 8×8 group as "always followed immediately by another CBP-gated group," but the
   real structure is **MB-scoped** — after a 16×16 MB's coded sub-blocks are exhausted (which for a
   CBP-driving handler means exactly 4 groups of up to 6 sub-blocks, or fewer if the MB is
   smaller/partial), control returns to `FUN_03000520`'s outer loop, which advances to the *next*
   16×16 MB and calls `FUN_03001dac` again — consuming a **fresh mode-select `ue(v)` code**, not
   another CBP code. `decode_vlc3.py` never modeled this MB boundary or the mode-select step, hence
   the desync exactly at the point where region 0 (the first MB, evidently a single-8×8-group MB or
   simply the first of up to 4 CBP groups) ended.
   **Still open:** how many `0x03005650`-groups make up one MB (is it always exactly the 4 luma
   quadrants worth, i.e. one `0x03005650` call per MB covering a fixed 16×16 span, or can it vary?);
   full decode of handler 5's intra chain (`FUN_03001ac0`→`FUN_0300081c`/`FUN_03001a68`/`FUN_03001a3c`)
   and the remaining ~18 undecoded handler entries; and whether `FUN_03000520`'s own QP-delta code
   consumes bits *before* frame position 0 in `stream00.video` (possible off-by-one-codes explanation
   for why region 0 lined up despite skipping this step — worth checking by prepending a QP-delta
   `ue(v)` read before the MB loop in the next simulator revision).

   **FULL 24-ENTRY DISPATCH TABLE MAPPED (this checkpoint)** — disassembled the entire
   `0x03001b10..0x03001d4c` handler region in one pass. The table splits **exactly in half** by a
   beautifully clean rule: **mode index `< 12` → SKIP (prediction only, no residual, immediate
   return); mode index `>= 12` → CODED (same prediction, then falls through to `r11=r12=0x10;
   b 0x03005650` — i.e. `0x03005650` is *always* invoked with a full 16×16 span, confirming 4
   CBP-groups per coded MB: 2×2 grid of 8×8 luma quadrants + their Cb/Cr).** Concretely,
   `coded_index = skip_index + 12` for the same underlying predictor. Underlying predictor types
   (10 distinct, some sharing a copy function with 3 selectable neighbor slots):
   ```
   idx  0/ 8/10 (skip) , 12/20/22 (coded): FUN_03001578, neighbor A/B/C  (top-left/top/top-right MV predictor)
   idx  3/ 9/11 (skip) , 15/21/23 (coded): FUN_030015a4, neighbor A/B/C  (different copy fn — diff block size/ref?)
   idx  4       (skip) , 16       (coded): FUN_03001ac0  (intra-style: averages 2 neighbor bytes, reads 1 more
                                             code via FUN_0300081c, calls FUN_03001a68 + FUN_03001a3c x2)
   idx  5       (skip) , 17       (coded): FUN_03001854  (not yet decompiled)
   idx  6       (skip) , 18       (coded): FUN_03000884, r4=r5=0x10  (not yet decompiled)
   idx  7       (skip) , 19       (coded): FUN_030008fc, r4=r5=0x10  (not yet decompiled)
   idx  1       (skip) , 13       (coded): FUN_03002184 called twice w/ +0x800/+0x400 then -0x800/-0x400
                                             pointer adjust around it (not yet decompiled — direct/temporal mode?)
   idx  2       (skip) , 14       (coded): FUN_030030f4 called twice w/ +0x8/+0x8 then -0x8/-0x8 adjust
                                             (not yet decompiled — much smaller offsets than idx1/13, maybe
                                             chroma-specific or a different geometry)
   ```
   **Notably: none of the copy/predict functions read any bitstream bits themselves** (no `se(v)` MVD
   reads observed anywhere in this dispatch region) — the single mode-select `ue(v)` code appears to
   fully determine the predictor with zero extra signaling (except handler 4/16's one extra
   `FUN_0300081c` read). If confirmed, this codec has **no explicit motion-vector-difference coding**
   at the MB level — just a choice among a handful of fixed spatial predictors. Unusual but plausible
   for a GBA-era, low-bitrate embedded codec.

   **Simulator test of the full per-MB model** (`decode_vlc4.py`, no QP-delta modeled yet): walks
   `stream00.video` as `mode=ue(v); if mode>=12: 4x(cbp=ue(v); decode masked blocks)`. Result against
   real bytes: **MB 0-2 decode cleanly as skip MBs** (`mode=2,1,1`, 3 bits each — plausible, short
   exp-golomb codes for small mode values). **MB 3** is `mode=18` (coded): **group 0 (all 5 sub-blocks)
   and group 1 (1 sub-block) both decode to small, plausible coefficients** — group 2's CBP code then
   comes back `89` (invalid, must be `<64`) at bit 69. This is real progress over the previous
   checkpoint (was failing at the very first CBP read; now 3 clean MBs + half of a 4th group-set
   decode correctly) but still not fully resolved. Hand-brute-forced nearby bit offsets (64-84) same as
   before — no unambiguous resync point jumps out without more ground truth. **Likely culprits for the
   remaining gap, in rough priority order:**
   1. The "4 groups always, r11=r12=0x10" reading of `0x03005650`'s entry parameters may be wrong in
      detail — e.g. the 2×2 quadrant traversal might not be 4 flat sequential CBP reads; re-examine the
      inner/outer loop structure in §3b for a possibly-skipped step between quadrants.
   2. QP-delta `ue(v)` (from `FUN_03000520`, still not modeled) may fire partway through the frame
      (a per-slice/per-row event, not strictly per-MB or frame-start-only) and land exactly here.
   3. Handler `mode=18` maps to `FUN_03000884`/`FUN_030008fc`'s "coded" pairing (idx 18 = `FUN_03000884`
      +residual per the table above) — neither `FUN_03000884` nor its skip counterpart (idx 6) has been
      decompiled; it may consume bits itself (unlike the plain motion-copy handlers, which read none).
   **This is a reasonable checkpoint to pause at** — further progress needs either decompiling
   `FUN_03000884` (most direct next step, since MB 3 specifically uses it) or `FUN_03000520`'s QP-delta
   trigger condition in full.

   **`FUN_03000884` decompiled (next checkpoint) — confirms it's an INTRA MB handler.** Disassembly:
   reads **two** `ue(v)` codes via `FUN_0300076c` (`r11`, `r12`), then dispatches through **two
   separate 4-entry jump tables** (`0x3000864`, `0x3000874`, verified exactly 4 entries each — code for
   `FUN_03000884` itself resumes immediately at `0x3000884`, right after the 2nd table, confirming the
   sizes). Table 1 = `{0x30009b8, 0x03000a04, 0x03000a54, 0x03000ba4}` — these are exactly the
   **luma intra16 prediction addresses already known from §4** (doc's old note "Intra: intra16
   `0x30009b8/0a04/0a54/0ba4/0c08`"). Table 2 = `{0x03000c08, 0x03000d90, 0x03000dd4, 0x03000e44}` —
   `0x03000c08` also matches that same old note, strongly suggesting Table 2 is the analogous
   **4-mode chroma intra prediction** dispatch. So mode 18 (and its skip twin, mode 6) = "intra MB:
   pick 1-of-4 luma16 mode + 1-of-4 chroma mode" (classic H.264-style intra4-mode set), each its own
   small `ue(v)` code — bits my MB-3 test above never consumed.

   **Retested MB 3 with this fix** (mode `ue`, then luma-mode `ue`, then chroma-mode `ue`, *then* the
   4 CBP groups): decoded `luma_mode=2, chroma_mode=0`, then **group 0 now decodes completely** (all 5
   masked sub-blocks, small plausible coefficients) — a full group further than before. **Group 1**
   gets 4 of its 5 blocks right, then the 5th (`Cb`) crashes with a genuine **VLC table-index overflow**
   (computed index `>=3584`, past the real table's end — a different, deeper failure mode than the
   earlier "invalid CBP/mode code" failures, meaning the fix is real progress but a smaller residual
   drift is still accumulating somewhere, likely within group 0's coefficient decode or the CBP/mask
   handling itself rather than the MB-level dispatch).

   **Status**: 3 clean skip-MBs, then a coded MB now getting through mode+intra-submode+CBP-group-0
   entirely and 4/5 of CBP-group-1 before drifting. Each checkpoint has meaningfully narrowed the
   remaining gap. Not yet at a full clean MB-to-MB walk. Next moves, roughly in order of likely payoff:
   (a) re-verify group 0's block-by-block bit accounting by hand against the raw bytes (small
   off-by-a-few-bits errors are the most likely remaining culprit, given how close this now is);
   (b) decompile `FUN_030008fc` (modes 7/19) and the `FUN_03001854`/`FUN_03002184`/`FUN_030030f4`
   families the same way, since a full clean walk needs every mode understood, not just 18/6;
   (c) the still-unmodeled QP-delta condition in `FUN_03000520`.

   **Hand-verification (next checkpoint):** printed the exact bit substrings consumed by every step of
   group 0 and group 1 side-by-side with the decode trace (`bits[a:b]=...`) — group 0 is clean start to
   finish (5/5 blocks). Group 1's crash turned out to be a **simulator artifact, not proof of desync**:
   `idx=3584` is exactly one past my Python table array's length (0-3583), which just threw
   `IndexError` — but the doc's own earlier note says the real ROM keeps going past the table into UI
   string bytes (`"Brightness +1"` etc. at file offset `~0xbbe4`), which real hardware would still
   blindly read as a cell rather than crash. Extended the loaded table to the full 4096-entry range
   (all possible 12-bit indices, dumped fresh from ROM as `vlc_rom_full4096.bin`) and reran: **group 1
   now finishes cleanly too** (its `Cb` block resolves via the byte at file offset `0xbc00` = `0x0032`,
   decoding as len=2/value=3 — a plausible-looking but not clearly "official" cell; doesn't fit the
   otherwise-clean len=n+2/value=n escape-ladder pattern seen elsewhere in the table, so this specific
   cell is flagged as unverified rather than trusted). **Group 2 then decodes `cbp=0` → mask `000000`
   → correctly zero blocks needed (no bits to consume) — a fully legitimate "nothing coded here"
   group.** MB 3 now gets through **3 of its 4 groups** cleanly; **group 4 (last one) still fails**
   (`cbp=2572`, wildly invalid). Net: the remaining gap is now a handful of bits at the very end of one
   MB, not a structural misunderstanding — strong sign the overall model (mode → intra-submodes →
   4×CBP-groups) is correct and what's left is precision, not architecture.
   **Immediate next step**: re-verify group 1's `Cb` block specifically (the one landing on the
   suspect boundary cell) bit-by-bit by hand, since that's the most likely source of the last few bits
   of drift before group 3.

   **CONFIRMED (next pass): the boundary cell is real string data, not a legitimate table entry.**
   Dumped ROM bytes at file offset `0xbc00` (= `idx 3584 * 2 + 0xa000`) directly: `32 00 00 00 42 72
   69 67 68 74 6e 65 73 73 20 2d` = literally the ASCII bytes of `"Brightness +2\0\0\0Brightness -"`.
   So `idx=3584` is unambiguously outside the real table — this is a genuine (if small) desync, not a
   harmless extended-table read as hoped. **But**: cross-checked all four of group 1's preceding
   `Y0-Y3` decodes against the canonical block-range table already on file in §2, and every single one
   lands exactly inside its documented block (idx 11→`0x000-0x011`, idx 2905→`0x972-0xb71`, idx
   1438→`0x572-0x971`, idx 3312→`0xc72-0xd71` — all correct, matching the exact `{len,value,run,more}`
   already catalogued there). So the drift is not accumulated error from group 1's luma blocks; it's
   localized to right around the `Y3`→`Cb` transition specifically (a handful of bits, since idx 3584
   is only just past the documented tail-end `0xdf1`=3569, not wildly off). Group 0's `Cb` block, by
   contrast, decoded perfectly (idx 792, inside the documented `0x2f2-0x371` block) — so this isn't
   "Cb blocks are handled differently," since group 0 proves Cb decodes fine there. The most likely
   explanation is a small state-dependent effect specific to *this* `Cb` occurrence — possibly tied to
   the still-undecoded `PTR_LAB_03005788`/`PTR_LAB_0300577c` post-decode dispatch jump tables (§3b's
   last open item — what they select was never actually resolved, only guessed at as "probably
   block-position/plane"), which could plausibly involve a per-call state increment (e.g. an alternating
   dequant-table index, or a small skip/align step) that only manifests on some calls. **Next concrete
   step**: decompile the jump targets of `PTR_LAB_03005788`/`PTR_LAB_0300577c` (referenced at the end
   of `FUN_03005794`, addresses not yet pulled) to check for exactly this kind of call-count-dependent
   side effect.

   **RULED OUT (next pass):** the jump index for both `PTR_LAB` tables was actually already known —
   it's the same `r10` plane selector from earlier in §3b (the *original* `r10` gets saved to the
   stack at function entry via `stmdb sp!,{r10,r11,r12,lr}` and reloaded later as `lr` right before the
   dispatch — a quirk of this codebase's register-passing convention, not a new register). So these
   are 3-entry Y/Cb/Cr **pixel-reconstruction** tables:
   ```
   PTR_LAB_03005788 (single-coefficient DC shortcut): [0]=0x03005c9c [1]=0x03005e30 [2]=0x03006044
   PTR_LAB_0300577c (multi-coefficient / full IDCT):   [0]=0x03005a94 [1]=0x03005b2c [2]=0x03005be4
   ```
   Disassembled `0x03005c9c` (index 0, the path `Y3` actually took — it decoded as a single
   coefficient): it's pure pixel arithmetic — unpack 4 packed bytes, add `DC>>6`, clamp to `[0,255]`,
   repack, store, repeat for the next 3 rows. **No bitstream reads anywhere in it.** This rules out
   the "post-processing consumes extra bits" hypothesis entirely — these routines only touch already-
   decoded coefficients and pixel buffers, they can't be the source of a bitstream-position bug.

   **Where this leaves things**: 9 of the MB's first 10 sub-blocks (all of group 0, and `Y0-Y3` of
   group 1) decode to indices that land exactly inside their correct, previously-documented canonical
   table ranges — about as strong a validation as is possible without hardware/emulator ground truth.
   The one remaining failure (group 1's `Cb`) has no found explanation in the architecture, dispatch
   tables, or post-processing — it now looks like either a genuinely subtle, localized bug in this
   Python port of `FUN_03005794`'s loop (worth a fresh, skeptical line-by-line reread of
   `decode_vlc2.py`'s `decode_one`/`decode_block` against the disassembly one more time), or something
   that can only be resolved with real ground truth — e.g. running the ROM in mGBA with a debugger/
   tracer (the doc already notes an unused `tools/gba_video/gdbrsp.py` GDB stub setup from an earlier session) to catch
   the actual register state at this exact point in a real playthrough, rather than continuing to
   guess blind from static analysis alone.

   **MAJOR FINDING (next pass) — the `more`/stop-bit polarity may be inverted, and it's a genuine
   unresolved contradiction, not a settled fix.** Re-disassembled `0x03005810-0x03005818` fresh
   (`str r6,[r12],#0x4; tst r4,#0x1; beq 0x030057cc`) — `0x030057cc` is unambiguously the loop's
   per-coefficient entry point (re-checks the escape condition, decodes another coefficient). `beq`
   branches when `(r4&1)==0`. So **bit15==0 means "decode another coefficient", bit15==1 means
   "stop"** — the *opposite* of the field name "more" that's been used since the very first session
   (an early, never-re-verified guess baked into §1/§3 above and every simulator since). This is
   independently corroborated by Ghidra's own decompiler output for this same function, captured
   earlier this session in §3: it rendered the loop as a literal `do { ... } while (uVar4 == 0);` —
   same polarity, from a completely different (semantic, not manual-syntax) analysis path.

   **However**, patching `decode_vlc2.py`'s stop condition to match (`if more: break` instead of
   `if not more: break`) and rerunning MB 3's group 0 produces a **14-coefficient chain** for what was
   previously the clean, validated `Y1` block — `run` values accumulate to a cumulative position past
   15 (the last valid slot in the 16-word coefficient buffer `DAT_030056f0..DAT_0300572c`), which would
   silently corrupt adjacent memory (`DAT_03005730` onward — the IDCT/dequant constants) on real
   hardware. That's implausible for a shipped, working decoder, and contradicts the very strong prior
   evidence (9 consecutive blocks, all independently landing in correct canonical index ranges) that
   the *original*, uninverted polarity was producing structurally sound, plausible 1-2-coefficient
   blocks. **Both readings are backed by real evidence and both have a real problem**: the original
   polarity has no support in the disassembly/decompiler; the corrected polarity produces
   buffer-overflowing coefficient runs. This needs to be resolved by one of:
   1. Very careful pcode-level re-examination of whether something clobbers `r4` between the escape
      check and the `tst` in ways not yet accounted for (checked `FUN_030007a8`'s register clobbers
      already — it doesn't touch r4 — but the *escape*-path variants' own joins into `0x0300580c`
      haven't been individually re-verified against this specific concern).
   2. Checking whether the 16-word buffer is really meant to be strictly 4×4/16 slots per call, or
      whether `FUN_03005794` can legitimately be walking a *longer* logical scan (the doc's very first,
      early-session characterization of this function as "residual 4×4 block" was itself a guess made
      before any disassembly existed — worth treating as unconfirmed rather than settled, same caution
      that applied to the `more` field name).
   3. Real ground truth: running the ROM in mGBA with the unused `tools/gba_video/gdbrsp.py` GDB stub from earlier
      sessions and single-stepping this exact loop on real data would settle this immediately, rather
      than continuing to infer from static analysis where two legitimate methods now disagree.
   **Left as-is for now**: reverted `decode_vlc2.py` back to the original (pre-"fix") polarity, since
   it's the one with actual empirical support (9 validated blocks) even though it lacks disassembly
   backing — flagged clearly as unresolved rather than silently kept.

   ### RESOLUTION + TWO CORRECTIONS TO EARLIER CLAIMS (2026-08-07 pass)

   **(A) A real bug in the Python port, found and fixed: the missing post-increment.** The ARM is
   ```
   0300580c: add r12, r12, r5, lsl #0x2   ; cpos += run
   03005810: str r6, [r12], #0x4          ; coeffs[cpos] = value ; cpos += 1   <-- POST-INCREMENT
   ```
   Every simulator revision did `cpos += run; coeffs[cpos] = value` and **never advanced past the
   written slot**, so each subsequent coefficient's run was interpreted relative to the wrong origin.
   Note this does *not* change bit consumption at all — only which slots values land in and whether
   the 16-word buffer overflows. Fixed in the rebuilt `vx.py`.

   **(B) The stop-bit polarity: the disassembly was right; my counter-argument was wrong.** Bit 15 is
   a **`last` flag** (stop when SET) — textbook MPEG-4/H.263 run/level/last RLC, which is exactly what
   this codec's structure otherwise looks like. The "but that overflows the buffer" objection recorded
   above was **not** valid evidence against it — it was reasoning from a simulator that had bug (A).
   Renamed `more` → `last` throughout `vx.py`. **The inherited field name `more` in §1/§3 above is
   wrong and should be read as `last` everywhere in this document.**

   **(C) CORRECTION — most of the "validation" claimed in the checkpoints above is statistically
   near-vacuous, and this document previously overstated it.** Quantified this pass:
   - *"9 consecutive blocks landed in their correct canonical index ranges"* (used above as "about as
     strong a validation as is possible without ground truth"): the documented blocks tile
     `0x000..0xdf1` = **87.2% of the whole 12-bit index space**, so 9 random indices all landing in
     *some* documented block happens **29% of the time by pure chance**. This was weak corroboration,
     not strong validation. **Do not build on it.**
   - *"N MBs decoded successfully"* as a metric is **degenerate**. The stream is high-entropy
     (47.8% one-bits ≈ random), and `ue(v)` makes the cheapest codes 1 bit (`mode 0`) and 3 bits
     (`modes 1-2`) — all of which are *skip* modes that consume no further bits. So random noise
     "decodes" as a long run of skip MBs. Measured against 200 trials of **pure random data**: median
     run = 11 MBs, mean 15.7, and **≥44 MBs occurs in 5% of random trials**. The real stream reaching
     44 MBs is therefore only ~p=0.05 — suggestive, not proof.
   - Consequence: the polarity conclusion in (B) rests on **the disassembly and Ghidra's decompiler
     agreeing independently**, which is solid. It does *not* rest on the 44-vs-3 MB comparison, which
     is weak. Both facts are worth keeping straight.

   **(D) The real remaining blocker is the frame entry point, not the coefficient layer.** Decoding
   `stream00.video` from bit 0 yields **43 of the first 44 MBs as near-empty skip MBs (1-3 bits each)
   and only ONE coded MB.** For the **first frame of a video**, which must be essentially all-intra,
   that is implausible — it is the signature of parsing noise, not structure. Additional evidence:
   sweeping the start offset over bits 0..71, **every** offset converges to the same failure wall at
   **bit 222** and yields exactly 1 coded MB, i.e. the decoder self-syncs into the same degenerate
   skip-run regardless of where it starts. Conclusion: **we are not starting at real MB data.** The
   4 bytes `69 04 dd 84` at the head of the video region (called "the header" in §4) are being fed
   straight into the MB loop. Next session should establish the actual frame/MB entry point before any
   further coefficient-level work — everything downstream is unverifiable until then. Candidates:
   a per-frame header of unknown length; frame 0 not actually starting at video-region offset 0
   despite the seek table's `frame 0 -> video bit 0`; or `FUN_03000520`'s pre-MB-loop reads
   (the QP-delta path, still unmodeled) consuming a frame header first.

## 6. Immediate next steps (pick up here)

> **READ FIRST (2026-08-07):** see §5 blocker #6 parts (A)-(D). Two corrections land there: bit 15 is
> `last` (stop-when-set), **not** `more`; and much of the "validation" recorded in earlier checkpoints
> is statistically near-vacuous (quantified against random-data null tests). The current top blocker is
> the **frame entry point** (D), not the coefficient layer.
>
> **Rebuilding artifacts after a reboot:** `/tmp/gbavx/*`, the scratchpad, and `/tmp/shrek_play_i1.bin`
> are all ephemeral and were lost once already. The ROM is the durable source; everything except the
> IWRAM dump rebuilds from it:
> ```bash
> ROM="<roms>/Game Boy Advance Video - Shrek + Shark Tale (USA) (Rev 5).gba"
> mkdir -p /tmp/gbavx
> dd if="$ROM" bs=1 skip=$((0xa000)) count=8192 of=/tmp/gbavx/vlc_rom_full4096.bin    # VLC table (4096 cells)
> dd if="$ROM" bs=1 skip=$((0xc000)) count=128  of=/tmp/gbavx/value_offset_table.bin  # escape: value offsets
> dd if="$ROM" bs=1 skip=$((0xc080)) count=128  of=/tmp/gbavx/run_offset_table.bin    # escape: run offsets
> dd if="$ROM" bs=1 skip=$((0x20200+0x38)) count=262144 of=/tmp/gbavx/stream00.video  # verified: starts 6904 dd84
> ```
> The **IWRAM dump is NOT rebuildable from the ROM** (it was a live RAM capture) — re-dump from mGBA if
> further disassembly of the `0x03xxxxxx` functions is needed. Note the Ghidra MCP bridge dropped out
> mid-session; see the `ghidra-mcp-bridge-workaround` notes for the direct-HTTP fallback.

Steps 1-3 below are DONE this session — see §3, §3b, and blocker #6 in §5. Simulators:
`tools/gba_video/vx_sim.py` (formerly decode_vlc2.py)
(single 4×4 block, full VLC+escape logic) and `decode_vlc3.py` (full 8×8-region loop: `ue(v)` CBP +
permute + masked blocks) — `decode_vlc3.py` bit-exactly validates region 0 of `stream00.video` before
desyncing at region 1. Copy these out of the scratchpad (session-ephemeral) if picking this up later —
`/tmp/gbavx/*` may also not survive a reboot; the ROM file itself is the durable source, everything else
is re-derivable from it via the file offsets recorded in §2/§3b.

1. ~~Validate the codebook interpretation~~ DONE.
2. ~~Resolve block order~~ DONE — indices `0x060-0x07F` are dead due to the escape check, not misordering.
3. ~~Decompile caller chain 0x3005650~~ DONE — see §3b.
4. ~~Decompile FUN_03001dac's jump table~~ DONE, and **superseded** — see §7. The static-analysis-only
   desync chased in earlier checkpoints turned out to trace to two real bugs (wrong byte order, wrong
   table address), not a missing per-region dispatch layer. Fixed and verified live against mGBA.
5. Once per-MB dispatch is fully understood (2 of the ~10 handler families still need live verification
   — see §7's open items), extend `vx_sim.py` to walk the full first frame and confirm no further
   desyncs.
6. Reassemble the full symbol table `{code, len, run, value, more}` for `vx.c`.
7. Implement `libavcodec/vx.c` modeled on `mobiclip.c`; validate against all 4 streams.

---

## 7. mGBA live ground-truth session (2026-08-07) — two real bugs found and fixed

Static analysis alone had stalled (see §5 blocker #6's long back-and-forth). This session used mGBA's
GDB stub (`-g`, port 2345; client = `tools/gba_video/gdbrsp.py`) to get actual hardware register/memory
state instead of continuing to guess from disassembly. That immediately paid off — **two real, distinct
bugs were found and fixed**, and the very first coefficient of frame 0's first coded macroblock now
decodes to a value that matches live hardware exactly.

### Workflow notes (useful for next time)

- `mGBA -g <rom>` does **not** halt at boot — the CPU runs freely from power-on, and the GDB stub just
  becomes reachable. Connecting before the cart has booted past its splash/menu into actual playback
  means breakpoints on decoder functions won't fire for ~15-25 real seconds; wait or the `cont()` call
  times out.
- Each Python client process should do **one continuous script per emulator boot**. Disconnecting and
  reconnecting does not pause the emulator, so a second script attempting to re-arm a breakpoint that
  already fired once this boot (e.g. `FUN_03006df8`, the once-per-video "main player entry") will hang
  forever waiting for an event that already passed.
- Launching mGBA: `nohup ... -g "$ROM" > log 2>&1 < /dev/null & disown` in its own shell call, `pkill -f
  mGBA` in a separate prior call — combining launch+wait+use in one Bash call was unreliable in this
  sandboxed environment (backgrounded child got reaped).
- `gdbrsp.py`'s `bp()`/`cont()`/`rmbp()` pattern (breakpoint at a call site, continue, capture regs,
  remove, breakpoint at the return address `lr`, continue again) works well for capturing a function's
  input/output state. Pure single-stepping (`r.cmd('s')`) is more robust for anything that needs to
  distinguish a real function return from a call into a shared subroutine (e.g. the bit-refill helper
  `FUN_030007a8`, which `FUN_03005794` calls internally — a naive "PC left the function's address range"
  check false-positives on it).

### Bug 1 — byte order: the accumulator is filled from little-endian HALFWORDS, not raw bytes

`FUN_0300076c`/`FUN_030007a8` refill the bit accumulator via `ldrh r10,[r1],#2` — a little-endian
16-bit load — then treat the result MSB-first. That means every 2-byte pair from the file is
**byte-swapped** relative to naive in-file-order MSB-first reading. Every simulator this session and
prior ones built the accumulator by reading raw file bytes as one continuous MSB-first stream, which is
wrong at every halfword boundary.

Proof: the stream's first two bytes are `69 04`. Loaded as a halfword that's `0x0469`. Decoding `0x0469`
as `ue(v)` (5 leading zeros, stop bit, 5-bit suffix `00011`=3, value = 3 + 31 = **34**) matches a live
mGBA capture of the very first bitstream read of the video (the QP-delta in `FUN_03000520`) exactly.
Naive raw-byte-order MSB-first decoding of the same two bytes gives **2** — wrong.

Fixed in `tools/gba_video/vx_sim.py`'s `Bits` class: precompute a per-halfword-swapped copy of the input
once in `__init__`, so `peek32()` stays a simple MSB-first byte reader over the swapped buffer.

### Bug 2 — wrong codebook table address: it's a runtime EWRAM copy, not the static ROM location

Every session before this one (going back to the very first) assumed the VLC/escape codebook (`fp` in
`FUN_03005794`) was a **flat ROM address**, `0x0800a000`. That assumption was never actually verified
against a live `fp` value — it was inferred once, early on, from a decompile note ("Writes `sl +
0x08000000` into `[ctx,#0x20]`") and never rechecked.

Live capture at the entry to `FUN_03005794` (breakpoint at `0x03005794`, then `fp = *(r0+0x20)` read via
`r.mem`) gives **`fp = 0x020010e4`** — an **EWRAM** address (`0x02000000-0x0203FFFF` range), not a ROM
address at all. The codebook is copied into EWRAM at runtime (presumably during video setup, from
wherever it actually lives in the bank-switched ROM — not investigated further, doesn't matter for the
simulator). Every table lookup this session and all prior ones was reading the *wrong table entirely*.

Proof: `table[89]` from the old (wrong, static-ROM) dump was `0x0098`. The live EWRAM table's `table[89]`
is `0x808d`. Single-stepping the real ARM code for this exact lookup (escape variant 1, at the very
first coefficient of frame 0's first coded MB) confirms hardware genuinely reads `0x808d` there — and
decoding it (`len=13, base value=8, value_offset_table[64]=8` → combined `16`, sign bit set → **-16**,
`run=0`, `last=1`, single coefficient) matches a live single-stepped hardware trace of that exact
coefficient **exactly** (value, run, and the stop condition all confirmed bit-for-bit).

Also confirms, as a side effect: the `last`-flag polarity from the (B) correction in §5 blocker #6 is
**right** — real hardware's `tst r4,#1; beq <loop start>` genuinely stops (branch not taken) when the
bit is 1, matching `stop_when_set=True`. That conclusion survives; the buffer-overflow objection raised
against it earlier was itself caused by Bug 2 producing garbage table lookups downstream, not a real
problem with the polarity.

**Live table + escape tables saved**: dumped fresh from EWRAM (`fp=0x020010e4`, `fp+0x2000`, `fp+0x2080`)
and copied over the old (wrong) `tools/gba_video/vlc_rom_full4096.bin` /
`value_offset_table.bin` / `run_offset_table.bin` equivalents in `/tmp/gbavx/` — **these old filenames
now mean the live/correct table**, not the original ROM-address dump. `vofs` should be loaded in full
(128 bytes) now, not truncated to the first 48 as earlier sessions guessed (that truncation was itself
an artifact of reading the wrong, ROM-address table, where bytes past offset 0x30 genuinely were
unrelated code).

**Also confirmed correct, unchanged**: the true stream start really is file offset `0x20238` (container
offset `0x20200 + 0x38`) — this was the *original*, very first session's guess, and it's right. A long
detour this session chasing an apparent "bank-switched" ROM location (`0x2ca3a`, `0x103a`+something) was
a red herring from a stale mid-stream GDB capture, not evidence against the container-offset extraction.
Re-verified by direct byte search of the flat ROM for the confirmed-live first 16 bytes
(`69 04 dd 84 02 03 18 cc 60 16 b3 c0 05 06 d4 99`) — unique hit at `0x20238`. The CBP permutation table
at `0x3005610` was also independently re-checked live and is unchanged/correct (it's static IWRAM code,
not runtime data, so it was never suspect the way the two bugs above were).

### Validated this session (live, bit-for-bit against mGBA)

With both fixes applied, a live 40-call trace of real `ue(v)` reads (QP-delta, mode-selects, intra
submodes, CBP codes) for the first several MBs of frame 0 was captured and cross-checked:

```
34, 1, 2, 18, 2, 0, 1, 6, 1, 0, 6, 0, 0, 6, 1, 0, 6, 1, 0, 6, 1, 0, 6, 1, 0, ...
```

`vx_sim.py` (fixed) reproduces the **first 8 of these exactly**: QP-delta=34; MB0 mode=1 (skip); MB1
mode=2 (skip); MB2 mode=18 (coded intra) with submodes luma=2/chroma=0; group0 CBP=1 (mask `0b01111` →
Y0-Y3 coded); group1 CBP=6. And the first coefficient of MB2's group0 Y0 block was verified via full
single-step trace to decode to exactly `-16, run=0, last=1` — matching hardware precisely, including the
escape-variant-1 codepath, the second-level table lookup, and the value-offset-table addition.

### Still open

- **A small, unresolved drift appears after group 1's `Y2` block** (my_vals index 8 diverges from the
  live trace: mine gives `0`, live gives `1`). Chased extensively this session via peek32 comparisons and
  register captures but not root-caused — ruled out: the CBP permutation table (re-verified live,
  correct), the escape-variant math (correct for the coefficients checked), and simple off-by-N-bit
  guesses (tested against real accumulator snapshots, none matched). A live single-step trace specifically
  through group 0's `Y2`/`Y3` blocks and the group-0→group-1 transition would likely resolve this
  quickly, but repeated attempts this session were undermined by GDB-stub flakiness (breakpoints timing
  out on fresh boots, the "bp at lr" pattern hanging intermittently after many prior connections to the
  same long-lived emulator process) and a bug in an ad hoc "did the function return" step-counter (it
  false-positived on the internal call to the refill subroutine `FUN_030007a8`). Next session: prefer
  fresh emulator boots over reusing one across many scripts, and single-step continuously through a
  block rather than trying to detect "returned" from register state alone (watch for `pc` actually
  leaving the full `0x03005650-0x03006050`-ish caller/callee range, or just count exact instruction
  addresses against a known-good disassembly walk).
- Two of the ~10 underlying MB predictor types are still fully unverified even structurally:
  `FUN_03001854` (modes 5/17) and the `FUN_03002184`/`FUN_030030f4` pair (modes 1/2/13/14) — live trace
  this session did confirm mode 1 and 2 (skip variants) consume **zero** extra bits beyond the
  mode-select code itself (contradicting nothing, but not yet explaining what `FUN_03002184`/
  `FUN_030030f4` actually *do* pixel-wise), and did NOT exercise their "coded" (13/14) counterparts.
- QP-delta re-read condition (`FUN_03000520`'s `uVar7 & 0x80000000` flag) still not explained — only
  fired once in the live 40-call trace (at the very start), consistent with "once per frame" but not
  proven for later frames/slices.
- Once the group-1 drift is fixed, extend the live-validated prefix into a full first-frame walk and
  compare MB-by-MB against a longer live trace before touching `vx.c` itself.

## 8. Bitstream grammar solved (2026-08-07, second mGBA session)

The "drift after group 1's `Y2`" recorded in §7 was **not a drift at all** — it was a structural error in
the MB model, and the fix invalidates that entire open item.

### 8.1 There is exactly ONE coded-block-pattern per macroblock

The model inherited from session 1 read **four** CBP "groups" per MB. Hardware reads one. The CBP is a
plain 6-bit mask over four 8×8 luma quadrants plus Cb and Cr — the ordinary H.263/MPEG-4 arrangement.
Blocks are therefore 8×8 (64 coefficients), not 16, which also retires the long-standing
"coefficient buffer overflows past 16 slots" symptom: it was never an overflow, the buffer was just
four times too small.

Proof, from the frame-0 live capture already on disk (`/tmp/gbavx/live_trace.json`):

| field | bit pos | value |
|---|---|---|
| QP delta | 0 | 34 |
| MB0 mode | 11 | 1 |
| MB1 mode | 14 | 2 |
| MB2 mode | 17 | 18 |
| MB2 intra luma / chroma | 26 / 29 | 2 / 0 |
| MB2 CBP | 30 | 1 → mask `001111` = Y0..Y3 |
| MB2 Y0..Y3 | 33..117 | 84 bits, one DC coefficient each (`-16, run=0, last=1`) |

The four luma blocks consume exactly 84 bits and land at bit 117 — precisely where hardware's next read
begins. With this model **all 40/40** `ue(v)` reads in that capture match hardware in *both bit position
and value* (previously only the first 8 did).

### 8.2 Frame geometry: 105 MBs = 15×7 = 240×112, letterboxed

A fresh 4000-call trace (breakpoint on `FUN_0300076c`, logging `lr`/`r1`/`r2` per call) shows a 1-bit gap
recurring at a period of exactly 105 macroblocks. The movie is letterboxed inside the GBA's 240×160
screen: 15×7 MBs of 16×16. Frames are separated by a single marker bit.

### 8.3 The QP delta is read ONCE for the whole video

`FUN_03000520`'s QP-delta read (`lr=0x03000734`) hits **exactly once in 4000 calls**. The §7 open question
about its "re-read trigger condition" is therefore moot: there is no re-read, and the earlier
per-frame-QP assumption was wrong.

### 8.4 Per-mode grammar, read directly off call sites

Modes are a 12-entry predictor family plus a `+12 = same predictor, with residual` variant. Side data
keys off the base mode:

| base mode | extra reads after the mode code | call site |
|---|---|---|
| 0,1,2,3,4,5 | none | — |
| 6 | 2 × `ue(v)` (intra luma + chroma submode) | `0x0300088c`, `0x03000894` |
| 7 | 1 × `ue(v)` | `0x030009a4` |
| ≥12 | additionally CBP + coded blocks | `0x0300566c` |

Mode 6/18 and 7/19 confirm the `+12` pairing directly in the trace.

### 8.5 Where it stands

With the above, **938 of 964** modelled hardware calls match exactly in bit position, across 963
macroblocks and 9 frames — up from a model that desynced at frame 5, MB 114. Frames 1–7 are entirely
skip macroblocks (a static title card); frame 8 is where real motion begins.

### Still open

- **Modes 4 and 5 consume extra bits that are not read through `FUN_0300076c`.** The gaps are always odd
  (mode 4: 3,5,7,9,17,19,31; mode 5: 7,11,15,23,25), i.e. exp-Golomb-shaped, but no traced call accounts
  for them — so there is a *second*, untraced exp-Golomb/`se(v)` reader, almost certainly the
  motion-vector path. Finding and breakpointing that function is the single highest-value next step.
- **Coded inter modes (12, 13, 14, 23) over-consume**, by −64 to −171 bits. Since intra-coded residual
  (mode 18) decodes perfectly, the likely cause is a separate inter coefficient VLC table rather than an
  error in the loop itself. Worth checking whether the codebook pointer `fp` differs between intra and
  inter MBs — it is read from `*(r0+0x20)` and could simply be reloaded.
- The frame-marker bit's phase is 4 MBs later than a naive "after every 105th MB" placement (first marker
  falls after MB 108). Modelled empirically; the reason is not yet understood.

### Workflow notes that worked

- Capture is far more productive than single-stepping: breakpoint `FUN_0300076c`, `cont()` + `regs()` per
  hit, ~18 calls/sec, 4000 calls in ~4 minutes. `(r1*8 - r2)` gives an exact bit position per call, and
  the `lr` identifies the call site — together that is enough to recover the grammar without reading any
  more disassembly.
- Compare against the simulator with a **resync-tolerant** differ: at each expected read, record
  `live_pos - sim_pos` as a gap, then force `sim_pos = live_pos` and continue. Attributing each nonzero
  gap to the *preceding* MB's mode turns one desync into a full per-mode table of what is unmodelled.

## 9. The second bit reader: `se(v)` at `FUN_0300081c`

§8 left "modes 4 and 5 consume bits that no traced call accounts for" as the top open item. Resolved, and
the reason it was invisible is worth recording.

A static scan of the IWRAM image (dump it once with the GDB stub — `r.mem(0x03000000, 0x8000)` — and all
further analysis is offline, no emulator needed) shows the `ue(v)` reader `FUN_0300076c` has exactly
**21 call sites**, which is exactly the 21 distinct `lr` values in the 4000-call hardware trace. So the
trace was complete: nothing else calls it, and the missing bits were never `ue(v)` reads at all.

They come from a **second, separate reader at `FUN_0300081c`** — signed exp-Golomb:

```
mov  sl, #0                 ; count leading zeros, as usual
...
add  r6, r6, sb, lsl sl     ; r6 = ue + 1  =: k
tst  r6, #1
rsbne r6, r6, #1            ; k odd  ->  k = 1 - k
asr  r6, r6, #1             ; >>1 (arithmetic)
sub  r2, r2, sl, lsl #1     ; consumes 2n+1 bits, same length as ue(v)
subs r2, r2, #1
```

That is the standard H.264 `se(v)` mapping (0, 1, −1, 2, −2, …). Because it consumes 2n+1 bits, every
gap it produced was **odd** — which is exactly the signature observed in §8 and is what identified it.

### Mode jump table

`FUN_03001dac` dispatches through a 24-entry table at **`0x03001d4c`** (`sub r7, pc, #0x70`):

| mode | handler | side data |
|---|---|---|
| 4 | `FUN_03001ac0` | 3 × `se(v)` |
| 5 | `FUN_03001854` | 5 × `se(v)` |
| 6 | `FUN_03000884` | 2 × `ue(v)` (intra luma/chroma submode) |
| 7 | `FUN_030008fc` | 1 × `ue(v)` |
| 16, 17 | = 4, 5 `+12` | same, plus residual |

Note the static call-count walk gives *upper bounds* only — it flattens conditional paths, and claims
2 × `se(v)` for modes 0/3/8/9/10/11 which hardware shows consume nothing. Hardware wins; use the walk to
generate candidates, not conclusions.

With `se(v)` modelled for modes 4 and 5, **956 of 964** hardware calls match exactly in bit position.

### Still open

- **Coded inter modes (12, 13, 14, 23) over-consume**, by −64 to −171 bits — my model reads *more* than
  hardware. Intra-coded residual (mode 18) is perfect, so the coefficient loop itself is right; the
  likely cause is a separate inter coefficient VLC table. The codebook pointer is loaded from
  `*(r0+0x20)`; check whether inter MBs reload it from a different offset. This is now the only
  structural unknown blocking a full-stream parse.
- The two remaining single-occurrence gaps (mode 4: −4, mode 5: −10) are almost certainly collateral from
  the coded-inter desync immediately upstream, not separate defects — 14 of 15 mode-4 and 4 of 5 mode-5
  MBs are now exact.

## 10. Correction to §8: there are FOUR CBP groups, and the mode labelling was wrong

**§8's headline claim — "exactly ONE coded-block-pattern per macroblock" — is wrong.** It is retracted
here. The original session-1 model's *four* CBP groups was right about the group count all along; §8
"corrected" a correct thing. Recording the mistake and how it happened, because the failure mode is
subtle and will recur otherwise.

### What is actually true

Every coded-mode handler in the jump table ends with:

```
push {fp, ip}
mov  fp, #0x10
mov  ip, #0x10
b    #0x3005650          ; the residual loop
```

and that loop is nested: the inner loop decrements `fp` by 8 (2 iterations), the outer decrements `ip`
by 8 (2 iterations). **Four CBP reads per coded MB**, each a 6-bit mask over four 8×8 luma quadrants plus
Cb/Cr.

Confirmed independently against hardware. Segmenting the 4000-call trace strictly at the *real* mode
reader and listing the call sites that follow each mode:

| mode | calls following the mode code | n |
|---|---|---|
| 6 | `intraL intraC` | 155 |
| 7 | `ue7` | 6 |
| 15, 16, 17 | `CBP CBP CBP CBP` | 4, 1, 9 |
| 18 | `intraL intraC CBP CBP CBP CBP` | 2 |
| 19 | `ue7 CBP CBP CBP CBP` | 3 |

### How §8 went wrong

`FUN_0300076c` has 21 call sites. **Only one of them — `lr=0x03001db4`, in `FUN_03001dac` — reads a
macroblock mode.** The other 20 are predictor helpers that read side data. §8 treated four sites
(`0x03001db4`, `0x0300218c`, `0x0300358c`, `0x030030fc`) as mode reads because the values they returned
happened to fall in the valid mode range.

Concretely: frame 0's "MB2 with mode 18" was never a macroblock. The value 18 was read at
`0x0300358c` — a helper inside `FUN_03003584`. With that read mislabelled as a mode, the single CBP that
followed looked like "one CBP per MB", and the 84-bit four-block decode that landed exactly on the next
read looked like decisive confirmation. It was a coincidence of code lengths.

**The lesson:** matching bit *positions* validates only the model's sequence of code *lengths*. It does
not validate what those codes mean. A grammar can be positionally perfect and semantically wrong, and
§8's "40/40 match" was exactly that. Derive semantics from call-site identity (which the trace gives for
free in `lr`), never from "the value looks plausible for this field".

### What survives from §8, and what does not

Survives — these were established from the all-skip frames, where every macroblock is a single mode read
at `0x03001db4` and segmentation is therefore unambiguous:

- 105 macroblocks per frame, with a 1-bit inter-frame marker.
- The QP delta is read once for the whole video (exactly one hit in 4000 calls).
- Blocks are 8×8 with 64 coefficients.
- `se(v)` at `FUN_0300081c` and the §9 side-data counts (those came from the jump table plus per-mode
  gap analysis, not from the mislabelled segmentation).

Does not survive:

- "One CBP per MB" — it is four.
- The "956/964 exact positions" figure. That score belongs to a parser with known-wrong mode labelling;
  it is not evidence the grammar is right. Switching to four groups scores *worse* (929/950) precisely
  because the labelling is still wrong — both numbers describe broken models and neither should be
  quoted as progress.

### Next step

Rebuild the parser around call-site identity rather than an assumed grammar: segment strictly at
`0x03001db4`, then work out the read conditions inside the three helper families that appear in the
trace — `FUN_03002184`, `FUN_030030f4`, `FUN_03003584`. They read *conditionally* (mode 1 is followed by
`h2184 h2184` sometimes and by nothing at other times), so their predicates are the remaining unknown.
That is the whole of what stands between here and a correct full-stream parse.

## 11. Solved: the codec is a recursive block partition (4000/4000 hardware calls)

The grammar is now fully recovered and validated end to end. §10's "next step" is done.

### The structure

VX++ is **not** a flat macroblock loop. It is a **recursive block partition** driven by **16 jump-table
dispatchers**, each of which reads a mode with `ue(v)` and expands to further reads — including recursing
into smaller dispatchers. Found by pattern-scanning the IWRAM image for the dispatch idiom:

```
bl   #0x300076c          ; read mode with ue(v)
sub  r7, pc, #<imm>      ; r7 = jump table base
ldr  r6, [r7, r6, lsl #2]
bx   r6
```

The 16 dispatchers account for 16 of the `ue(v)` reader's 21 call sites; the other five are the one-time
QP delta, the two intra submodes, the intra-4×4 helper, and the CBP read. Top-level and second-level
tables have 24 entries; the deeper ones have 12 (verified — the words preceding each table are not code
pointers, so they are not larger tables).

Modes 1 and 2 are the partition operators: each calls a sub-dispatcher **twice**, adjusting the
destination pointers by ±0x800/±0x400 (vertical split) or ±8 (horizontal split) in between. Modes
`n` and `n+12` share a predictor; the `+12` form additionally decodes a residual.

### The last missing reader: intra 4×4 modes

`FUN_030008fc` reads, for each 4×4 sub-block of an `r4`×`r5` region, a flag bit: **1 bit if the predicted
mode is reused, otherwise 4 bits** (the flag plus a 3-bit remainder, incremented if it is ≥ the
prediction) — H.264's `prev_intra4x4_pred_mode_flag` / `rem_intra4x4_pred_mode`, exactly. Only *after*
that loop does it call `ue(v)`. Those raw bits are invisible to a breakpoint trace of the `ue(v)` reader,
which is why they were the final unexplained gap.

### Validation

A generated grammar (`tools/gba_video/vx_grammar.py`, 240 table entries) plus a recursive parser
reproduces **4000/4000 `ue(v)` calls** from the mGBA trace — every call, exact in **both** bit position
and originating call site. This is a much stronger check than position alone: matching the call site
proves the parser is in the right *function* at the right time, which is precisely what §10 showed
position-matching alone cannot establish.

Two practical notes that made this possible:

- The decoder reads the bitstream through a **2 KB circular window** at `0x08001000-0x080017ff`. The
  pointer wraps once per 2 KB; unwrapping it (add 2048 bytes per wrap) recovers a linear bit position and
  extends validation from 2430 calls to the full 4000.
- Static call-graph walking gives *upper bounds* on `se(v)` counts and needs a call-depth limit of 2 to
  match reality. Three entries were still wrong and were corrected by hardware calibration: search for the
  `se(v)` count that makes the next traced call land in the right place. Static analysis proposes,
  the trace disposes — all three corrections converged on "mode 4 reads 3 `se(v)`", consistent with every
  other table.

### Still open

- The parse runs cleanly for **26 frames** and then desyncs at bit 41590 (byte 5198), reading a mode of 16
  from a 12-entry table. That is well past the 4000-call trace (which covers to byte 3267), so it is
  undiagnosed rather than contradicted. A longer capture is the way in — the capture loop runs at
  ~18 calls/sec, so ~20k calls is a 20-minute run.
- The `resid` group counts come from the `fp`/`ip` immediates at each `b #0x3005650` site and are only
  exercised for a subset of modes in the current trace.
- Nothing here touches reconstruction — prediction, dequantisation and the inverse transform are still
  entirely unexamined. This is a *bitstream* parser; a decoder needs all three.

## 12. The container header is a better oracle than the emulator

`gbavx_extract.py` was already parsing the `VX++` stream header, and it answers — offline, with no
emulator — two questions this document spent sessions inferring.

### Frame geometry, confirmed

```
[0] 0x00020200  240x112  34874 frames  7.00 fps  16384 Hz  20 chapters
```

**240×112** — exactly 15×7 macroblocks. The `MB_PER_FRAME = 105` inferred in §8 from the period of a
1-bit gap in a hardware trace is independently correct. (Streams 2 and 3 are 240×160, so the letterboxing
is per-stream, not a property of the codec.)

### The seek table is a bit-exact frame index

The header carries a 181-entry seek table whose second field is a **bit offset**:

```
(frame, bit, audio_off, 0)
(    0,      0,   3124, 0)
(  154, 388377,  41464, 0)
(  325, 523291,  89162, 0)
```

Parsing forward from bit 0, **my frame boundary 153 lands on bit 388377 — exactly the table's frame 154**
(the table is 1-based). That is 154 consecutive frames, ~16,000 macroblocks, validated against an oracle
that has nothing to do with the emulator or with how the grammar was derived.

This is now the primary regression check: it is free, offline, and covers far more of the stream than any
GDB trace. Only three entries fall inside the extracted 256 KB, but that is still two full seek intervals.

### Uniformity as a bug-finder

Tabulating each mode's `se(v)` count across all sixteen dispatcher tables showed mode 4 disagreeing in
four of them. Reading those handlers confirmed the majority: mode 4 reads **three** `se(v)` everywhere —
one directly, one in each of two paired sub-helpers, with a third callee that is pure pixel averaging and
reads nothing. Fixing it took the parse from 26 frames to **324**.

The whole grammar is now internally consistent, which is itself evidence:

- Every mode has one `se(v)` count across all tables: `0,0,0,2,3,5,0,0,0,2,0,2` for modes 0-11.
- Modes 12-23 are exactly modes 0-11 plus a residual — the `+12` symmetry holds without exception.
- The partition tree is coherent: 16×16 (4 CBP groups, 16 intra sub-blocks) → 16×8 and 8×16 (2 groups,
  8 sub-blocks) → 8×8 (1 group, 4 sub-blocks) → 4×4 and below, which carry no residual at all. Only the
  12-entry tables lack coded modes, exactly as they must.

### Still open: a drift between frames 154 and 325

The parse is exact to frame 154 and then desyncs 59 macroblocks into frame 324, at bit 523291 — which is
precisely the seek table's offset for frame 325. So the error accumulates somewhere in that 171-frame
window.

Ruled out so far:

- **All three coefficient escape paths**, checked instruction by instruction against `0x030059b4`
  onward: escape detection (top 7 bits == 3), the bit-24/bit-23 variant selection, the 8/9/9-bit
  prefixes, the value-offset table at `fp+0x2000` indexed by `cell>>9`, the run-offset table at
  `fp+0x2080` indexed by `last*64 + value` (computed *before* the value is negated), and the raw literal's
  9+1+6+12 layout with an arithmetic shift for the signed 12-bit value. All correct as implemented.
- **Any single wrong grammar constant**: an exhaustive search perturbing every table entry's `se(v)`
  count by ±1 and every `resid` group count over {1,2,4} finds nothing that improves on the baseline.

So the defect is conditional or contextual rather than a constant — which is why a per-*frame* breakpoint
(`FUN_03000520`, called once per frame from `0x03006dc8`) is the right next probe: ~400 hits instead of
the ~80,000 needed to reach frame 325 by tracing every `ue(v)` call.

## 13. Solved: the whole movie parses bit-exactly

The §12 drift is fixed, and the cause was a wrong claim in §8, not a wrong grammar.

### The quantiser delta is per-segment, not per-video

§8.3 stated the QP delta is "read ONCE for the whole video", on the evidence that
`lr=0x03000734` hit exactly once in a 4000-call hardware trace. That trace covered about 30 frames — all
of them inside the first seek segment. The field is actually read **once per seek segment**.

Each of the seek table's 181 entries begins a self-contained segment: a `ue(v)` quantiser delta, then that
segment's frames. Bit 0 is simply segment 0, which is why the opening read looked like a stream header.

### Result

With a `ue(v)` consumed at each seek point, **all 180 segments of stream 0 parse bit-exactly** — every
segment lands precisely on the next segment's recorded bit offset, and the final segment ends 370 bytes
from end-of-file (trailing padding). That is the complete **34,874-frame, ~48-minute** movie.

### What made this findable

Two things, neither of which needed the emulator:

- **The working stream file was a truncated 256 KB prefix** of a 22.5 MB stream. Re-running the extractor
  produced the real file and made all 181 checkpoints reachable. (The truncation was not the bug — the
  failure was at byte 65411, well inside the prefix — but it had capped the oracle at three checkpoints.)
- **Parsing each seek interval independently**, rather than as one long chain, converted a single opaque
  desync into 180 isolated experiments. Every interval then failed *immediately at its own start* with the
  same error, which points straight at "something is consumed at a seek point" — a much sharper signal
  than one failure 324 frames deep.

### Method note

Both §10's retraction and this one share a root cause: generalising from a sample that could not
distinguish the hypotheses. "One CBP per macroblock" came from a trace where a helper's value was
mistaken for a mode; "QP once per video" came from a trace one segment long. In both cases the evidence
was real and the inference overreached it. The seek table is a better class of evidence precisely because
it is independent of the decoder disassembly, the emulator, and the reasoning that produced the grammar —
and it covers the entire stream rather than its first few seconds.

### Generalisation: all four streams on the cart

The grammar was derived entirely from stream 0 at 240×112. Running it against every stream on the
cartridge, with macroblocks-per-frame taken from each header:

| stream | geometry | MBs/frame | frames | segments exact |
|---|---|---|---|---|
| 0 | 240×112 | 105 | 34,874 | 180/180 |
| 1 | 240×112 | 105 | 34,261 | 171/171 |
| 2 | 240×160 | 150 | 595 | 1/1 |
| 3 | 240×160 | 150 | 707 | 1/1 |

**353/353 segments, 70,437 frames, bit-exact.** Streams 2 and 3 are the meaningful test: 240×160 is a
geometry the grammar never saw, and 150 macroblocks per frame instead of 105 falls out of the header with
no other change. The bitstream layer is done.

### What a decoder still needs

This parses the bitstream and nothing else. Reconstruction is untouched:

- **Prediction.** The mode tree is recovered structurally, but what each of the ~10 predictor families
  actually computes is unexamined, beyond noting that modes 1 and 2 split the block and that the handlers
  are full of pixel-averaging (`ldrb`/`add`/`asr #1`) and half-pel interpolation loops.
- **Dequantisation.** The per-segment quantiser delta is read and discarded; its scaling is unknown.
- **Inverse transform.** Untouched. Blocks are 8×8 with 64 coefficients, and §10 of the ffmpeg9 notes
  suggests the DS-era relatives use a plain H.264-style integer transform, but that is an assumption here.
- **Coefficient scan order.** `decode_block` writes coefficients at `run`-derived positions in raster
  order; the real zig-zag or field scan has not been identified.

## 14. Reconstruction layer: architecture recovered, port started

Building the decoder. What the disassembly says about reconstruction, all verified against the IWRAM
image:

### The codec is H.264-shaped

- **Transform**: the 4×4 integer butterfly (`e2 = (d1>>1) - d3`, `e3 = d1 + (d3>>1)`), four columns at
  `0x03005828`, then a row pass with `+32` rounding and `>>6` in each reconstruct routine.
- **Dequantisation**: `factors[qp % 6] << (qp / 6)`, from tables at `0x03000680` (factors), `0x030006c8`
  (row offset) and `0x030006f8` (shift), set up at `0x03000730`. The factor table is **byte-identical to
  H.264's** — `(10,13,16), (11,14,18), (13,16,20), (14,18,23), (16,20,25), (18,23,29)`.
- **Scan**: standard H.264 zig-zag, confirmed from which coefficient addresses each column of the
  transform loads.
- **Block size**: 4×4. Each CBP bit covers one 4×4 block, and four CBP groups give 16 luma + 8 chroma
  blocks per macroblock.
- **Whole-block intra**: modes 0-3 are vertical / horizontal / DC / plane, with DC taking the usual
  availability fallbacks (`0x80` when neither edge exists).

### Where it differs

Prediction is a **recursive rectangular partition**, not a fixed set of shapes. Each dispatcher handles
one block size, recovered from the `r4`/`r5` immediates its mode-6 handler passes to the intra predictor:

| dispatcher | block | dispatcher | block | dispatcher | block |
|---|---|---|---|---|---|
| `0x03001dac` | 16×16 | `0x030030f4` | 8×16 | `0x030041cc` | 4×16 |
| `0x03002184` | 16×8 | `0x03003584` | 8×8 | `0x03004468` | 4×8 |
| `0x030024ac` | 16×4 | `0x03003818` | 8×4 | `0x03004750` | 4×4 |

Mode 1 splits vertically, mode 2 horizontally, each recursing twice into the next dispatcher down. The
remaining seven dispatchers sit below 4×4 and offer no whole-block intra mode.

Also unlike H.264, **the prediction is written into the frame buffer first and the residual added on top
in place** — `clip(pred + ((f + 32) >> 6))` reading and writing the same address, with a fixed pitch of
`0x100` for both luma and chroma. A macroblock's position travels as a byte offset into that buffer,
`mb_y*16*STRIDE + mb_x*16`, which is why the intra predictors test the offset itself for edge
availability: bits 0-7 are zero exactly when `mb_x == 0`, bits 8-15 when `mb_y == 0`.

### Ported so far (`tools/gba_video/vx_reconstruct.py`)

Dequant tables, zig-zag, the 4×4 inverse transform, in-place residual addition, and intra
vertical/horizontal/DC. Each constant is read from the image rather than assumed.

### Not yet ported

Intra plane mode; the four chroma intra modes at `0x03000874`; the nine intra 4×4 modes at `0x030008d8`;
inter prediction (the predictor families, half-pel interpolation around `0x03001704`/`0x03002a8c`, and the
motion vectors carried as `se(v)`); five of the six reconstruct variants; and reference-frame management.

### Note on the first frame

Frame 0 of segments 0 and 1 is *pure* intra with **no residual at all** — 104 mode-6 macroblocks and one
split. With no residual and no neighbours at the top-left, it reconstructs to flat grey: a title card.
Real picture content needs inter prediction as well, so there is no shortcut to a watchable frame via
keyframes alone — segment 2 onward mixes inter modes into its keyframes.

---

## 15. The emulator is no longer needed, and §3's codebook address was wrong

Everything the parser needs is in the cartridge, statically. Two findings remove the mGBA/GDB stub
from the workflow entirely — which matters, because that stub was the least reliable part of it.

### The decoder image is in ROM: `0xbcc8` == IWRAM `0x03000000`

The decoder runs from IWRAM, but IWRAM is filled from ROM at boot, so the image can simply be lifted.
Searching the ROM for the two-instruction signature of the coefficient loop
(`add r12,r12,r5,lsl #2` / `str r6,[r12],#4`, documented in §11) gives exactly **one** hit, at ROM
`0x114d4`, which puts the image base at ROM `0xbcc8`.

Confirmed three independent ways before being relied on:

- all 16 entries of the mode jump table at `0x03001d4c` are valid IWRAM pointers;
- the dequant factors at `0x03000680` read 10, 13, 16, 11, 14, 18, 13, 16, 20 — H.264's table;
- the reconstruct-variant table at `0x0300577c` yields `0x03005a94`, `0x03005b2c`, `0x03005be4`,
  `0x03005c9c`, `0x03005e30`, `0x03006044` — the exact six addresses earlier sessions recorded live.

`gbavx_extract.py -o` now writes `iwram.bin` alongside the streams, and `vx_dis.py` disassembles any
range of it. Every `0x03xxxxxx` address in this document indexes straight into that file.

### The codebook is at ROM `0x9ae4`, not `0xa000`

§5's "Bug 2" established that the live codebook is not the `0x8000a000` one — it read the true table
from an EWRAM copy at `fp=0x020010e4` and noted the ROM source was "not investigated further". It is
worth investigating: the blob is in ROM verbatim, at **`0x9ae4`**, `0x51c` below the address §3 and §5
assumed.

Located from the two live values those sections preserved — `table[89] == 0x808d` and
`value_offset_table[64] == 8` — which together leave three candidates in the whole 64 MB ROM. Only
`0x9ae4` has all 4096 cells non-zero with no code length above 13; the other two carry 345 and 433
impossible lengths, i.e. they are noise. The layout matches the runtime one exactly: 4096 LE uint16
cells, then the escape tables at `+0x2000` and `+0x2080`. It also sits flush against the IWRAM image
at `0xbcc8`, which is a coherent cartridge layout.

**Proof**: with the tables regenerated from `0x9ae4`, all four streams parse bit-exactly again —
**353/353 seek segments, 70,437 frames**. From `0xa000` the parser dies on the first frame of the
first stream in under half a second. A wrong codebook cannot produce a bit-exact parse of a whole
cartridge; a right one cannot fail instantly.

### Method note

The failure was caught in the first second by `vx_validate.py`, written minutes earlier as a permanent
tool precisely because the previous session's checks had lived in `/tmp` and been lost. The cost of
rebuilding the parser's inputs from a documented-but-superseded recipe was one command; the cost of
*not noticing* would have been a wrong table quietly feeding every later reconstruction result. Both
the disassembler and the validator now live in `tools/gba_video/`, and the extractor emits the tables
it needs, so the whole pipeline reproduces from the ROM alone.

## 16. Intra mode 3 is recursive midpoint subdivision, not plane prediction

The luma table's fourth entry (`0x03000ba4`) was assumed to be H.264's plane mode. It is not.

The entry seeds the block's **bottom-right corner** from the two far neighbours —
`(above_right + below_left + 1) >> 1`, taking `(w-1,-1)` and `(-1,h-1)` — and then dispatches through
an 11-entry table at `0x03000b78` indexed by `(w>>3) + 4*(h>>3)`. Indices 3 and 7 hold zero, being
exactly the combinations that cannot occur, and the table ends precisely where mode 3's own code
begins — an 11-word fit that confirms the indexing.

Each of the eleven targets is the same three writes, then a recursion into four quadrants. For a block
whose bottom-right corner is set, with `sw,sh = w/2,h/2`:

    bottom_mid(sw-1, h-1) = (left(-1, h-1)  + corner) >> 1
    right_mid (w-1, sh-1) = (top (w-1, -1)  + corner) >> 1
    centre    (sw-1, sh-1)                                     -- see axis rule

Note the bare `>>1`, with none of the `+1` rounding the corner seed uses.

### The centre's axis follows a parity rule

The centre can be reached two ways, and which one the code uses varies by block shape. Reading all
nine size variants:

| w×h  | 4×4 | 8×4 | 16×4 | 4×8 | 8×8 | 16×8 | 4×16 | 8×16 | 16×16 |
|------|-----|-----|------|-----|-----|------|------|------|-------|
| axis |  V  |  H  |  V   |  H  |  V  |  H   |  V   |  H   |   V   |

- **V** — `(bottom_mid + top(sw-1,-1)) >> 1`, down column `sw-1`
- **H** — `(right_mid + left(-1,sh-1)) >> 1`, along row `sh-1`

The rule is **vertical iff log2(w) + log2(h) is even**. "Square versus rectangular" fits six of the
nine and fails on 16×4 and 4×16, which are 4:1 yet vertical — worth stating because it was the first
hypothesis and it is wrong.

The parity is invariant under halving both sides, so a block keeps one axis for its entire recursion.
That is a real consistency check rather than a restatement: 16×16 recurses into 8×8 (V into V) and
16×8 into 8×4 (H into H), and every one of the eleven routines calls only same-parity children.

Ported as a single recursive `intra_midpoint` in `tools/gba_video/vx_reconstruct.py`, since all eleven
hardware routines are one function specialised by size.

## 17. The intra path is complete: chroma is interleaved, and 4×4 is H.264's

### Chroma is one interleaved plane, two bytes per sample

Mode 1 gives it away: it loads a neighbour with `ldrh` and replicates it with
`orr r9, r9, r9, lsl #16`. Cb and Cr sit interleaved in a single plane at the same `0x100` pitch as
luma, so a chroma block is `(w/2)×(h/2)` samples — `w` bytes wide. That answers §14's open question
about which of Cb/Cr the residual loop's plane index selects: neither, it is a byte offset of 0 or 1
into an interleaved pair.

Chroma predictors take a different base pair than luma — `[r0,#-4] + [r0,#4]` against luma's
`[r0,#-8] + [r0,#0]` — are handed the *luma* dimensions and halve them themselves, and number their
modes differently: **0 = DC, 1 = horizontal, 2 = vertical, 3 = midpoint**, where luma is
0 = vertical, 1 = horizontal.

Two details worth keeping:

- **Availability is read straight off the block's byte offset**, never tracked: the whole offset zero
  means no neighbours, `tst #0xff` zero means `mb_x == 0`, `tst #0xff00` zero means `mb_y == 0`. The
  same trick appears in the 4×4 DC mode.
- **Chroma DC averages the two edge DCs**, `(top + left + 1) >> 1`, rather than summing every
  neighbouring sample and dividing once as H.264 does. Each edge DC is `(sum + n/2) >> log2(n)` with
  the shift from a byte table at `0x03000c00` = `[0, 1, 2, 0, 3]` indexed by the luma dimension `>> 2`
  (entries 0 and 3 are the unreachable slots). With one edge available its DC is used unrounded; with
  neither the result is `0x80`.

Mode 3 reuses the §16 subdivision, running it twice over its own 11-entry table at `0x03000e18` —
once at byte `+0` for Cb and once at `+1` for Cr, with a 2-byte sample stride.

### All nine 4×4 modes are H.264's, in H.264's numbering

`0 V, 1 H, 2 DC, 3 diagonal down-left, 4 diagonal down-right, 5 vertical-right, 6 horizontal-down,
7 vertical-left, 8 horizontal-up`, with the standard `(a + 2b + c + 2) >> 2` and `(a + b + 1) >> 1`
taps. Modes 3 and 4 were read instruction by instruction; 5–8 were identified from the families of
positions that share a computed value — `2x−y` for mode 5 including its `−1` and `−2/−3` special
cases, `2y−x` for mode 6 — together with their neighbour sets (mode 7 reaches T6, mode 8 touches only
the left column).

**One real difference from H.264: there is no neighbour substitution.** The directional modes read
`T4..T7` unconditionally instead of replicating `T3` when the above-right block is unavailable, so
they filter whatever the frame buffer already holds there.

The 4×4 DC does follow H.264: `+2` per available edge and a shift of `1 + edges`, from the two little
sum helpers at `0x03000f38` (above) and `0x03000f64` (left).

### Mode coding is H.264's too

The loop at `0x030008fc` walks the 4×4 blocks in raster order: predicted mode is `min(above, left)`,
one bit accepts it, otherwise three bits carry a remainder with the predicted value skipped
(`rem >= pred → rem + 1`). Neighbour modes live in a byte array at `0x030008c2` with a **row stride of
5** — four block columns plus one border column preset to `9`, the "unavailable" marker that maps to
DC. After the last block the loop reads one `ue(v)` for the chroma mode and dispatches through the
chroma table, which is why the grammar shows a `ue` there.

## 18. Inter prediction: full-pel, three reference frames, and two odd modes

### There is no sub-pel interpolation

§14 listed "half-pel interpolation loops around `0x03001704`/`0x03002a8c`" as work to do. That was a
misreading. Motion vectors are **full-pel**, and the four-entry table at `0x03001568` — indexed by
`fp & 3`, where `fp` is the luma source *byte offset* — selects between four **`ldm` alignment
fixups**, not filter phases.

Variant 1 (`0x03001650`) makes it plain: it loads from one byte *below* the source, pulls 17 bytes,
and shifts the whole window right by 8 bits, reassembling exactly the bytes at the unaligned address.
Variants 2 and 3 do the same at halfword and 3-byte offsets. `0x03001704` is inside variant 2.

The variants also confirm each other. Chroma is misaligned by 2 exactly when `mv_x & 3 >= 2`, so
variants 0 and 1 can copy chroma with a plain `ldm` while 2 and 3 must fix it up — and variant 2's
chroma loop duly switches to `ldrh` with `lsl #16`/`lsr #16`. That was predicted before reading it.

Since all four produce identical pixels, a byte-wise copy reproduces them; the split is an ARM
addressing constraint with no bearing on the output.

The vector maps to the two planes as `luma = mv_x + mv_y*0x100` and
`chroma = (mv_x & ~1) + ((mv_y & ~1) >> 1)*0x100`. Chroma halves the vector, but since its samples are
two bytes the x term reappears as `mv_x` rounded down to even — `bic #1` on a two's-complement value.

### Three reference frames

The pointers modes 0/8/10 and 3/9/11 select between are not motion-vector candidates but **reference
frame bases**, in luma/chroma pairs: `[r0,#-0x20]/[r0,#-0x1c]` for reference 0, `-0x18`/`-0x14` for
reference 1, `-0x10`/`-0x0c` for reference 2. Modes 0/8/10 copy from reference 0/1/2 using the vector
the dispatcher predicted from neighbours; modes 3/9/11 add a two-`se(v)` delta to it.

### Mode 4 is not inter at all, and mode 5 corrects brightness

Two modes turn out to be neither plain copies nor plain intra, and the grammar's `se(v)` counts
confirm both readings exactly:

- **Mode 4 (3 `se(v)`)** — the §16 midpoint subdivision with a *coded* corner. Rather than deriving
  the bottom-right corner from neighbours alone it uses
  `((above_right + below_left + 1) >> 1) + 2*se(v)`, then runs the same fill (`0x03001a68`) mode 3
  uses. Chroma gets its own correction per component via `0x03001a3c`, which calls `0x030019e0` — and
  `0x030019e0` is exactly entry [10] of the chroma midpoint table at `0x03000e18`, an independent
  consistency check. One luma plus two chroma corrections is the 3 `se(v)`.

- **Mode 5 (5 `se(v)`)** — motion compensation with a **DC correction**. Two `se(v)` give an explicit
  vector, then three more carry one offset per component, applied as `clip(pixel + 2*delta)`
  (`adds sb, sb, r8, lsl #1`, `movlt #0`, `cmp #0xff`, `movgt #0xff`). It is a brightness/fade mode.

### The 16×16 mode space, complete

| mode | meaning | `se(v)` |
|------|---------|---------|
| 0, 8, 10 | copy from reference 0/1/2, predicted MV | 0 |
| 3, 9, 11 | same, plus an MV delta | 2 |
| 1 | split into two 16×8 (`0x03002184`) | 0 |
| 2 | split into two 8×16 (`0x030030f4`) | 0 |
| 4 | midpoint, corrected corners | 3 |
| 5 | motion compensation, DC corrected | 5 |
| 6 | intra 16×16 | 0 |
| 7 | intra 4×4 | 0 |
| 12–23 | modes 0–11 with a residual appended | as above |

Every count matches `vx_grammar.py`, which was derived independently from the bitstream. The
prediction path is now fully ported.

### Still open

The dispatcher's neighbour prediction of the motion vector before modes 0/3/8/9/10/11 consume it; how
the three reference frames are rotated as frames are decoded; the five unread reconstruct variants;
and the integration that walks `vx_sim`'s symbols into a frame buffer.

## 19. The residual path and the macroblock loop

### The "six reconstruct variants" are three planes times two paths

Not six transforms. The residual loop keeps a plane index in `r10` — 0 luma, 1 Cb, 2 Cr — and that
index selects into one of *two* three-entry tables:

- `0x0300577c` — full inverse transform, per plane: `0x03005a94`, `0x03005b2c`, `0x03005be4`
- `0x03005788` — DC-only fast path, per plane: `0x03005c9c`, `0x03005e30`, `0x03006044`

Variant 0 confirms §14's transform port instruction for instruction: `+0x20` on `d0` before the row
pass, the same butterfly, `asr #6`, then the clamp. Variant 1 differs only in writing bytes 0, 2, 4, 6
and reassembling 1, 3, 5, 7 untouched — the Cb half of an interleaved pair, which is the §17 layout
showing up again. Variant 3 has no butterfly at all: it adds one value, `(dc + 32) asr #6`, to every pixel,
which is what a block carrying nothing but a DC coefficient needs.

### The coded-block-pattern is six bits over an 8×8 quadrant

The loop at `0x03005650` walks a macroblock in 8×8 quadrants. Each quadrant reads **one `ue(v)`**,
maps it through the permutation table at `0x03005610`, and reads the result as six bits: bits 0–3 are
the quadrant's four luma 4×4 blocks, bit 4 is Cb, bit 5 is Cr. Four quadrants per macroblock gives the
16 luma and 8 chroma blocks §10 arrived at, and confirms §10's correction of §8 from the other
direction.

Block stepping inside a quadrant is `+4`, `+0x3fc`, `+4` — right, then down-and-back-left, then right.

### Motion vectors are predicted by the median of three

The macroblock loop (`0x03000520`, the vector code at `0x030005b0`) computes, per component,
`sum − min − max` of three neighbouring vectors — the cheap median, and H.264's predictor. It leaves
the result in `fp`/`ip` and calls the dispatcher, which is why modes 0/8/10 can use the vector without
reading anything and modes 3/9/11 need only a delta.

The predictor writes its final vector back with `strh` as `mv_x | (mv_y << 8)`, so **each component is
a signed byte** and vectors span roughly ±128 full pixels. The context is one halfword per macroblock
with a row stride of `0x24` — 18 entries where the 240-wide frame needs 15, the remainder being
border.

### Loop geometry

Per macroblock, luma and chroma each advance 16 bytes. Per macroblock row, luma advances `0x1000`
(16 lines) and chroma `0x800` (8 lines) — chroma being half height but the same width in bytes, since
its samples are two bytes. The frame width is read from `[r0,#0x18]`.

### Still open

Only two things. How the three reference frames are rotated as frames are decoded — that lives in the
frame-level setup near `0x03006e40`, not in the macroblock loop. And the driver that walks `vx_sim`'s
symbol stream into a frame buffer: every primitive it needs now exists, but nothing calls them in
sequence yet.

### The decoder context, pinned

The frame driver calls the macroblock loop with `r0 = ctx + 0xbc` (`0x03006dc4`), which turns every
`[r0,#N]` in the predictors into a concrete field:

| `[r0,#N]` | ctx | meaning |
|-----------|-----|---------|
| `-0x20` / `-0x1c` | `0x9c` / `0xa0` | reference 0, luma / chroma base |
| `-0x18` / `-0x14` | `0xa4` / `0xa8` | reference 1, luma / chroma base |
| `-0x10` / `-0x0c` | `0xac` / `0xb0` | reference 2, luma / chroma base |
| `-0x08` / `-0x04` | `0xb4` / `0xb8` | current frame, luma / chroma base |
| `0x00` / `0x04` | `0xbc` / `0xc0` | luma / chroma byte offset of this macroblock |
| `0x08` | `0xc4` | motion-vector context pointer |
| `0x18` | `0xd4` | frame width in pixels |

The offsets at `+0x00`/`+0x04` are both what every predictor adds to its base *and* what the intra
modes test for edge availability — a macroblock's position and its neighbour availability are
literally the same number, which is why §17's availability trick works.

Four frame buffers are allocated (the setup loop at `0x03006e40` runs `r7 = 0..3`), matching three
references plus the frame being decoded. How the four are rotated between frames is the one piece of
the architecture still unread.

## 20. First reconstructed frames

`tools/gba_video/vx_decode.py` now walks `vx_sim`'s symbols into a frame buffer and writes PGMs. The
output is **recognisably the DreamWorks logo** — clouds and the crescent moon — so prediction,
residual and geometry are broadly right.

### Making the parser emit symbols

`decode_unit` gained an optional `sink` and an `off`. The symbols were always being read; the sink
only stops them being discarded, so bit positions are unchanged — and the four streams still validate
**353/353 segments exact** after the change.

Block position comes from the split tree. Mode 1 halves the height and mode 2 the width, which pins
the size of the seven dispatchers that have no whole-block intra mode to check against. The sixteen
are exactly the 4×4 grid of {2,4,8,16} widths and heights, and the seven without an intra mode are
exactly those with a side of 2 — below what the intra predictors can address. Each of those seven is
reached two independent ways and both agree.

### Three bugs the pictures found

- **The quantiser was being ignored.** The driver passed `qp=0` while the segment's opening `ue(v)`
  gives 34. The container's `quantizer` field is 0, so the whole quantiser comes from the segment.
- **`strb` truncates, it does not clamp.** Mode 4's corrected corner, `((TR+BL+1)>>1) + 2*se`, has to
  wrap at 8 bits.
- **Luma and chroma live in separate planes.** `inter_copy` had been written to do both against one
  buffer pair, so its chroma pass was overwriting luma. Split into `inter_copy` and
  `inter_copy_chroma`.

A fourth fix is right but changed nothing visible yet: the midpoint fill takes its corner **in a
register**, not by reading the byte back, which matters only for mode 4 where the stored byte is
truncated and the register value is not.

### Open: mode 4 blocks stripe

Frames are correct except for scattered blocks of hard horizontal or vertical banding. Replacing every
mode-4 block with flat grey removes the striping completely; doing the same for modes 6 or 7 does not.
So the fault is in the corrected-corner midpoint mode.

What has been checked and is *not* the cause: the zig-zag and dequant (the column transform's load
offsets pick scan indices 0, 2, 3, 9 for column 0, which map to raster 0, 4, 8, 12 under the ported
`ZIGZAG`, and its multiplier pattern matches H.264's 10/13/16 assignment); the `se(v)` ordering (luma,
Cb, Cr, as the handler reads them); and the per-size handlers (`0x03003298` for 8×8 is structurally
identical to `0x03001ac0` for 16×16, so one generic implementation is right).

Note also that the frame at 240×112 uses only intra modes in its early frames — the mode histogram for
frame 9 shows no inter modes at all — so this is not an inter-prediction or reference-frame problem.

### Still open

The striping above; how the three reference frames rotate between frames; and chroma output (the
driver writes luma-only PGMs, though chroma is reconstructed).

## 21. Motion vector prediction and the reference ring

The first decoded video was blocky, and reference-frame rotation was only part of it.

### The predicted vector was never being applied

Modes 0/8/10 carry no `se(v)` because they use the vector *predicted from neighbours*, and 3/9/11
carry a delta to that prediction. The first driver did neither: it read `se` as an absolute vector,
so those modes copied with (0,0) and the delta modes used the delta alone.

The three neighbours are **left (`-2`), above (`-0x24`) and above-right (`-0x22`)** — H.264's A, B, C
over the `0x24` row stride — and the median is the usual `sum - min - max`.

### The context entry is an arithmetic sum, not a bitfield

`add fp, fp, ip, lsl #8` packs the vector as `mv_x + (mv_y << 8)`, so a **negative `mv_x` borrows from
the high byte**. The decoder undoes this when reading a neighbour: it takes the high byte with `asr #8`,
sign-extends the low byte, and then adds one back to y when x is negative (`addlt r5, r5, #1`). Packing
with an OR instead of a sum is wrong for every negative horizontal vector.

Mode 5 is the exception: its two `se(v)` are an absolute vector, not a delta, and it never writes the
context back.

### The reference frames are a four-buffer ring

At `0x03006d30` the frame driver computes `n & 3`, `(n-1) & 3` and `(n-2) & 3` from a counter it bumps
once per frame, and indexes a four-entry table of buffers:

    reference 0 = buffer[n & 3]        the previous frame
    reference 1 = buffer[(n-1) & 3]    two frames back
    reference 2 = buffer[(n-2) & 3]    three frames back

So the three references are simply the last three decoded frames, and modes 0/3, 8/9 and 10/11 select
between them.

### Correction to §19: the context base is `ctx + 0xec`

The frame driver passes `ctx + 0xbc` to the macroblock loop, but `FUN_03000520` re-bases it by
`4 + 0x2c` before the predictors see it, so `r0 = ctx + 0xec`. The reference bases are therefore at
`ctx+0xcc`..`ctx+0xe0`, not `ctx+0x9c`..`ctx+0xb0`.

The check that settles it: the driver stores zero to `ctx+0xec` and `ctx+0xf0` immediately before the
loop, and those are the macroblock's luma and chroma offsets, which must start at zero. Only with a
base of `0xec` do all twelve fields line up with stores the driver actually makes — with `0xbc` the
reference bases land on addresses nothing ever writes.

## 22. Residual quadrants follow the coded leaf's aspect

The first reconstruction driver placed residual groups with a hard-coded two-quadrant row:
`qx=(q%2)*8, qy=(q//2)*8`. That is right for 16×8 and 16×16 leaves, but wrong for 8×16: its two
groups are stacked vertically, not side by side. The error affected both luma and chroma and is one
source of the block damage in busy frames.

The loop at `0x03005650` makes the geometry unambiguous. Its inner loop advances both plane pointers
by `+8` and counts down the coded width by 8. The outer loop advances luma by `+0x800`, chroma by
`+0x400`, and counts down the height by 8. Therefore groups are raster ordered with
`groups_per_row = leaf_width / 8`; the chroma vertical step is half luma's because of 4:2:0.

`vx_decode.py` now derives the group position from `unit["size"]`. Focused tests cover the distinct
16×8, 8×16 and 16×16 layouts, and the parser remains exact over all four streams: **353/353** seek
segments.

## 23. Hardware-exact reconstruction through busy frames and seek boundaries

Comparing packed NV12 planes directly against mGBA frame-buffer dumps exposed four independent driver
errors. None required changing the bitstream grammar or inverse transform:

- Mode 5 read five `se(v)` values but applied only the luma correction. `0x03001854` and
  `0x0300192c` apply the fourth and fifth values independently to Cb and Cr as
  `clip(sample + 2*delta)`.
- Coded split modes 13 and 14 returned after their children, skipping the residual attached to the
  parent node. Plain splits still stop after prediction; coded splits now run the common residual path.
- Whole-block luma DC at `0x03000a54` does not take one weighted mean of all available edge pixels.
  It rounds the top mean and left mean separately, then computes `(top + left + 1) >> 1`. The
  distinction matters for rectangular blocks; the first observable error was a 16×8 block in frame 8.
- Intra-4×4 DC tests edge availability using the plane-relative frame offset. The Python safety margin
  had been included in that test, falsely making the top edge available. The first affected block was
  at `(8,0)` in frame 9.

The reconstruction driver also consumes extracted header metadata. It resets to every seek entry's
exact video bit and reads that segment's opening quantiser, and it rotates four contiguous physical
frame slots with the hardware's `0xa000`-byte luma and `0x5000`-byte chroma allocations.

Fresh mGBA captures at frames 0, 1, 5, 7, 8, 9, 10, 100 and 325 of stream 0 now compare
**byte-for-byte exact in both luma and chroma**. Frame 100 exercises the formerly corrupted busy
material; frame 325 is the first frame at a seek/QP boundary. Focused unit tests retain each corrected
geometry or rounding case, while `vx_validate.py` remains the all-stream grammar gate.

## 24. Audio is the existing VX LPC codec in a raw AFrame stream

The `.audio` region needs no new codec reverse engineering. Its first 3124 bytes have exactly the
`libavcodec/vx_audio.c` layout: three 64×8 signed LPC codebooks, eight scale modifiers, eight LPC base
coefficients and an initial scale. After that block is a headerless sequence of little-endian AFrames.
Each AFrame starts with two 16-bit words; bits 13–12 of word 2 select 8, 5, 4 or 3 following pulse
words, exactly as the DS-era decoder does. The first word at stream 0 byte 3124 is `0xfe47`, whose
`prev_frame_offset` is `0x7f` (intra) and whose remaining fields are all valid under that grammar.

Every nonterminal audio seek offset starts with one and only one intra AFrame. Walking the variable
sizes from each offset to the next lands exactly on all **353/353** seek boundaries:

```
stream0: 637707 AFrames -> 180/180 seek segments exact
stream1: 626494 AFrames -> 171/171 seek segments exact
stream2:  10905 AFrames ->   1/1 seek segments exact
stream3:  12943 AFrames ->   1/1 seek segments exact
```

This also corrects the frame-rate interpretation in §12. The header's `0x30c3` timing field is **not**
Q10 frames/second. There are almost exactly `128/7` AFrames per video frame. At seven video frames per
second that is 128 AFrames/second; each AFrame reconstructs 128 mono samples, giving the declared
16384 Hz exactly. The two large streams are separate full movies of about 83 and 82 minutes, not
47-minute halves. The GBA setup at `0x0300724c` routes Direct Sound FIFO A to both speakers, confirming
mono output.

`tools/gba_video/vx_audio_decode.py` ports the already-established fixed-point decoder math and writes
mono S16 WAV. `vx_audio_validate.py` is the framing gate. As a complete smoke test, stream 2 decodes to
1,395,840 samples (85.195 seconds), matching 595 video frames at 7 fps plus normal codec padding.

The Python port explicitly wraps the fixed-point intermediates to signed 32-bit values, matching the
C/ARM arithmetic rather than Python's unbounded integers. Its complete stream-2 PCM is byte-identical
to `libavcodec/vx_audio.c`: both produce 2,791,680 bytes with SHA-256
`b4c973965726440e038274804924d068e12028d7a5da29d6d8961c1e0f64edf6`.

Native demux and audio are integrated. `libavformat/gbavx.c` scans the ROM, selects a movie with
`-resource`, passes the 3124-byte codebook to the existing `vx_audio` decoder and packets whole raw
AFrames. It builds an exact sample index from the cartridge seek table by counting AFrames between
the recorded byte offsets; every indexed position is also checked for an intra AFrame. Decoder flush
clears all history, so a seek starts in the same state as a fresh decoder. Indexed `-ss` output was
compared byte-for-byte with continuous decoding at stream 0 seconds 300, 1200 and 4000 and stream 1
second 1000; every one-second window matched exactly.

The demuxer also exposes the video as `gba_vx`, one packet per seek-table interval. These are the only
honest packet boundaries: each packet may contain hundreds of frames, since individual frame sizes are
not recorded. `libavcodec/gba_vx.h` defines a 16-byte prefix carrying the leading bit skip, valid bit
count and frame count. The payload is byte-swapped per little-endian halfword into ordinary MSB-first
bit order, so the decoder can use FFmpeg's normal bit reader after the recorded skip. Starting
the Python decoder with fresh state at seek frames 325 and 612 reproduced the continuous decode
byte-for-byte for the first five frames at each boundary, confirming that these packets are genuine
independently decodable key segments. Dumping and independently parsing the native packets confirms
their prefix fields and byte extents against all **353/353** container intervals.
`tools/gba_video/vx_native_packet_validate.py` is the repeatable native packet gate.

```
./ffmpeg -resource 2 -i "$ROM" -map 0:a:0 -c:a pcm_s16le stream02-native.wav
./ffmpeg -ss 1200 -resource 0 -i "$ROM" -map 0:a:0 -t 1 seek.wav
```

## 25. Native VX++ video decoder

`libavcodec/gbavxdec.c` now decodes `VX++` as well as `VXGB`. For VX++, the ROM demuxer copies the
global coefficient data at ROM `0x9ae4` into video extradata: 4096 little-endian 16-bit VLC cells,
then the 128-byte value-offset and 128-byte run-offset escape tables. Each `GVX1` packet supplies one
independent seek segment. The receive-frame decoder skips the packet's leading bits, reads its
`ue(v)` quantizer, emits the declared number of frames, consumes one marker between consecutive
frames and verifies that the final bit position equals the packet prefix.

The last video region is the sole exception: it has no following seek entry and
is rounded up to the cartridge's `0x200`-byte alignment. The decoder accepts at
most 4095 trailing bits there, and only when every one is zero.

Prediction uses the hardware's contiguous four-slot arena: each slot is `0xa000` bytes of luma plus
`0x5000` bytes of interleaved chroma at pitch `0x100`. This is observable behavior, not just an
allocation detail, because legal full-pel motion vectors can read outside the visible image and
across plane or slot boundaries. Motion-vector predictor entries are stored as the cartridge's
signed-byte pair packed into one halfword.

The proprietary coefficient decoder implements the direct lookup and all three escape forms. VX++
also requires a full-range unsigned Exp-Golomb read for its 64-entry coded-block permutation;
FFmpeg's 0..31 helper truncates real index 58 to 32 and desynchronizes the next macroblock.

On the 240x160 Rev 6 Shrek stream, native RGB output matches the Python hardware model byte-for-byte
for 330 consecutive frames, including independent segment starts at frames 155 and 318. The same
arena changes retain a byte-exact 160-frame VXGB regression across its first seek boundary.
