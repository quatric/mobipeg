#!/usr/bin/env python3
"""Caimans 2.2 (Bad Boys 2 trailer) video: a first working macroblock decoder.

Reverse engineered this session from `caimans_badboys2.gba`. This is a
different codec generation from Pro -- see doc/caimans_handoff.md for the
full derivation. Summary of what's implemented and how it was checked:

**Container.** A 7-word cursor struct at IWRAM 0x0300502c (crt0-copied from
ROM 0x0800213c) holds the audio cursor (word 0, confirmed against a live
audio capture) and the video cursor (word 4, ROM 0x08002710). Records are
read sequentially: a 24-bit length at the record's own +1..+3, a type/flag
byte at +11 selecting one of two top-level dispatch paths, both eventually
reaching `FUN_08000418` (ROM 0x08000418), which is a tagged sub-chunk parser,
not a single per-frame routine.

**Tag 0x3000: the raster/macroblock pass.** After a per-record header (which
carries width implicitly via the 480-byte stride literal, and a per-record
height in the bitstream itself), it walks the frame in 4x4-pixel cells,
row-major, reading one bit per cell from a 32-bit little-endian bitmask
(MSB first, tested via a sign check then left-shifted) to choose between two
cell encodings:

  - bit clear: **one index byte**, selecting a "codebook" cache entry that
    gets replicated into the 4x4 destination cell (see `blit_simple`).
  - bit set: **four index bytes** (two pairs), blending two cache entries
    each into a 4x4 cell with a specific corner-tap pattern (see
    `blit_complex`). Only worked out and implemented for this session; less
    thoroughly checked than the simple path (see below).

**The codebook cache** is a separate array of 0x24 = 36-byte cells (16 bytes
of pre-built pixel data + 6 bytes of the original source record + padding),
built by tag `0x2000`..`0x2300` sub-chunks from *another* stream of 6-byte
records (4 index bytes + 2 signed chroma deltas), via `decode_pair` --
this is the actual color/texture reconstruction, table-driven and
confirmed YCoCg-R-shaped.

**Validated against real hardware, live, this session:**

- `decode_pair`: captured a real cell's source bytes, its two output pixels,
  and the two lookup tables in one mGBA breakpoint, and reproduced both
  output pixels exactly in Python from those same bytes (see the handoff
  doc).
- `blit_simple`: captured a real call's `(dest, cache_ptr)`, then read back
  the resulting VRAM and the source cache cell -- the row0/row1 (and by
  symmetry row2/row3) replication-with-swap pattern reproduces the captured
  VRAM bytes exactly.

`blit_complex` was subsequently validated on a discriminating live case
(different, non-uniform A/B cells) for both halves of the 4x4 output. Both
halves call the same IWRAM routine at 0x0300015c.

This module implements the fixed colour tables, codebook-building pass, and
all raster variants observed in the sample. ``caimans22_decode.py`` supplies
the record-level cold-ROM driver.
"""
import struct

ROM_PATH = "/Volumes/SSD/dlz/Folders/mobipeg/build_caimans/roms/caimans_badboys2.gba"
ROM_BASE = 0x08000000

CELL_SIZE = 0x24          # bytes per codebook cache entry
CELL_PIXELS_OFFSET = 0    # first 16 bytes of a cell are 8 pixels (4 pairs)


def build_color_tables():
    """Reproduce ``FUN_08000324``'s two fixed signed-index colour tables.

    Each nominal table pointer has 128 valid bytes before it and 384 from it
    onward, covering indices -128..383. The tables differ only in their
    rounding threshold within each eight-value quantisation interval.
    Returns ``(table_a, table_b, table_base)``.
    """
    tables = []
    for threshold in (2, 5):
        table = bytearray(512)
        for value in range(-128, 384):
            if value <= 0:
                quantised = 0
            elif value > 247:
                quantised = 31
            else:
                base = value & ~7
                quantised = base >> 3 if value - base <= threshold else (base + 8) >> 3
            table[value + 128] = quantised
        tables.append(table)
    return tables[0], tables[1], 128


def decode_pair(idx, d1, d2, table_a, table_b, table_base=256):
    """One 6-byte source record's single index -> (pixel_from_A, pixel_from_B).

    `table_a`/`table_b` must be byte buffers captured with `table_base` bytes
    of padding before the nominal table start, since deltas can be negative
    (this was true of the real capture that validated this function).
    `idx`, `d1`, `d2` are the raw bytes as read from the stream; `d1`/`d2`
    are interpreted signed.
    """
    d1 = d1 - 256 if d1 > 127 else d1
    d2 = d2 - 256 if d2 > 127 else d2
    dr = 2 * d2
    dg = -d2 - ((d1 + 1) >> 1)
    db = 2 * d1

    def px(table):
        r = table[table_base + idx + dr]
        g = table[table_base + idx + dg]
        b = table[table_base + idx + db]
        return (r & 0x1F) | ((g & 0x1F) << 5) | ((b & 0x1F) << 10)

    return px(table_a), px(table_b)


def build_cell(record, table_a, table_b, table_base=256, complex_cell=False):
    """Build one 0x24-byte cache cell from its six-byte source record.

    The dense 0x2200 and sparse 0x2300 chunks build simple cells, for which
    all four indices become adjacent A/B pixel pairs.  The 0x2000/0x2100
    chunks build complex-blit cells; hardware only materialises the four
    taps consumed by ``blit_complex`` (pixel slots 0, 3, 5 and 6).
    """
    if len(record) != 6:
        raise ValueError("a Caimans 2.2 cell record is exactly six bytes")
    indices = record[:4]
    d1, d2 = record[4:]
    cell = bytearray(CELL_SIZE)
    cell[16:22] = record
    pairs = [decode_pair(i, d1, d2, table_a, table_b, table_base)
             for i in indices]
    if complex_cell:
        pixels = [0] * 8
        pixels[0] = pairs[0][0]
        pixels[3] = pairs[1][1]
        pixels[5] = pairs[2][1]
        pixels[6] = pairs[3][0]
    else:
        pixels = [pixel for pair in pairs for pixel in pair]
    struct.pack_into("<8H", cell, 0, *pixels)
    return cell


def decode_codebook_chunk(tag, data, simple_cache, complex_cache,
                          table_a, table_b, table_base=256):
    """Apply one Caimans 2.2 codebook sub-chunk to the persistent caches.

    ``0x2000``/``0x2200`` are dense streams of six-byte records starting at
    slot zero. ``0x2100``/``0x2300`` prepend 32-bit little-endian, MSB-first
    update masks; each set bit consumes one six-byte record and updates the
    corresponding cache slot. The 0x20xx family targets the complex cache
    and the 0x22xx family targets the simple cache.
    """
    if tag not in (0x2000, 0x2100, 0x2200, 0x2300):
        raise ValueError("not a codebook chunk: 0x%04x" % tag)
    cache = complex_cache if tag in (0x2000, 0x2100) else simple_cache
    complex_cell = tag in (0x2000, 0x2100)
    pos = 0
    if tag in (0x2000, 0x2200):
        if len(data) % 6:
            raise ValueError("dense codebook payload is not a multiple of six")
        slots = range(len(data) // 6)
        for slot in slots:
            record = data[pos:pos + 6]
            pos += 6
            off = slot * CELL_SIZE
            cache[off:off + CELL_SIZE] = build_cell(
                record, table_a, table_b, table_base, complex_cell)
        return pos

    slot = 0
    while pos < len(data):
        if pos + 4 > len(data):
            raise ValueError("truncated sparse codebook mask")
        mask = struct.unpack_from("<I", data, pos)[0]
        pos += 4
        for bit in range(32):
            if mask & (0x80000000 >> bit):
                if pos + 6 > len(data):
                    raise ValueError("truncated sparse codebook record")
                record = data[pos:pos + 6]
                pos += 6
                off = slot * CELL_SIZE
                cache[off:off + CELL_SIZE] = build_cell(
                    record, table_a, table_b, table_base, complex_cell)
            slot += 1
    return pos


def blit_simple(buf, dest_off, cache, cache_off, stride):
    """The single-index 4x4 replication blit (IWRAM 0x03000114).

    Row0 = cache pixels[0:4] verbatim. Row1 = the same 4 pixels with each
    adjacent pair swapped (a 32-bit rotate-by-16 in the original). Row2/3
    repeat with cache pixels[4:8]. Confirmed against a live VRAM capture.
    """
    px = list(struct.unpack_from("<4H", cache, cache_off))
    row0 = px
    row1 = [px[1], px[0], px[3], px[2]]
    px2 = list(struct.unpack_from("<4H", cache, cache_off + 8))
    row2 = px2
    row3 = [px2[1], px2[0], px2[3], px2[2]]
    for r, row in enumerate((row0, row1, row2, row3)):
        o = dest_off + r * stride
        struct.pack_into("<4H", buf, o, *row)


def blit_complex(buf, dest_off, cache_a, off_a, cache_b, off_b, stride):
    """The two-cache blended 4x2 blit (IWRAM 0x0300015c), called twice by the
    caller (rows 0-1 from one cache pair, rows 2-3 from another) to fill a
    full 4x4 cell. Tap pattern read directly off the disassembly -- see the
    module docstring for the confidence caveat.
    """
    a = struct.unpack_from("<8H", cache_a, off_a)
    b = struct.unpack_from("<8H", cache_b, off_b)
    row0 = [a[0], a[3], b[0], b[3]]
    row1 = [a[5], a[6], b[5], b[6]]
    struct.pack_into("<4H", buf, dest_off, *row0)
    struct.pack_into("<4H", buf, dest_off + stride, *row1)


def decode_raster_3100(bitstream, pos, remaining, simple_cache, simple_base,
                       complex_cache, complex_base, width, height, stride,
                       dest_buf, dest_off):
    """Decode a tag-**0x3100** (or 0x8100) raster pass -- the *skip-capable*
    variant, which is what real delta frames actually use.

    This is a different loop from tag 0x3000/0x8000. The mask is consumed
    **two bits per coded cell, one bit per skipped cell**:

      - bit clear            -> cell is SKIPPED: no bytes consumed, no blit,
                                the destination keeps its previous contents.
      - bit set, next clear  -> SIMPLE cell: one index byte, `blit_simple`.
      - bit set, next set    -> COMPLEX cell: four index bytes, two
                                `blit_complex` calls (rows 0-1 and rows 2-3).

    When the selector reaches the last bit of the current 32-bit mask word
    *during* a coded cell, a fresh mask word is loaded mid-cell -- that
    reload is what makes this loop impossible to model as a fixed-width
    bit reader.

    Note there are **two separate cell arrays**: simple cells come from
    `param_1[1]` and complex cells from `param_1[0]` (they are different
    base addresses in the real player -- getting these backwards silently
    produces plausible-looking garbage).
    """
    row = 0
    col = 0
    while row < height:
        if pos + 4 > len(bitstream):
            break
        mask = struct.unpack_from("<I", bitstream, pos)[0]
        pos += 4
        remaining -= 4
        sel = 0x80000000
        while True:
            if row >= height:
                break
            if mask & sel:
                if sel == 1:
                    if remaining < 0:
                        return pos
                    mask = struct.unpack_from("<I", bitstream, pos)[0]
                    pos += 4
                    remaining -= 4
                    sel = 0x80000000
                else:
                    sel >>= 1
                d_top = dest_off + row * stride + col * 2
                if (mask & sel) == 0:
                    idx = bitstream[pos]
                    pos += 1
                    remaining -= 1
                    blit_simple(dest_buf, d_top, simple_cache,
                                simple_base + idx * CELL_SIZE, stride)
                else:
                    b0, b1, b2, b3 = bitstream[pos:pos + 4]
                    pos += 4
                    remaining -= 4
                    d_bot = dest_off + (row + 2) * stride + col * 2
                    blit_complex(dest_buf, d_top,
                                 complex_cache, complex_base + b0 * CELL_SIZE,
                                 complex_cache, complex_base + b1 * CELL_SIZE, stride)
                    blit_complex(dest_buf, d_bot,
                                 complex_cache, complex_base + b2 * CELL_SIZE,
                                 complex_cache, complex_base + b3 * CELL_SIZE, stride)
            sel >>= 1
            col += 4
            if col >= width:
                col = 0
                row += 4
            if sel == 0 or row >= height:
                break
        if remaining < 1:
            break
    return pos


def decode_raster_3000(bitstream, pos, remaining, simple_cache, simple_base,
                       complex_cache, complex_base, width, height, stride,
                       dest_buf, dest_off):
    """Decode 0x3000/0x8000: one selector bit and no skips per 4x4 cell."""
    row = col = 0
    while remaining > 0 and row < height:
        if pos + 4 > len(bitstream):
            break
        mask = struct.unpack_from("<I", bitstream, pos)[0]
        pos += 4
        remaining -= 4
        for bit in range(32):
            if row >= height:
                break
            dest = dest_off + row * stride + col * 2
            if mask & (0x80000000 >> bit):
                if pos + 4 > len(bitstream):
                    return pos
                b0, b1, b2, b3 = bitstream[pos:pos + 4]
                pos += 4
                remaining -= 4
                blit_complex(dest_buf, dest,
                             complex_cache, complex_base + b0 * CELL_SIZE,
                             complex_cache, complex_base + b1 * CELL_SIZE, stride)
                blit_complex(dest_buf, dest + 2 * stride,
                             complex_cache, complex_base + b2 * CELL_SIZE,
                             complex_cache, complex_base + b3 * CELL_SIZE, stride)
            else:
                if pos >= len(bitstream):
                    return pos
                idx = bitstream[pos]
                pos += 1
                remaining -= 1
                blit_simple(dest_buf, dest, simple_cache,
                            simple_base + idx * CELL_SIZE, stride)
            col += 4
            if col >= width:
                col = 0
                row += 4
    return pos


def decode_raster_3200(bitstream, pos, remaining, simple_cache, simple_base,
                       width, height, stride, dest_buf, dest_off):
    """Decode 0x3200/0x8200: an unmasked stream of simple-cell indices."""
    row = col = 0
    while remaining > 0 and pos < len(bitstream) and row < height:
        idx = bitstream[pos]
        pos += 1
        remaining -= 1
        dest = dest_off + row * stride + col * 2
        blit_simple(dest_buf, dest, simple_cache,
                    simple_base + idx * CELL_SIZE, stride)
        col += 4
        if col >= width:
            col = 0
            row += 4
    return pos


def decode_raster_chunk(tag, data, simple_cache, complex_cache,
                        width, height, stride, dest_buf, dest_off=0):
    """Dispatch any currently known Caimans 2.2 raster sub-chunk."""
    if tag in (0x3000, 0x8000):
        return decode_raster_3000(data, 0, len(data), simple_cache, 0,
                                  complex_cache, 0, width, height, stride,
                                  dest_buf, dest_off)
    if tag in (0x3100, 0x8100):
        return decode_raster_3100(data, 0, len(data), simple_cache, 0,
                                  complex_cache, 0, width, height, stride,
                                  dest_buf, dest_off)
    if tag in (0x3200, 0x8200):
        return decode_raster_3200(data, 0, len(data), simple_cache, 0,
                                  width, height, stride, dest_buf, dest_off)
    raise ValueError("not a raster chunk: 0x%04x" % tag)


def load_tables_and_cache_live(port=2345):
    """Pull the already-built codebook tables and cache out of a running
    mGBA session (attach to one already stopped inside FUN_08000418's body,
    e.g. via a breakpoint script -- this module doesn't launch mGBA itself).
    Returns (table_a, table_b, cache, table_base_padding).
    """
    raise NotImplementedError(
        "call this from a script that already holds an RSP connection; "
        "see tools/gba_video/caimans_trace_video22.py for the pattern")
