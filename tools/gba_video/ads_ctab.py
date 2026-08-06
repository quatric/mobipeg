"""Decode the 64-entry audio context table that prefixes an ADS type-2 stream.

Builder: EWRAM 0x02004fe0..0x020050f8.  Palette + VLC live at 0x02008a04.
"""
PALETTE = [0xd8, 0x72, 0xe4, 0x4e, 0x78, 0xd2, 0xe1]      # main.bin 0x8a04
# code (top 4 bits) -> (palette index, bit length); codes >= 12 escape
VLC = [(0,2),(0,2),(0,2),(0,2),(1,3),(1,3),(2,3),(2,3),
       (3,4),(4,4),(5,4),(6,4)]
ESC = [0x0c, 0x0d, 0x08, 0x06]                            # main.bin 0x8a00

def decode_ctab(bits):
    """bits: object with peek(n)/skip(n) over an MSB-first bit stream."""
    tab = []
    for _ in range(64):
        c = bits.peek(4)
        if c < 12:
            idx, ln = VLC[c]
            bits.skip(ln)
            tab.append(PALETTE[idx])
            continue
        bits.skip(2)                       # the '11' escape prefix
        a = bits.peek(2); bits.skip(2)     # field 0
        b = bits.peek(2); bits.skip(2)     # field 1
        sel = bits.peek(1); bits.skip(1)
        mask = (1 << a) | (1 << b)         # 0x02005098: a one-hot pair
        lo, hi = ESC[mask & 3], ESC[mask >> 2]
        if sel:
            f2, f3 = hi >> 2, lo & 3       # 0x020050b2: swapped
        else:
            f2, f3 = lo & 3, hi >> 2       # 0x020050cc
        tab.append((a | (b << 2) | (f2 << 4) | (f3 << 6)) & 0xff)
    return tab
