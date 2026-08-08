#!/usr/bin/env python3
"""Focused tests for the raw GBA VX++ audio driver."""

import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import vx_audio_decode as A


class AudioDecodeTest(unittest.TestCase):
    def test_signed_32_bit_wrap(self):
        self.assertEqual(A.s32(0xffffffff), -1)
        self.assertEqual(A.s32(0x80000000), -2147483648)
        self.assertEqual(A.s32(0x100000001), 1)

    def test_variable_aframe_sizes(self):
        for mode, words in enumerate(A.PULSE_DATA_LEN):
            frame = struct.pack("<HH", 0xfe00, mode << 12) + bytes(2 * words)
            self.assertEqual(A.aframe_size(frame, 0), len(frame))

    def test_zero_filter_intra_decodes_pulse_grid(self):
        extra = bytearray(A.EXTRADATA_SIZE)
        scale_modifiers = 3 * 64 * 8 * 2
        struct.pack_into("<H", extra, scale_modifiers, 8192)
        struct.pack_into("<I", extra, A.EXTRADATA_SIZE - 4, 1)
        state = A.State(A.ExtraData(extra))
        frame = struct.pack("<5H", 0xfe00, 0x3000, 0, 0, 0)
        samples, end, intra = A.decode_aframe(state, frame, 0)
        self.assertEqual(end, len(frame))
        self.assertTrue(intra)
        self.assertEqual(samples[:16], [-3, 0, 0, 0, 0, -3, 0, 0,
                                        0, 0, -3, 0, 0, 0, 0, -3])


if __name__ == "__main__":
    unittest.main()
