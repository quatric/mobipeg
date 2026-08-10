#!/usr/bin/env python3
"""Pixel-exact validation of the intra decoder against the real player.

Runs mGBA to the end of the first picture, reads the player's own decoded
planes straight out of its framebuffer, and compares them byte for byte with
`caimans_frame`'s output. This is the bar every other GBA video lineage in this
repo had to clear before any FFmpeg code was written.

    python3 tools/gba_video/caimans_validate_frame.py
"""
import argparse
import os
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from gdbrsp import RSP
from caimans_blocks import BitReader
from caimans_codebooks import Images, ROM_PATH
from caimans_frame import DATA_BASE, decode_intra_picture, read_index, write_pgm

MGBA = "/Applications/mGBA.app/Contents/MacOS/mGBA"
BLOCK_FN = 0x03004CD4
BITSTATE = 0x03000024
ROM_ADDR_BASE = 0x08000000


def launch(rom, port):
    proc = subprocess.Popen([MGBA, "-g", rom],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(40):
        time.sleep(0.5)
        try:
            return proc, RSP(port=port)
        except OSError:
            continue
    proc.kill()
    raise SystemExit("could not connect to mGBA on port %d" % port)


def capture(rom_path, port, nplanes_mb, nbytes):
    """Return (picture base address, plane base, framebuffer bytes)."""
    proc, rsp = launch(rom_path, port)
    try:
        rsp.bp(BLOCK_FN)
        rsp.cont()
        regs = rsp.regs()
        plane_base = regs[0]
        base = struct.unpack("<II", rsp.mem(BITSTATE, 8))[0]
        for _ in range(nplanes_mb - 1):
            rsp.cont()
        rsp.cont()  # first macroblock of the *next* picture: this one is done
        data = rsp.mem(plane_base, nbytes)
        return base, plane_base, data
    finally:
        proc.kill()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", default=ROM_PATH)
    ap.add_argument("--port", type=int, default=2345)
    ap.add_argument("--dump", default=None, help="write both luma planes as PGMs")
    args = ap.parse_args()

    with open(args.rom, "rb") as f:
        rom = f.read()

    img = Images()
    off = DATA_BASE + read_index(rom)[0]
    pic, mine, br = decode_intra_picture(rom[off:off + 0x4E20], img=img)
    planes = pic.planes()
    total_mb = sum((w // 16) * (h // 16) for _, w, h in planes)
    print("python: %dx%d, %d macroblocks, %d bits" % (pic.width, pic.height, total_mb, br.pos))

    base, plane_base, theirs = capture(args.rom, args.port, total_mb, pic.buffer_size())
    print("hardware: picture at 0x%08x, framebuffer at 0x%08x" % (base, plane_base))
    if base - ROM_ADDR_BASE != off:
        raise SystemExit("hardware decoded a different picture")

    ok = True
    for name, (poff, w, h) in zip("YUV", planes):
        a = bytes(mine[poff:poff + w * h])
        b = theirs[poff:poff + w * h]
        if a == b:
            print("  plane %s %3dx%-3d  %6d bytes  IDENTICAL" % (name, w, h, len(a)))
        else:
            diff = sum(1 for x, y in zip(a, b) if x != y)
            worst = max(abs(x - y) for x, y in zip(a, b))
            print("  plane %s %3dx%-3d  %6d bytes  %d differ (%.2f%%), max delta %d"
                  % (name, w, h, len(a), diff, 100.0 * diff / len(a), worst))
            ok = False

    if args.dump:
        write_pgm(args.dump + ".mine.pgm", mine, 0, 240, 128)
        write_pgm(args.dump + ".hw.pgm", bytearray(theirs), 0, 240, 128)
        print("wrote %s.mine.pgm and %s.hw.pgm" % (args.dump, args.dump))

    print("\n%s" % ("PIXEL EXACT" if ok else "MISMATCH"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
