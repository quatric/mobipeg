#!/usr/bin/env python3
"""Focused tests for the original VXGB bitstream parser."""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import vxgb_sim as V
import vxgb_decode as D


class VXGBParserTest(unittest.TestCase):
    def test_all_sixteen_dispatchers_map_to_known_block_sizes(self):
        self.assertEqual(len(V.DISPATCHER_MAP), 16)
        self.assertEqual(set(V.GRAMMAR), set(V.DISPATCHER_MAP.values()))
        self.assertEqual(V.BLOCK_SIZE[V.TOP], (16, 16))

    def test_original_cbp_permutation(self):
        self.assertEqual(len(V.CBP_PERMTAB), 32)
        self.assertEqual(V.CBP_PERMTAB[:12],
                         bytes((0, 1, 2, 4, 8, 16, 3, 5, 10, 12, 15, 31)))

    def test_cavlc_zero_coefficient_token(self):
        # Bits swaps each little-endian halfword before reading it MSB-first.
        # 00 80 therefore presents a leading 1, the nC=0 TotalCoeff=0 token.
        br = V.Bits(bytes((0x00, 0x80)))
        state = V.FrameState()
        pos, total, coeffs = state.decode_block(
            br, V.CAVLCTables(), 0, 0, 0)
        self.assertEqual((pos, total), (1, 0))
        self.assertEqual(coeffs, [0] * 16)
        self.assertEqual(state.luma[(0, 0)], 0)

    def test_gba_transform_keeps_standard_h264_scan_order(self):
        # VXGB shares CAVLC with the DS codec, but not its transposed scan.
        # The GBA inverse transform loads coefficients in standard H.264
        # zigzag order, which is already what vx_reconstruct accepts.
        coeffs = list(range(16))
        unit = {'x': 4, 'y': 8, 'resid': [(1, [(0, coeffs[:])])]}
        adapted = D._adapt_unit(unit, 0, 0)
        self.assertEqual(adapted['off'], 8 * D.R.STRIDE + 4)
        self.assertEqual(adapted['resid'][0][1][0][1], coeffs)


if __name__ == '__main__':
    unittest.main()
