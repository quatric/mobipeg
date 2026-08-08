#!/usr/bin/env python3
"""Focused tests for the GBA VX++/VXGB container extractor."""

import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gbavx_extract as G


def synthetic_stream(magic, stream_off):
    """Return one small but internally consistent container fixture."""
    data = bytearray(0x200)
    struct.pack_into('<4s13I', data, 0,
                     magic, 14, 240, 160, 0x30c3, 32, 16384,
                     0x80, 0xc0, 7, 0xa0, 1, 7, 0)
    struct.pack_into('<4I', data, 0xa0, 0, 0, G.AUDIO_EXTRADATA_SIZE, 0)
    struct.pack_into('<2I', data, 0xc0, 1, 0)
    return stream_off, data


class ExtractTest(unittest.TestCase):
    def test_finds_both_revisions_in_rom_order(self):
        rom = bytearray(0x1000)
        for off, stream in (synthetic_stream(b'VXGB', 0x200),
                            synthetic_stream(b'VX++', 0x600)):
            rom[off:off + len(stream)] = stream
        streams = G.find_streams(rom)
        self.assertEqual([(s.off, s.magic) for s in streams],
                         [(0x200, b'VXGB'), (0x600, b'VX++')])
        self.assertEqual([s.fps for s in streams], [7.0, 7.0])

    def test_rejects_magic_inside_unrelated_data(self):
        rom = bytearray(0x1000)
        rom[3:7] = b'VXGB'
        rom[0x303:0x307] = b'VX++'
        self.assertEqual(G.find_streams(rom), [])

    def test_decoder_rom_images_are_revision_specific(self):
        self.assertEqual(G.VXGB_TABLES['iwram.bin'], (0xb548, 0x8000))
        self.assertEqual(G.VXPP_TABLES['iwram.bin'], (0xbcc8, 0x8000))
        self.assertNotIn('vlc_rom_full4096.bin', G.VXGB_TABLES)


if __name__ == '__main__':
    unittest.main()
