#!/usr/bin/env python3
"""Focused tests for the Nintendo Pokemon FVMV parser."""

import importlib.util
from pathlib import Path
import struct
import unittest


MODULE = Path(__file__).with_name("fvmv_decode.py")
SPEC = importlib.util.spec_from_file_location("fvmv_decode", MODULE)
F = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(F)


def packet(video_size=0x20, audio_size=0x50):
    total = video_size + 8 + audio_size
    data = bytearray(0x10 + total)
    struct.pack_into("<I", data, 0, total)
    struct.pack_into("<I", data, 0x10, video_size)
    struct.pack_into("<II", data, 0x10 + video_size, 0, audio_size)
    return data


def stream(records):
    body = b"".join(records)
    data = bytearray(0x20) + body
    struct.pack_into("<4sIIIII", data, 0, b"FVMV", len(records),
                     240, 160, 1704, len(body))
    return data


class FVMVParserTest(unittest.TestCase):
    def test_find_and_iterate(self):
        one = packet()
        two = packet(0x28, 0x78)
        rom = bytes(0x100) + stream([one, two]) + bytes(0x40)
        streams = F.find_streams(rom)
        self.assertEqual(len(streams), 1)
        self.assertEqual(streams[0].offset, 0x100)
        packets = [p for _, p in F.iter_packets(rom, streams[0])]
        self.assertEqual([p.video_size for p in packets], [0x20, 0x28])
        self.assertEqual([p.audio_size for p in packets], [0x50, 0x78])
        self.assertEqual(packets[-1].next, streams[0].data_end)

    def test_reject_bad_geometry(self):
        data = stream([packet()])
        struct.pack_into("<I", data, 8, 256)
        self.assertEqual(F.find_streams(data), [])

    def test_find_iwram_by_decoder_entry(self):
        base = 0x400
        rom = bytearray(base + F.IWRAM_IMAGE_SIZE)
        off = base + F.VIDEO_DECODER_IWRAM_OFF
        rom[off:off + len(F.VIDEO_DECODER_PREFIX)] = F.VIDEO_DECODER_PREFIX
        found, image = F.find_iwram_image(rom)
        self.assertEqual(found, base)
        self.assertEqual(len(image), F.IWRAM_IMAGE_SIZE)


if __name__ == "__main__":
    unittest.main()
