#!/usr/bin/env python3
"""Extract the Caimans Pro leaf codebooks from the IWRAM dump and the ROM.

See ``doc/caimans_handoff.md``, section "Leaf modes 1-6". A leaf whose mode
VLC decodes to N reconstructs as

    out = clamp(pred + value + sum of N codebook patterns)

where each of the N layers spends 4 bits selecting one of 16 patterns, and
layer i draws from pattern slots 16*i .. 16*i+15 -- so a level's codebook is
96 patterns, each one full leaf's worth of signed 8-bit samples.

The codebook base pointers are two four-word tables in IWRAM (levels 0..3
only; the 16-wide leaves at levels 4 and 5 never take this path). Some of the
entries point into IWRAM and some into cartridge ROM at 0x08xxxxxx, so both
images are needed to recover the full set.

Run directly to print a summary and validate the layout:

    python3 tools/gba_video/caimans_codebooks.py
"""
import os
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
IWRAM_PATH = os.path.join(ROOT, "build_caimans", "caimans_iwram2.bin")
ROM_PATH = os.path.join(ROOT, "build_caimans", "roms", "caimans_pro_pooh_hq.gba")

IWRAM_BASE = 0x03000000
ROM_BASE = 0x08000000

# DAT_030057f8 / DAT_030057fc: base = table[level], indexed by the leaf level.
TABLE_INTRA = 0x03001D20  # param_3 != 0, absolute pixels
TABLE_INTER = 0x03002640  # param_3 == 0, residual on top of the prediction

LEVELS = (0, 1, 2, 3)
PATTERNS_PER_LEVEL = 96  # 6 layers x 16 patterns
LAYERS = 6
PATTERNS_PER_LAYER = 16

# Leaf geometry per level, from the subdivision ladder. The pattern stride the
# decoder computes is (nib + 16*i) << (level + 1) words, i.e. exactly one leaf.
LEAF_SIZE = {0: (4, 2), 1: (4, 4), 2: (8, 4), 3: (8, 8)}


def leaf_bytes(level):
    w, h = LEAF_SIZE[level]
    return w * h


class Images:
    """The two memory images, addressed by GBA address rather than offset."""

    def __init__(self, iwram_path=IWRAM_PATH, rom_path=ROM_PATH):
        with open(iwram_path, "rb") as f:
            self.iwram = f.read()
        with open(rom_path, "rb") as f:
            self.rom = f.read()

    def region(self, addr):
        if IWRAM_BASE <= addr < IWRAM_BASE + len(self.iwram):
            return "iwram", self.iwram, addr - IWRAM_BASE
        if ROM_BASE <= addr < ROM_BASE + len(self.rom):
            return "rom", self.rom, addr - ROM_BASE
        raise ValueError("address 0x%08x is in neither image" % addr)

    def read(self, addr, length):
        _, buf, off = self.region(addr)
        if off + length > len(buf):
            raise ValueError("read of 0x%x at 0x%08x runs past the image" % (length, addr))
        return buf[off:off + length]

    def word(self, addr):
        return struct.unpack("<I", self.read(addr, 4))[0]


def base_pointers(img, table_addr):
    """The four codebook base addresses, one per leaf level 0..3."""
    return [img.word(table_addr + 4 * lv) for lv in LEVELS]


def extract_level(img, base, level):
    """Return the level's 96 patterns as lists of signed samples, row-major."""
    size = leaf_bytes(level)
    raw = img.read(base, PATTERNS_PER_LEVEL * size)
    out = []
    for p in range(PATTERNS_PER_LEVEL):
        chunk = raw[p * size:(p + 1) * size]
        out.append([b - 256 if b > 127 else b for b in chunk])
    return out


def load(img=None):
    """Extract every codebook. Returns {"intra"|"inter": {level: [patterns]}}."""
    img = img or Images()
    books = {}
    for path, table in (("intra", TABLE_INTRA), ("inter", TABLE_INTER)):
        bases = base_pointers(img, table)
        books[path] = {lv: extract_level(img, bases[lv], lv) for lv in LEVELS}
    return books


def pattern_for(books, path, level, layer, nibble):
    """The pattern a leaf selects: layer i draws from slots 16*i .. 16*i+15."""
    if not 0 <= layer < LAYERS:
        raise ValueError("layer %d outside 0..%d" % (layer, LAYERS - 1))
    if not 0 <= nibble < PATTERNS_PER_LAYER:
        raise ValueError("nibble %d outside 0..15" % nibble)
    return books[path][level][layer * PATTERNS_PER_LAYER + nibble]


def _extents(img):
    """(path, level, base, size, region) for all eight codebooks."""
    rows = []
    for path, table in (("intra", TABLE_INTRA), ("inter", TABLE_INTER)):
        for lv, base in zip(LEVELS, base_pointers(img, table)):
            size = PATTERNS_PER_LEVEL * leaf_bytes(lv)
            rows.append((path, lv, base, size, img.region(base)[0]))
    return rows


def validate(img=None):
    """Check the layout claims in the handoff doc. Returns a list of findings."""
    img = img or Images()
    notes = []
    rows = _extents(img)

    # Every base must land in one of the two images, and the whole codebook
    # must fit -- extract_level raises if it does not.
    for path, lv, base, size, region in rows:
        extract_level(img, base, lv)
        notes.append("%-5s level %d  0x%08x + 0x%04x  (%s)" % (path, lv, base, size, region))

    # Each pointer table is exactly four words: the data of whichever level
    # sits first in memory begins immediately after the table.
    for path, table in (("intra", TABLE_INTRA), ("inter", TABLE_INTER)):
        bases = base_pointers(img, table)
        first_after = table + 4 * len(LEVELS)
        if first_after not in bases:
            notes.append("WARNING: %s table at 0x%08x is not followed by codebook data"
                         % (path, table))
        else:
            notes.append("%-5s table 0x%08x is 4 words; data starts at 0x%08x (level %d)"
                         % (path, table, first_after, bases.index(first_after)))

    # The three ROM codebooks should tile one contiguous run with no overlap.
    rom = sorted(((base, size, path, lv) for path, lv, base, size, region in rows
                  if region == "rom"))
    for (b0, s0, p0, l0), (b1, _, p1, l1) in zip(rom, rom[1:]):
        if b0 + s0 == b1:
            notes.append("ROM   %s level %d ends exactly where %s level %d begins (0x%08x)"
                         % (p0, l0, p1, l1, b1))
        else:
            notes.append("WARNING: ROM gap/overlap between %s level %d and %s level %d "
                         "(0x%08x + 0x%x != 0x%08x)" % (p0, l0, p1, l1, b0, s0, b1))

    # The level-0 intra codebook is the one whose end is visible in the IWRAM
    # dump; zero padding should follow it.
    base = base_pointers(img, TABLE_INTRA)[0]
    end = base + PATTERNS_PER_LEVEL * leaf_bytes(0)
    tail = img.read(end, 16)
    notes.append("intra level 0 ends at 0x%08x, followed by %s"
                 % (end, "zero padding" if not any(tail) else "non-zero data"))
    return notes


def main():
    img = Images()
    for note in validate(img):
        print(note)

    books = load(img)
    print()
    for path in ("intra", "inter"):
        for lv in LEVELS:
            pats = books[path][lv]
            flat = [s for p in pats for s in p]
            w, h = LEAF_SIZE[lv]
            print("%-5s level %d  %dx%d  %d patterns  range %+d..%+d  mean |s| %.2f"
                  % (path, lv, w, h, len(pats), min(flat), max(flat),
                     sum(abs(s) for s in flat) / len(flat)))

    print("\nintra level 1, layer 0, pattern 0 (4x4):")
    pat = pattern_for(books, "intra", 1, 0, 0)
    for r in range(4):
        print("   " + " ".join("%+4d" % s for s in pat[r * 4:(r + 1) * 4]))


if __name__ == "__main__":
    main()
