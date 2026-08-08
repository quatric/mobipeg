#!/usr/bin/env python3
"""Focused reconstruction geometry tests for vx_decode."""

import os
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import vx_decode as D


class ResidualGeometryTest(unittest.TestCase):
    @staticmethod
    def offsets(size):
        groups = [(0x11, [(0, [q] * 16), (4, [q] * 16)])
                  for q in range((size[0] // 8) * (size[1] // 8))]
        unit = {"size": size, "resid": groups}
        calls = []
        with patch.object(D.R, "idct4x4", side_effect=lambda coeffs, qp: coeffs), \
             patch.object(D.R, "add_residual",
                          side_effect=lambda buf, off, residual, plane=0:
                          calls.append((off, plane))):
            D._residual(D.Frame(16, 16), unit, 1000, 2000, 0)
        return calls

    def test_two_groups_follow_leaf_orientation(self):
        self.assertEqual(self.offsets((16, 8)), [
            (1000, 0), (2000, 1),
            (1008, 0), (2008, 1),
        ])
        self.assertEqual(self.offsets((8, 16)), [
            (1000, 0), (2000, 1),
            (1000 + 8 * D.STRIDE, 0),
            (2000 + 4 * D.STRIDE, 1),
        ])

    def test_four_groups_are_raster_ordered(self):
        self.assertEqual(self.offsets((16, 16)), [
            (1000, 0), (2000, 1),
            (1008, 0), (2008, 1),
            (1000 + 8 * D.STRIDE, 0),
            (2000 + 4 * D.STRIDE, 1),
            (1008 + 8 * D.STRIDE, 0),
            (2008 + 4 * D.STRIDE, 1),
        ])

    def test_mode5_chroma_dc_corrects_components_independently(self):
        off = D.MARGIN
        ref = bytearray([0]) * (off + 4 * D.STRIDE)
        dst = bytearray(ref)
        ref[off:off + 4] = bytes([250, 5, 20, 240])
        ref[off + D.STRIDE:off + D.STRIDE + 4] = bytes([1, 100, 80, 0])
        D.R.inter_copy_chroma_dc(dst, ref, off, 0, 0, 4, 4, (5, -10))
        self.assertEqual(dst[off:off + 4], bytes([255, 0, 30, 220]))
        self.assertEqual(dst[off + D.STRIDE:off + D.STRIDE + 4],
                         bytes([11, 80, 90, 0]))

    def test_luma_dc_averages_edges_in_two_rounded_stages(self):
        off = D.STRIDE + 1
        buf = bytearray([0]) * (10 * D.STRIDE)
        buf[off - D.STRIDE:off - D.STRIDE + 16] = bytes(
            [10] * 8 + [11] * 8)
        for row, value in enumerate([12] * 4 + [13] * 4):
            buf[off + row * D.STRIDE - 1] = value
        D.R.intra_dc(buf, off, 16, 8, True, True)
        self.assertEqual(buf[off:off + 16], bytes([12]) * 16)

    def test_intra4_dc_tests_plane_offset_not_host_margin(self):
        off = D.MARGIN + 8
        buf = bytearray([D.GREY]) * (off + 4 * D.STRIDE + 8)
        for row in range(4):
            buf[off + row * D.STRIDE - 1] = 64
        D.R.intra4x4(buf, off, 2, frame_off=8)
        for row in range(4):
            self.assertEqual(buf[off + row * D.STRIDE:off + row * D.STRIDE + 4],
                             bytes([64]) * 4)

    def test_dc_only_residual_uses_transform_rounding(self):
        buf = bytearray([100]) * (4 * D.STRIDE)
        D.R.add_residual_dc(buf, 0, 32)
        for row in range(4):
            self.assertEqual(buf[row * D.STRIDE:row * D.STRIDE + 4],
                             bytes([101]) * 4)

    def test_extracted_header_metadata(self):
        text = """frames          42
geometry        240x160
seek table (frame, video bit, audio byte):
         0             0        3124
        17          1234        9000
        42             0           0
"""
        with tempfile.NamedTemporaryFile("w", delete=False) as f:
            f.write(text)
            path = f.name
        try:
            self.assertEqual(D.load_header(path), {
                "frames": 42, "width": 240, "height": 160,
                "seek": [(0, 0, 3124, 0), (17, 1234, 9000, 0),
                         (42, 0, 0, 0)],
            })
        finally:
            os.unlink(path)

    def test_coded_split_applies_parent_residual(self):
        frame = D.Frame(16, 16)
        refs = [D.Frame(16, 16) for _ in range(3)]
        unit = {"mode": 13, "size": (16, 16), "off": 0, "se": [],
                "resid": []}
        with patch.object(D, "_residual") as residual:
            D._paint(frame, refs, unit, 0, 0, 0, (0, 0), [0], 0)
            residual.assert_called_once_with(frame, unit, D.MARGIN, D.MARGIN, 0)

        unit = {"mode": 1, "size": (16, 16), "off": 0, "se": []}
        with patch.object(D, "_residual") as residual:
            D._paint(frame, refs, unit, 0, 0, 0, (0, 0), [0], 0)
            residual.assert_not_called()

    def test_physical_frame_slots_expose_adjacent_allocations(self):
        arena = bytearray([D.GREY]) * (2 * D.MARGIN + 4 * D.FRAME_SLOT)
        slots = [D.Frame(240, 112, arena, i) for i in range(4)]
        # The byte immediately before slot 1 luma is the final byte of slot 0
        # chroma, just as an off-picture negative MV sees on hardware.
        slots[0].chroma[D.MARGIN + D.CHROMA_ALLOC - 1] = 37
        self.assertEqual(slots[1].luma[D.MARGIN - 1], 37)
        # Chroma's preceding byte is in the same slot's luma allocation.
        slots[1].luma[D.MARGIN + D.LUMA_ALLOC - 1] = 91
        self.assertEqual(slots[1].chroma[D.MARGIN - 1], 91)


if __name__ == "__main__":
    unittest.main()
