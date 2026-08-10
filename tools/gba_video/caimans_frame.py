#!/usr/bin/env python3
"""Decode a Caimans Pro **intra** picture end to end.

The intra path needs no motion vectors and no motion compensation -- an intra
picture is just the picture header followed by three planes of macroblocks,
each decoded by the block layer in `caimans_blocks`. That makes it reachable
with what has been reverse engineered so far, and decoding one is the first
real end-to-end check on the whole chain.

Inter pictures are **not** implemented: they need the macroblock-type layer,
the MV VLC and the four half-pel routines. See the handoff doc's next-work
list.

Layout, from FUN_03005a00's buffer setup:

    luma   (w + 3 & ~3) padded up to a multiple of 16, likewise the height
    chroma ((w + 3) >> 2) padded to 16, likewise the height -- so chroma is
           quarter resolution on *both* axes, and there are two such planes
    frame  Y at offset 0, U at luma_w*luma_h, V after that

For the 240x160 sample that is Y 240x160 at 0, U 64x48 at 0x9600, V at 0xa200,
44544 bytes total -- and 0x9600/0xa200 are exactly the constants the routine
writes into its state, which is what pins the layout down.

Usage:

    python3 tools/gba_video/caimans_frame.py            # first picture in the ROM
    python3 tools/gba_video/caimans_frame.py --index 3 --out /tmp/f3.pgm
"""
import argparse
import os
import struct

from caimans_blocks import BitReader, BlockDecoder
from caimans_codebooks import Images, ROM_PATH

# Container, established by structure in the ROM (see the handoff doc).
INDEX_TABLE = 0x0000C4B0   # 87 x uint32, zero terminated
DATA_BASE = 0x0000C768     # index entries are relative to this

# Picture header
SIZE_TABLE = 0x030070B4    # DAT_030065ec: 8 x {uint16 width, uint16 height}
START_CODE = 0x20
U_PLANE_OFFSET = 0x9600    # param_1[0xc], a literal in FUN_03005a00
V_PLANE_OFFSET = 0xA200    # param_1[0xd]
VALID_START = 0x70         # (v & ~0x70) must be 0 and (v & 0x60) must be set


class HeaderError(Exception):
    pass


def read_index(rom):
    out = []
    off = INDEX_TABLE
    while True:
        v = struct.unpack_from("<I", rom, off)[0]
        if v == 0 and out:
            return out
        out.append(v)
        off += 4


def descramble(data):
    """The non-0x20 start-code obfuscation: 8 words at +4, first 4 rewritten.

    w[i] = rot16(w[i]) ^ w[7 - i], for i in 0..3.
    """
    words = list(struct.unpack_from("<8I", data, 4))
    out = bytearray(data)
    for i in range(4):
        v = words[i]
        mixed = (((v >> 16) | (v << 16)) & 0xFFFFFFFF) ^ words[7 - i]
        struct.pack_into("<I", out, 4 + i * 4, mixed)
    return bytes(out)


class Picture:
    """The fields of the picture header this decoder actually uses."""

    def __init__(self, start_code, ptype, width, height):
        self.start_code = start_code
        self.ptype = ptype
        self.width = width
        self.height = height

    @property
    def is_intra(self):
        return self.ptype == 0

    def planes(self):
        """[(offset, width, height)] for Y, U, V.

        The chroma plane offsets are the **fixed constants** the routine
        writes into its state (`param_1[0xc] = 0x9600`, `param_1[0xd] =
        0xa200`), sized for a full 240x160 luma plane -- they are not derived
        from this picture's dimensions.
        """
        lw = (((self.width + 3) & ~3) + 15) & ~15
        lh = (((self.height + 3) & ~3) + 15) & ~15
        cw = (((self.width + 3) >> 2) + 15) & ~15
        ch = (((self.height + 3) >> 2) + 15) & ~15
        return [(0, lw, lh), (U_PLANE_OFFSET, cw, ch), (V_PLANE_OFFSET, cw, ch)]

    def buffer_size(self):
        (_, _, _), (_, cw, ch), (voff, _, _) = self.planes()
        return voff + cw * ch


def parse_header(br, img):
    """Transcription of FUN_03005a00's header parse. Returns a Picture."""
    start = br.peek32() >> (32 - 22)
    br.skip(22)
    if (start & ~VALID_START) != 0 or (start & 0x60) == 0:
        raise HeaderError("bad start code 0x%x" % start)

    br.skip(8)                      # temporal reference
    ptype = br.peek32() >> 30
    br.skip(2)

    width = height = None
    if ptype == 0:
        if start in (0x50, 0x60):
            br.skip(16)
        if (start ^ 0x10) > 0x4F:
            n = br.peek32() >> 24
            br.skip(8)
            br.skip(n * 8)          # a byte-counted extension field
        br.skip(2)
        br.skip(2)
        br.skip(1)
        fmt = br.peek32() >> 29
        br.skip(3)
        if fmt == 7:
            width = br.peek32() >> 20
            br.skip(12)
            height = br.peek32() >> 20
            br.skip(12)
        else:
            width, height = struct.unpack("<HH", img.read(SIZE_TABLE + fmt * 4, 4))

    if br.bit() == 1:
        br.bit()
        br.bit()
        if (br.peek32() >> 30) != 0:
            raise HeaderError("unsupported header extension")
        br.skip(2)
    if br.bit() == 1:
        br.bit()
        br.skip(4)
        br.bit()
        n = 2
        while True:
            br.skip(n)
            n = 8
            if br.bit() != 1:
                break

    return Picture(start, ptype, width, height)


def decode_intra_picture(data, dec=None, img=None):
    """Decode one intra picture. Returns (Picture, framebuffer bytearray)."""
    img = img or Images()
    dec = dec or BlockDecoder(img)

    if data[0] == 0 and data[1] == 0 and data[2] not in range(0x80, 0xC0):
        pass  # let the header parser complain
    peek = int.from_bytes(data[:3], "big") >> 2
    if peek != START_CODE:
        data = descramble(data)

    br = BitReader(data)
    pic = parse_header(br, img)
    if not pic.is_intra:
        raise HeaderError("picture type %d is not intra" % pic.ptype)

    buf = bytearray(pic.buffer_size())
    for offset, w, h in pic.planes():
        for y in range(0, h, 16):
            for x in range(0, w, 16):
                dec.decode_macroblock(br, buf, offset + y * w + x, w, intra=True)
    return pic, buf, br


def parse_header_sized(br, img, width, height):
    """Like parse_header, but supplies the size for an inter picture (which
    carries none of its own -- FUN_03005a00 reuses the previous intra
    picture's frame_size / width / height fields, param_1[2]/[3])."""
    pic = parse_header(br, img)
    if pic.width is None:
        pic.width, pic.height = width, height
    return pic


def _require_size(pic):
    if pic.width is None:
        raise HeaderError("inter picture with no prior size available")
    return pic


def decode_picture(data, dec_intra, dec_inter, prev_buf=None, prev_size=None, img=None):
    """Decode one picture of either type. Returns (Picture, buf, BitReader).

    `prev_buf`/`prev_size` (a (width, height) pair) are required for an inter
    picture: its own header carries no dimensions, and its macroblocks read
    the reference frame out of `prev_buf`.
    """
    img = img or Images()
    peek = int.from_bytes(data[:3], "big") >> 2
    if peek != START_CODE:
        data = descramble(data)
    br = BitReader(data)

    if prev_size is not None:
        pic = parse_header_sized(br, img, *prev_size)
    else:
        pic = _require_size(parse_header(br, img))

    buf = bytearray(pic.buffer_size())
    if pic.is_intra:
        for offset, w, h in pic.planes():
            for y in range(0, h, 16):
                for x in range(0, w, 16):
                    dec_intra.decode_macroblock(br, buf, offset + y * w + x, w, intra=True)
    else:
        if prev_buf is None:
            raise HeaderError("inter picture with no reference frame")
        for offset, w, h in pic.planes():
            dec_inter.decode_plane(br, buf, prev_buf, offset, w, w, h)
    return pic, buf, br


def write_pgm(path, buf, offset, w, h):
    with open(path, "wb") as f:
        f.write(b"P5\n%d %d\n255\n" % (w, h))
        for row in range(h):
            f.write(bytes(buf[offset + row * w:offset + row * w + w]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", default=ROM_PATH)
    ap.add_argument("--index", type=int, default=0,
                    help="entry in the ROM index table to decode")
    ap.add_argument("--out", default=None, help="write the luma plane as a PGM")
    args = ap.parse_args()

    with open(args.rom, "rb") as f:
        rom = f.read()
    index = read_index(rom)
    off = DATA_BASE + index[args.index]
    print("index entry %d -> ROM 0x%x" % (args.index, off))
    print("first bytes: %s" % " ".join("%02x" % b for b in rom[off:off + 12]))

    img = Images()
    pic, buf, br = decode_intra_picture(rom[off:off + 0x4E20], img=img)
    print("start code 0x%x, ptype %d, %dx%d" % (pic.start_code, pic.ptype, pic.width, pic.height))
    print("planes: %s" % pic.planes())
    print("consumed %d bits (%d bytes) of a %d byte limit" % (br.pos, (br.pos + 7) // 8, 0x4E20))

    y_off, y_w, y_h = pic.planes()[0]
    luma = buf[y_off:y_off + y_w * y_h]
    print("luma: min %d max %d mean %.1f" % (min(luma), max(luma), sum(luma) / len(luma)))
    if args.out:
        write_pgm(args.out, buf, y_off, y_w, y_h)
        print("wrote %s" % args.out)


if __name__ == "__main__":
    main()
