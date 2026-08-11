#!/usr/bin/env python3
"""Decode Caimans 2.2 video records directly from the Bad Boys 2 ROM."""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from caimans22_frame import (CELL_SIZE, ROM_PATH, build_color_tables,
                             decode_codebook_chunk, decode_raster_chunk)

VIDEO_OFFSET = 0x2710
WIDTH = 240
FRAME_HEIGHT = 160
CODED_HEIGHT = 104
STRIDE = WIDTH * 2
DISPLAY_OFFSET = 0x3480
VIDEO_RATE = 16


class Decoder:
    def __init__(self):
        self.table_a, self.table_b, self.table_base = build_color_tables()
        self.simple_cache = bytearray(256 * CELL_SIZE)
        self.complex_cache = bytearray(256 * CELL_SIZE)
        self.framebuffer = bytearray(WIDTH * FRAME_HEIGHT * 2)

    def decode_record(self, record):
        if len(record) < 22:
            raise ValueError("truncated Caimans record header")
        declared = int.from_bytes(record[1:4], "big")
        if declared > len(record):
            raise ValueError("truncated Caimans record")
        chunk_bytes = struct.unpack_from("<H", record, 12)[0] - 12
        height = struct.unpack_from("<H", record, 18)[0]
        if height > FRAME_HEIGHT:
            raise ValueError("invalid coded height %d" % height)
        pos = 22
        end = pos + chunk_bytes
        tags = []
        while pos < end:
            if pos + 4 > len(record):
                raise ValueError("truncated sub-chunk header")
            tag, length = struct.unpack_from("<HH", record, pos)
            if length < 4 or pos + length > len(record):
                raise ValueError("invalid sub-chunk length %d" % length)
            data = record[pos + 4:pos + length]
            tags.append(tag)
            if tag in (0x2000, 0x2100, 0x2200, 0x2300):
                decode_codebook_chunk(tag, data, self.simple_cache,
                                      self.complex_cache, self.table_a,
                                      self.table_b, self.table_base)
            elif tag in (0x3000, 0x3100, 0x3200, 0x8000, 0x8100, 0x8200):
                decode_raster_chunk(tag, data, self.simple_cache,
                                    self.complex_cache, WIDTH, height, STRIDE,
                                    self.framebuffer, DISPLAY_OFFSET)
            pos += length
        return tags


def records(rom, start=VIDEO_OFFSET):
    pos = start
    while pos + 4 <= len(rom):
        if pos + 22 > len(rom) or rom[pos + 11] not in (0x10, 0x11):
            break
        length = int.from_bytes(rom[pos + 1:pos + 4], "big")
        if length < 22 or pos + length > len(rom):
            break
        yield pos, rom[pos:pos + length]
        pos += length + 8


def write_ppm(path, framebuffer):
    rgb = bytearray(WIDTH * FRAME_HEIGHT * 3)
    for i, pixel in enumerate(struct.unpack("<%dH" % (WIDTH * FRAME_HEIGHT), framebuffer)):
        rgb[i * 3:i * 3 + 3] = bytes(((pixel & 31) << 3,
                                      ((pixel >> 5) & 31) << 3,
                                      ((pixel >> 10) & 31) << 3))
    with open(path, "wb") as out:
        out.write(b"P6\n240 160\n255\n")
        out.write(rgb)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rom", default=ROM_PATH)
    parser.add_argument("--records", type=int, default=1)
    parser.add_argument("--output", default="caimans22.ppm")
    args = parser.parse_args()
    rom = open(args.rom, "rb").read()
    decoder = Decoder()
    count = 0
    for count, (offset, record) in enumerate(records(rom), 1):
        tags = decoder.decode_record(record)
        print("record %d at 0x%x: %s" %
              (count - 1, offset, ", ".join("0x%04x" % x for x in tags)))
        if count >= args.records:
            break
    write_ppm(args.output, decoder.framebuffer)
    print("wrote %s after %d record(s)" % (args.output, count))


if __name__ == "__main__":
    main()
