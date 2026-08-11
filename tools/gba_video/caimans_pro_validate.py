#!/usr/bin/env python3
"""Compare consecutive Caimans Pro reference frames with the live player.

At entry to the next call of `FUN_03005a00`, the descriptor's word at +0x28
points to the fully decoded previous picture.  This makes a stable validation
point: unlike instruction-level traces, it is after all macroblocks and before
the next picture can reuse the buffer.
"""
import argparse
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from caimans_blocks import BlockDecoder
from caimans_codebooks import Images, ROM_PATH
from caimans_frame import decode_picture
from caimans_inter import InterDecoder
from gdbrsp import RSP

MGBA = "/Applications/mGBA.app/Contents/MacOS/mGBA"
PICTURE_FN = 0x03005A00
ROM_BASE = 0x08000000
TERMINAL_OFFSET = 0x147354  # player-entered non-picture sentinel
OUTPUT_PTR_OFFSET = 0x28


def launch(rom, port):
    proc = subprocess.Popen([MGBA, "-g", rom], stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    for _ in range(100):
        time.sleep(0.1)
        try:
            return proc, RSP(port=port)
        except OSError:
            pass
    proc.kill()
    raise RuntimeError("could not connect to mGBA on port %d" % port)


def diff_planes(pic, ours, theirs):
    result = []
    for name, (offset, width, height) in zip("YUV", pic.planes()):
        count = width * height
        a, b = ours[offset:offset + count], theirs[offset:offset + count]
        result.append((name, sum(x != y for x, y in zip(a, b)), count))
    return result


def first_differences(pic, ours, theirs, limit=32):
    """Return compact (plane, x, y, reference, hardware) mismatch samples."""
    found = []
    for name, (offset, width, height) in zip("YUV", pic.planes()):
        for y in range(height):
            for x in range(width):
                pos = offset + y * width + x
                if ours[pos] != theirs[pos]:
                    found.append((name, x, y, ours[pos], theirs[pos]))
                    if len(found) == limit:
                        return found
    return found


def validate(rom_path, port=2345, calls=100, details=False):
    """Validate player-delimited pictures from stream start, through looping."""
    rom = open(rom_path, "rb").read()
    image = Images(rom_path=rom_path)
    intra, inter = BlockDecoder(image), InterDecoder(image)
    proc, rsp = launch(rom_path, port)
    offsets = []
    reference = None
    size = None
    bad = []
    try:
        rsp.bp(PICTURE_FN)
        for call in range(calls):
            rsp.cont()
            regs = rsp.regs()
            offset = regs[1] - ROM_BASE
            if not (0 <= offset < len(rom)):
                raise RuntimeError("call %d pointer is outside ROM" % call)
            # The player signals end-of-stream by entering the picture routine
            # again with the prior picture address.  There is no picture at an
            # empty/reversed interval, so do not try to descramble it.
            if offsets and offset <= offsets[-1]:
                if offsets[-1] != TERMINAL_OFFSET:
                    raise RuntimeError("unexpected terminal pointer 0x%x" % offsets[-1])
                print("end of picture stream at call %d (counter %d, ROM 0x%x)" %
                      (call, regs[3], offset))
                break
            offsets.append(offset)
            if call == 0:
                continue

            pic, reference, _ = decode_picture(
                rom[offsets[-2]:offset], intra, inter, reference, size, image)
            size = pic.width, pic.height
            output = int.from_bytes(rsp.mem(regs[0] + OUTPUT_PTR_OFFSET, 4), "little")
            hardware = rsp.mem(output, pic.buffer_size())
            planes = diff_planes(pic, reference, hardware)
            differing = sum(d for _, d, _ in planes)
            if differing:
                bad.append((call - 1, regs[3], offsets[-2], differing, planes))
                print("call %d (counter %d, ROM 0x%x): %d differing bytes %s" %
                      (call - 1, regs[3], offsets[-2], differing, planes))
                if details:
                    print("  first differences: %s" %
                          first_differences(pic, reference, hardware))
            elif call <= 5 or call in (60, 61, 62, 63, 64):
                print("call %d (counter %d, ROM 0x%x): identical" %
                      (call - 1, regs[3], offsets[-2]))
    finally:
        if proc.poll() is None:
            proc.kill()
    # `offsets` includes the terminal non-picture call.
    return max(0, len(offsets) - 1), bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", default=ROM_PATH)
    ap.add_argument("--port", type=int, default=2345)
    ap.add_argument("--calls", type=int, default=100)
    ap.add_argument("--details", action="store_true",
                    help="print coordinates and values for the first mismatches")
    args = ap.parse_args()
    checked, bad = validate(args.rom, args.port, args.calls, args.details)
    print("checked %d pictures; %d mismatched" % (checked, len(bad)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
