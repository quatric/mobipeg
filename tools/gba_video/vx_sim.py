#!/usr/bin/env python3
"""VX++ (GBA Video) bitstream parser.

The codec is a recursive block partition driven by 16 jump-table dispatchers, not
a flat macroblock loop; the per-mode grammar lives in vx_grammar.py, extracted
from the decoder's IWRAM image.

Validated against hardware: reproduces 4000/4000 ue(v) calls from an mGBA GDB-stub
trace, exact in both bit position and originating call site. See
doc/gba_video_vxpp.md section 11.

Coefficient loop detail (0x0300580c-0x03005818):
    add r12, r12, r5, lsl #2   ; cpos += run
    str r6, [r12], #4          ; coeffs[cpos] = value ; cpos += 1   <-- POST-INCREMENT
    tst r4, #1
    beq <loop start>           ; bit15==0 -> decode another coefficient
"""
import os
import sys

from vx_grammar import GRAMMAR, TOP, MB_PER_FRAME

ROM = os.environ.get("VXPP_ROM", "")  # path to the GBA Video cart dump
VLC = "/tmp/gbavx/vlc_rom_full4096.bin"
VOFS = "/tmp/gbavx/value_offset_table.bin"
ROFS = "/tmp/gbavx/run_offset_table.bin"
STREAM = os.environ.get("VXPP_STREAM", "/tmp/gbavx/stream00.video")

CBP_PERMTAB = [
    0x00,0x0f,0x1f,0x08,0x02,0x01,0x04,0x3f,0x0a,0x05,0x0e,0x0b,0x03,0x0c,0x10,0x0d,
    0x07,0x2f,0x06,0x09,0x20,0x1b,0x1e,0x17,0x1a,0x1d,0x15,0x11,0x13,0x12,0x18,0x14,
    0x1c,0x37,0x3b,0x3e,0x19,0x2b,0x21,0x27,0x16,0x2a,0x2e,0x25,0x22,0x3d,0x2d,0x28,
    0x24,0x35,0x23,0x3a,0x33,0x2c,0x29,0x30,0x26,0x31,0x3c,0x32,0x39,0x36,0x34,0x38,
]

# Default frame geometry: 15x7 macroblocks = 240x112, the letterboxed movie
# streams. The 240x160 streams use 150. Prefer deriving it from the container
# header (see decode_stream) rather than relying on this default. Frames are
# separated by a single marker bit, and each seek segment opens with its own
# quantiser delta.
# The ONLY true macroblock-mode reader. The other 20 ue(v) call sites are predictor
# helpers; treating any of them as a mode read is what produced the mislabelling
# described in doc/gba_video_vxpp.md section 10.
MODE_READ_SITE = 0x03001db4


def load(p):
    with open(p, "rb") as f:
        return f.read()


def load16(p):
    r = load(p)
    return [r[i] | (r[i + 1] << 8) for i in range(0, len(r), 2)]


class Bits:
    """Bit source matching FUN_0300076c/030007a8's real refill mechanism: the ARM
    code does `ldrh r10,[r1],#2` -- a little-endian HALFWORD load -- then treats the
    result MSB-first. That means each 2-byte pair is byte-SWAPPED relative to file
    order before the MSB-first bit interpretation applies (confirmed live: the first
    two stream bytes `69 04` load as halfword 0x0469, which decodes as ue(v)=34,
    matching a real mGBA capture -- naive raw-byte-order MSB-first reading of `69 04`
    gives 2, which is wrong). Precompute the swap once so peek32 stays a simple
    MSB-first byte reader.
    """
    def __init__(self, data):
        n = len(data) & ~1
        swapped = bytearray(len(data))
        for i in range(0, n, 2):
            swapped[i] = data[i + 1]
            swapped[i + 1] = data[i]
        if len(data) & 1:
            swapped[-1] = data[-1]
        self.d = bytes(swapped)

    def peek32(self, pos):
        acc = 0
        bi, bo = divmod(pos, 8)
        got = 0
        while got < 32:
            b = self.d[bi] if bi < len(self.d) else 0
            avail = 8 - bo
            take = min(avail, 32 - got)
            acc = (acc << take) | ((b >> (avail - take)) & ((1 << take) - 1))
            got += take
            bo += take
            if bo == 8:
                bo = 0
                bi += 1
        return acc & 0xFFFFFFFF


def sext(v, bits):
    m = 1 << (bits - 1)
    return (v ^ m) - m


def read_ue(br, pos):
    """FUN_0300076c: unsigned exp-golomb -> (value, new_pos)."""
    p = pos
    n = 0
    while True:
        if (br.peek32(p) >> 31) & 1:
            p += 1
            break
        p += 1
        n += 1
        if n > 31:
            return None, p
    if n == 0:
        return 0, p
    suffix = br.peek32(p) >> (32 - n)
    return suffix + (1 << n) - 1, p + n


def read_se(br, pos):
    """FUN_0300081c: signed exp-golomb. Same bit length as ue(v) (2n+1), then the
    standard H.264 zig-zag mapping 0, 1, -1, 2, -2, ... The ARM computes
    k = ue+1; if k is odd, k = 1-k; result = k >> 1 (arithmetic).
    This reader is why modes 4/5 appeared to consume untraced bits: it is a
    separate function from the ue(v) reader at FUN_0300076c, so it never showed
    up in a breakpoint trace of the latter.
    """
    u, p = read_ue(br, pos)
    if u is None:
        return None, p
    k = u + 1
    return ((1 - k) >> 1) if (k & 1) else (k >> 1), p


def decode_one(br, tab, vofs, rofs, pos):
    """One coefficient. Returns (value, run, last, new_pos, tag)."""
    acc = br.peek32(pos)
    if (acc >> 25) != 3:
        cell = tab[acc >> 20]
        ln = cell & 0xF
        val = (cell >> 4) & 0x1F
        run = (cell >> 9) & 0x3F
        last = (cell >> 15) & 1
        if ln and (acc >> (32 - ln)) & 1:
            val = -val
        return val, run, last, pos + ln, ("N", acc >> 20)

    if not ((acc >> 24) & 1):                    # variant 1: value offset
        p = pos + 8
        a = br.peek32(p)
        cell = tab[a >> 20]
        ln = cell & 0xF
        val = ((cell >> 4) & 0x1F) + vofs[cell >> 9]
        run = (cell >> 9) & 0x3F
        last = (cell >> 15) & 1
        if ln and (a >> (32 - ln)) & 1:
            val = -val
        return val, run, last, p + ln, ("V1", a >> 20)

    if not ((acc >> 23) & 1):                    # variant 2: run offset
        p = pos + 9
        a = br.peek32(p)
        cell = tab[a >> 20]
        ln = cell & 0xF
        val = (cell >> 4) & 0x1F
        last = (cell >> 15) & 1
        run = ((cell >> 9) & 0x3F) + rofs[last * 0x40 + val]
        if ln and (a >> (32 - ln)) & 1:
            val = -val
        return val, run, last, p + ln, ("V2", a >> 20)

    p = pos + 9                                  # variant 3: raw literal
    last = (br.peek32(p) >> 31) & 1
    p += 1
    run = br.peek32(p) >> 26
    p += 6
    val = sext(br.peek32(p) >> 20, 12)
    p += 12
    return val, run, last, p, ("V3", None)


def decode_block(br, tab, vofs, rofs, pos, stop_when_set=True, verbose=False):
    """Returns (coeffs, new_pos, overflow)."""
    # 8x8 blocks (the 6-bit CBP covers 4 luma quadrants + Cb + Cr), so 64 coeffs.
    co = [0] * 64
    cpos = 0
    over = False
    for _ in range(80):
        val, run, last, npos, tag = decode_one(br, tab, vofs, rofs, pos)
        cpos += run
        if cpos < 64:
            co[cpos] = val
        else:
            over = True
        cpos += 1                                # post-increment (str r6,[r12],#4)
        if verbose:
            print(f"      [{pos}:{npos}] {tag} run={run} last={last} val={val} slot={cpos-1}")
        pos = npos
        if last if stop_when_set else not last:
            return co, pos, over
    return co, pos, True


def decode_unit(br, tab, vofs, rofs, pos, disp=TOP, depth=0, stats=None):
    """Decode one node of the recursive block partition, starting at `disp`.

    The codec is not a flat macroblock loop: each of the 16 dispatchers reads a
    mode with ue(v) and may recurse into smaller dispatchers, so a top-level
    macroblock expands into a tree. Returns the new bit position.
    """
    if depth > 14:
        raise ValueError("partition nested too deep")
    mode, pos = read_ue(br, pos)
    table = GRAMMAR.get(disp)
    if table is None or mode is None or mode not in table:
        raise ValueError(f"mode {mode} not in table {disp:#x}")
    n_se, events = table[mode]
    if stats is not None:
        stats[(disp, mode)] = stats.get((disp, mode), 0) + 1
    for _ in range(n_se):
        _, pos = read_se(br, pos)
    for ev in events:
        kind = ev[0]
        if kind == "intra2":                 # FUN_03000884: luma + chroma submode
            _, pos = read_ue(br, pos)
            _, pos = read_ue(br, pos)
        elif kind == "ue1":                  # FUN_030008fc
            for _ in range(ev[1]):
                # H.264-style intra 4x4 mode: 1 bit when the predicted mode is
                # reused, otherwise 3 more bits selecting the remainder.
                pos += 1 if (br.peek32(pos) >> 31) & 1 else 4
            _, pos = read_ue(br, pos)
        elif kind == "disp":
            pos = decode_unit(br, tab, vofs, rofs, pos, ev[1], depth + 1, stats)
        elif kind == "resid":
            for _ in range(ev[1]):
                cbp, pos = read_ue(br, pos)
                if cbp is None or cbp >= 64:
                    raise ValueError(f"bad cbp {cbp}")
                mask = CBP_PERMTAB[cbp]
                for b in range(6):
                    if mask & (1 << b):
                        _, pos, _ = decode_block(br, tab, vofs, rofs, pos, True)
    return pos


def decode_segment(br, tab, vofs, rofs, start_bit, n_frames, mb_per_frame=None,
                   stats=None):
    """Decode one seek segment: a ue(v) header, then n_frames frames.

    Each seek point begins a self-contained segment whose first field is a
    quantiser delta -- the same read that opens the stream at bit 0. (An earlier
    pass called this a once-per-video field; it only looked like that because the
    hardware trace behind it covered ~30 frames, all inside segment 0.)
    """
    if mb_per_frame is None:
        mb_per_frame = MB_PER_FRAME
    pos = start_bit
    _, pos = read_ue(br, pos)                # per-segment quantiser delta
    for _ in range(n_frames):
        for _ in range(mb_per_frame):
            pos = decode_unit(br, tab, vofs, rofs, pos, TOP, 0, stats)
        pos += 1                             # inter-frame marker bit
    return pos - 1                           # last frame's marker is the segment end


def decode_stream(seek, nb_frames, width=240, height=112, path=None, quiet=True):
    """Walk every seek segment, checking each lands on the next seek offset.

    `seek` is the container's seek table: (frame, bit, audio_off, 0) entries, the
    last of which is a sentinel with bit 0. Macroblocks per frame comes from the
    header dimensions, so this works for both retail geometries (240x112 -> 105,
    240x160 -> 150). Returns (segments_ok, mismatches).
    """
    tab = load16(VLC)
    vofs, rofs = load(VOFS), load(ROFS)
    br = Bits(load(path or STREAM))
    mbs = (width // 16) * (height // 16)
    real = [e for e in seek if e[1] or e[0] == 0]
    ok, bad = 0, []
    for i, (f0, b0, _, _) in enumerate(real):
        n = (real[i + 1][0] if i + 1 < len(real) else nb_frames) - f0
        end = decode_segment(br, tab, vofs, rofs, b0, n, mbs)
        want = real[i + 1][1] if i + 1 < len(real) else None
        if want is None or end == want:
            ok += 1
        else:
            bad.append((f0, want, end))
        if not quiet:
            print(f"segment {i:3d}: frames {f0:6d}+{n:4d}  bit {b0:10d} -> {end:10d}")
    return ok, bad


def decode_frames(n_frames=None, quiet=True):
    """Walk whole frames. Returns (frames_done, end_bit, stats, stop_reason)."""
    tab = load16(VLC)
    vofs, rofs = load(VOFS), load(ROFS)
    br = Bits(load(STREAM))
    total = len(load(STREAM)) * 8
    _, pos = read_ue(br, 0)                  # one-time QP delta for the video
    stats = {}
    f = 0
    why = "end of stream"
    while (n_frames is None or f < n_frames) and pos < total - 256:
        start = pos
        try:
            for _ in range(MB_PER_FRAME):
                pos = decode_unit(br, tab, vofs, rofs, pos, TOP, 0, stats)
        except ValueError as e:
            # Known: the parse desyncs around byte 5198, past the end of the
            # hardware trace that validates it. See doc section 11, "Still open".
            why = f"desync in frame {f} at bit {pos}: {e}"
            break
        pos += 1                             # inter-frame marker bit
        if not quiet:
            print(f"frame {f:4d}: {pos - start} bits")
        f += 1
    return f, pos, stats, why


if __name__ == "__main__":
    frames, end, stats, why = decode_frames(quiet=False)
    print(f"\n{frames} frames parsed, ended at bit {end} ({why})")
    top = sorted((m, c) for (d, m), c in stats.items() if d == TOP)
    print("top-level mode histogram:", dict(top))
