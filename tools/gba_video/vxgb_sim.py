#!/usr/bin/env python3
"""Bit-exact parser for the original ``VXGB`` GBA Video revision.

VXGB and VX++ share the recursive prediction grammar, but their residual
coders differ.  VX++ uses a cartridge-ROM VLC codebook; VXGB uses the older
ActImagine/H.264-style CAVLC coder also found in the DS codec.  This module
maps the sixteen solved VX++ dispatchers to their byte-matched VXGB functions
and implements the CAVLC walk from the retail decoder at 0x030062e0.

The parser intentionally reconstructs no pixels yet.  Its regression oracle
    is the container seek table: every segment end is an independently recorded
    video bit offset.  Unlike VX++, VXGB takes its quantizer from the container
    header and has neither a per-segment quantizer delta nor a frame marker.
"""

import os
import re

from vx_grammar import GRAMMAR as VXPP_GRAMMAR, TOP as VXPP_TOP
from vx_reconstruct import BLOCK_SIZE as VXPP_BLOCK_SIZE
from vx_sim import Bits, read_se, read_ue


# Sequence matching the two retail IWRAM images maps every dispatcher exactly.
_VXPP_DISPATCHERS = (
    0x03001dac, 0x03002184, 0x030024ac, 0x030028ec,
    0x030030f4, 0x03003584, 0x03003818, 0x03003adc,
    0x030041cc, 0x03004468, 0x03004750, 0x03004950,
    0x03004fd0, 0x03005294, 0x03005494, 0x030055fc,
)
_VXGB_DISPATCHERS = (
    0x03001ee0, 0x030023c0, 0x03002740, 0x03002be0,
    0x03003510, 0x03003a70, 0x03003d50, 0x03004070,
    0x030047c0, 0x03004ab0, 0x03004df0, 0x03005040,
    0x03005740, 0x03005a60, 0x03005cb0, 0x03005e70,
)
DISPATCHER_MAP = dict(zip(_VXPP_DISPATCHERS, _VXGB_DISPATCHERS))
TOP = DISPATCHER_MAP[VXPP_TOP]
BLOCK_SIZE = {DISPATCHER_MAP[k]: v for k, v in VXPP_BLOCK_SIZE.items()
              if k in DISPATCHER_MAP}


def _map_event(event):
    if event[0] == 'disp':
        return ('disp', DISPATCHER_MAP[event[1]])
    return event


GRAMMAR = {
    DISPATCHER_MAP[disp]: {
        mode: (n_se, [_map_event(event) for event in events])
        for mode, (n_se, events) in table.items()
    }
    for disp, table in VXPP_GRAMMAR.items()
}


# 0x03005e90, indexed by the residual ue(v). Bits 0..3 are the four
# raster-ordered luma 4x4 blocks and bit 4 codes the Cb/Cr pair.
CBP_PERMTAB = bytes.fromhex(
    '00 01 02 04 08 10 03 05 0a 0c 0f 1f 07 0b 0d 0e '
    '06 09 13 15 1a 1c 11 12 14 18 17 1b 1d 1e 16 19'
)

CAVLC_SUFFIX_LIMIT = (0, 3, 6, 12, 24, 48, 0x8000)


class CAVLCTables:
    """Load the standard table values already shared with libavcodec/vx.c."""

    def __init__(self, path=None):
        if path is None:
            root = os.path.dirname(os.path.dirname(os.path.dirname(
                os.path.abspath(__file__))))
            path = os.path.join(root, 'libavcodec', 'vx_cavlc_vlc.h')
        with open(path) as f:
            text = f.read()

        def array(name):
            match = re.search(
                r'static const int8_t\s+' + re.escape(name) +
                r'(?:\[[^]]+\])+\s*=\s*\{(.*?)\};', text, re.S)
            if not match:
                raise ValueError('CAVLC table %s not found in %s' % (name, path))
            return [int(v) for v in re.findall(r'-?\d+', match.group(1))]

        flat_len = array('vx_coeff_token_len')
        flat_bits = array('vx_coeff_token_bits')
        self.coeff_len = [flat_len[i:i + 68] for i in range(0, 272, 68)]
        self.coeff_bits = [flat_bits[i:i + 68] for i in range(0, 272, 68)]
        self.total_len = [None] + [array('vx_total_zeros_len_%d' % i)
                                   for i in range(1, 16)]
        self.total_bits = [None] + [array('vx_total_zeros_bits_%d' % i)
                                    for i in range(1, 16)]
        self.run_len = [None] + [array('vx_run_len_%d' % i)
                                 for i in range(1, 7)]
        self.run_bits = [None] + [array('vx_run_bits_%d' % i)
                                  for i in range(1, 7)]
        self.run7_len = array('vx_run7_len')
        self.run7_bits = array('vx_run7_bits')


def _read_bits(br, pos, count):
    if not count:
        return 0, pos
    return br.peek32(pos) >> (32 - count), pos + count


def _read_vlc(br, pos, lengths, codes):
    for symbol, length in enumerate(lengths):
        if length and br.peek32(pos) >> (32 - length) == codes[symbol]:
            return symbol, pos + length
    raise ValueError('invalid CAVLC code at bit %d' % pos)


class FrameState:
    """Neighbouring TotalCoeff values used to select coeff_token tables."""

    def __init__(self):
        self.luma = {}
        self.chroma = {}

    @staticmethod
    def _nc(grid, x, y):
        return (grid.get((x - 1, y), 0) + grid.get((x, y - 1), 0) + 1) // 2

    def decode_block(self, br, tables, pos, x, y, chroma=False):
        grid = self.chroma if chroma else self.luma
        nc = self._nc(grid, x, y)
        table = (0 if nc < 2 else 1 if nc < 4 else 2 if nc < 8 else 3)
        token, pos = _read_vlc(br, pos, tables.coeff_len[table],
                               tables.coeff_bits[table])
        total_coeff, trailing_ones = token >> 2, token & 3
        grid[(x, y)] = total_coeff
        if not total_coeff:
            return pos, 0, [0] * 16

        if total_coeff == 16:
            zeros_left = 0
        else:
            zeros_left, pos = _read_vlc(br, pos,
                                        tables.total_len[total_coeff],
                                        tables.total_bits[total_coeff])

        suffix_length = 0
        levels = [0] * 16
        write_pos = 15
        if total_coeff < 16:
            write_pos -= 16 - (total_coeff + zeros_left)
        for coeff in range(total_coeff):
            if coeff < trailing_ones:
                sign, pos = _read_bits(br, pos, 1)
                value = -1 if sign else 1
            else:
                prefix = 0
                while True:
                    bit, pos = _read_bits(br, pos, 1)
                    if bit:
                        break
                    prefix += 1
                    if prefix > 32:
                        raise ValueError('invalid CAVLC level prefix')
                suffix_bits = 11 if prefix == 15 else suffix_length
                suffix, pos = _read_bits(br, pos, suffix_bits)
                level_code = (prefix << suffix_length) + suffix + 1
                if (suffix_length < 6 and
                        level_code > CAVLC_SUFFIX_LIMIT[suffix_length + 1]):
                    suffix_length += 1
                sign, pos = _read_bits(br, pos, 1)
                value = -level_code if sign else level_code

            if write_pos < 0:
                raise ValueError('CAVLC coefficient position underflow')
            levels[write_pos] = value
            write_pos -= 1

            if coeff + 1 < total_coeff and zeros_left:
                if zeros_left < 7:
                    run, pos = _read_vlc(br, pos, tables.run_len[zeros_left],
                                         tables.run_bits[zeros_left])
                else:
                    run, pos = _read_vlc(br, pos, tables.run7_len,
                                         tables.run7_bits)
                zeros_left -= run
                if zeros_left < 0:
                    raise ValueError('CAVLC run exceeds zeros_left')
                write_pos -= run
        return pos, total_coeff, levels

    def decode_residual(self, br, tables, pos, x, y, width, height):
        groups = []
        for gy in range(y, y + height, 8):
            for gx in range(x, x + width, 8):
                cbp, pos = read_ue(br, pos)
                if cbp is None or cbp >= len(CBP_PERMTAB):
                    raise ValueError('bad VXGB cbp %r at bit %d' % (cbp, pos))
                mask = CBP_PERMTAB[cbp]
                blocks = []
                for bit, (dx, dy) in enumerate(((0, 0), (1, 0),
                                                (0, 1), (1, 1))):
                    bx, by = gx // 4 + dx, gy // 4 + dy
                    if mask & (1 << bit):
                        pos, _, coeffs = self.decode_block(br, tables, pos, bx, by)
                        blocks.append((bit, coeffs))
                    else:
                        self.luma[(bx, by)] = 0
                cx, cy = gx // 8, gy // 8
                if mask & 0x10:
                    # Both colour blocks use the same predicted nC; the retail
                    # decoder stores their rounded mean for the next block.
                    pos, cb, cb_coeffs = self.decode_block(
                        br, tables, pos, cx, cy, True)
                    # Remove Cb's temporary result so Cr sees the same left/top
                    # neighbours, rather than Cb at the current coordinate.
                    self.chroma.pop((cx, cy), None)
                    pos, cr, cr_coeffs = self.decode_block(
                        br, tables, pos, cx, cy, True)
                    self.chroma[(cx, cy)] = (cb + cr + 1) // 2
                    blocks.extend(((4, cb_coeffs), (5, cr_coeffs)))
                else:
                    self.chroma[(cx, cy)] = 0
                mask6 = mask | (0x20 if mask & 0x10 else 0)
                groups.append((mask6, blocks))
        return pos, groups


def decode_unit(br, tables, state, pos, x, y, disp=TOP, depth=0, sink=None):
    """Consume one recursive VXGB prediction node and its optional residual."""
    if depth > 14:
        raise ValueError('partition nested too deep')
    mode, pos = read_ue(br, pos)
    table = GRAMMAR.get(disp)
    if table is None or mode not in table:
        raise ValueError('mode %r not in VXGB table %#x at bit %d' %
                         (mode, disp, pos))
    n_se, events = table[mode]
    side = []
    for _ in range(n_se):
        value, pos = read_se(br, pos)
        side.append(value)
    width, height = BLOCK_SIZE[disp]
    node = {'disp': disp, 'mode': mode, 'x': x, 'y': y,
            'size': (width, height), 'se': side, 'depth': depth}
    child_index = 0
    for event in events:
        if event[0] == 'disp':
            child_disp = event[1]
            child_width, child_height = BLOCK_SIZE[child_disp]
            child_x = x + (width // 2 if child_index and child_width < width else 0)
            child_y = y + (height // 2 if child_index and child_height < height else 0)
            pos = decode_unit(br, tables, state, pos, child_x, child_y,
                              child_disp, depth + 1, sink)
            child_index += 1
        elif event[0] == 'intra2':
            luma, pos = read_ue(br, pos)
            chroma, pos = read_ue(br, pos)
            node['intra'] = (luma, chroma)
        elif event[0] == 'ue1':
            flags = []
            for _ in range(event[1]):
                flag, pos = _read_bits(br, pos, 1)
                if flag:
                    flags.append(None)
                else:
                    rem, pos = _read_bits(br, pos, 3)
                    flags.append(rem)
            chroma, pos = read_ue(br, pos)
            node['intra4'] = (flags, chroma)
        elif event[0] == 'resid':
            pos, node['resid'] = state.decode_residual(
                br, tables, pos, x, y, width, height)
    if sink:
        sink(node)
    return pos


def decode_segment(br, tables, start_bit, n_frames, width, height, sink=None):
    """Consume one independently decodable seek segment."""
    pos = start_bit
    for frame in range(n_frames):
        state = FrameState()
        for y in range(0, height, 16):
            for x in range(0, width, 16):
                pos = decode_unit(br, tables, state, pos, x, y, sink=sink)
    return pos


def validate_seek(video, seek, nb_frames, width, height):
    """Return ``(exact, mismatches)`` against container-recorded bit offsets."""
    br = Bits(video)
    tables = CAVLCTables()
    real = [entry for entry in seek if entry[1] or entry[0] == 0]
    exact, bad = 0, []
    for i, (frame, bit, _, _) in enumerate(real):
        next_frame = real[i + 1][0] if i + 1 < len(real) else nb_frames
        got = decode_segment(br, tables, bit, next_frame - frame, width, height)
        want = real[i + 1][1] if i + 1 < len(real) else None
        if want is None or got == want:
            exact += 1
        else:
            bad.append((frame, want, got))
    return exact, bad
