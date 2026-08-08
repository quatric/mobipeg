#!/usr/bin/env python3
"""Disassemble a range of the VX++ decoder's IWRAM image.

The decoder runs from IWRAM (0x03000000), but the image is copied there from a
fixed spot in cartridge ROM, so it can be lifted statically -- no emulator, no
GDB stub. gbavx_extract.py -o writes it out as iwram.bin.

    ./vx_dis.py 0x03000ba4 0x60          # 0x60 bytes from an address
    ./vx_dis.py 0x03000ba4               # 0x100 bytes by default
    ./vx_dis.py 0x03001d4c 64 -w         # dump as words instead (tables)

Data words inside code are normal here (ARM literal pools), and capstone stops
at the first one, so decode strictly word by word and print the undecodable ones
as `.word` rather than truncating the listing there.
"""

import os
import struct
import sys

from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM

BASE = 0x03000000
IWRAM = os.path.join(
    os.environ.get("VXPP_DATA",
                   os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(
                       os.path.abspath(__file__)))), "build_gbavx")),
    "iwram.bin")


def load():
    with open(IWRAM, "rb") as f:
        return f.read()


def disasm(img, addr, length):
    md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    out = []
    for a in range(addr, addr + length, 4):
        off = a - BASE
        if off < 0 or off + 4 > len(img):
            break
        word = struct.unpack_from("<I", img, off)[0]
        ins = list(md.disasm(img[off:off + 4], a))
        if ins:
            i = ins[0]
            text = "%-8s %s" % (i.mnemonic, i.op_str)
            # Resolve pc-relative literal loads, the usual reason a listing is
            # unreadable: [pc, #N] is 8 bytes ahead on ARM.
            if "[pc, #" in i.op_str:
                try:
                    disp = int(i.op_str.split("[pc, #")[1].split("]")[0], 0)
                    tgt = a + 8 + disp
                    val = struct.unpack_from("<I", img, tgt - BASE)[0]
                    text += "    ; =0x%08x (@0x%08x)" % (val, tgt)
                except (IndexError, ValueError, struct.error):
                    pass
        else:
            text = ".word    0x%08x" % word
        out.append("%08x  %08x  %s" % (a, word, text))
    return out


def words(img, addr, count):
    out = []
    for i in range(count):
        off = addr - BASE + 4 * i
        v = struct.unpack_from("<I", img, off)[0]
        out.append("%08x  [%3d] 0x%08x  %d" % (addr + 4 * i, i, v, v))
    return out


def main(argv):
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    as_words = "-w" in argv
    argv = [a for a in argv if a != "-w"]
    addr = int(argv[1], 0)
    n = int(argv[2], 0) if len(argv) > 2 else 0x100
    img = load()
    print("\n".join(words(img, addr, n) if as_words else disasm(img, addr, n)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
