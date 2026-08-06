"""LZMA decoder for ADS-era GBA Video (Majesco).

A port of the IWRAM routine at 0x03001cd4 in Dragon Ball GT's main.bin.
It is stock LZMA with lc=0, lp=0, pb=2 and no end marker; streams carry an
8-byte prefix of [u32 uncompressed_size][u32 params] instead of the usual
LZMA properties header.

The probability-model offsets below are transcribed from the ROM (byte
offsets halved to prob units) and match the canonical LZMA layout exactly.
"""

IsMatch     = 0x000 // 2
IsRep       = 0x180 // 2
IsRepG0     = 0x198 // 2
IsRepG1     = 0x1b0 // 2
IsRepG2     = 0x1c8 // 2
IsRep0Long  = 0x1e0 // 2
PosSlot     = 0x360 // 2
SpecPos     = 0x560 // 2
Align       = 0x644 // 2
LenCoder    = 0x664 // 2
RepLenCoder = 0xa68 // 2
Literal     = 0xe6c // 2
NPROBS      = Literal + 0x300

_M = 0xFFFFFFFF


class _RC:
    """LZMA binary range decoder."""

    def __init__(self, d, p):
        self.d = d
        self.p = p + 1                     # first byte is discarded
        self.code = 0
        for _ in range(4):
            self.code = (self.code << 8) | self._nb()
        self.range = _M

    def _nb(self):
        b = self.d[self.p] if self.p < len(self.d) else 0
        self.p += 1
        return b

    def _norm(self):
        if self.range < (1 << 24):
            self.range = (self.range << 8) & _M
            self.code = ((self.code << 8) | self._nb()) & _M

    def bit(self, pr, i):
        bound = (self.range >> 11) * pr[i]
        if self.code < bound:
            self.range = bound
            pr[i] += (2048 - pr[i]) >> 5
            r = 0
        else:
            self.range -= bound
            self.code -= bound
            pr[i] -= pr[i] >> 5
            r = 1
        self._norm()
        return r

    def direct(self, n):
        r = 0
        for _ in range(n):
            self.range >>= 1
            r <<= 1
            if self.code >= self.range:
                self.code -= self.range
                r |= 1
            self._norm()
        return r

    def tree(self, pr, base, nbits):
        m = 1
        for _ in range(nbits):
            m = (m << 1) | self.bit(pr, base + m)
        return m - (1 << nbits)

    def rtree(self, pr, base, nbits):
        m, r = 1, 0
        for i in range(nbits):
            b = self.bit(pr, base + m)
            m = (m << 1) | b
            r |= b << i
        return r


def _declen(rc, pr, base, posState):
    """choice -> choice2 -> low(8) / mid(8) / high(256)."""
    if rc.bit(pr, base) == 0:
        return rc.tree(pr, base + 2 + (posState << 3), 3)
    if rc.bit(pr, base + 1) == 0:
        return 8 + rc.tree(pr, base + 2 + 128 + (posState << 3), 3)
    return 16 + rc.tree(pr, base + 2 + 256, 8)


def decode_raw(data, off, outsize, lc=0, lp=0, pb=2):
    """Decode `outsize` bytes of raw LZMA starting at data[off].

    Returns (output, bytes_consumed).
    """
    pr = [1024] * NPROBS
    rc = _RC(data, off)
    out = bytearray()
    state = 0
    r0 = r1 = r2 = r3 = 0
    pbm = (1 << pb) - 1
    lpm = (1 << lp) - 1

    while len(out) < outsize:
        pos = len(out)
        ps = pos & pbm
        if rc.bit(pr, IsMatch + (state << 4) + ps) == 0:
            prev = out[-1] if out else 0
            lit = Literal + 0x300 * ((((pos & lpm) << lc) + (prev >> (8 - lc)))
                                     if (lc or lp) else 0)
            if state < 7:
                sym = 1
                while sym < 0x100:
                    sym = (sym << 1) | rc.bit(pr, lit + sym)
            else:
                mb = out[len(out) - r0 - 1]
                sym = 1
                while sym < 0x100:
                    mbit = (mb >> 7) & 1
                    mb = (mb << 1) & 0xFF
                    b = rc.bit(pr, lit + 0x100 + (mbit << 8) + sym)
                    sym = (sym << 1) | b
                    if mbit != b:
                        while sym < 0x100:
                            sym = (sym << 1) | rc.bit(pr, lit + sym)
                        break
            out.append(sym & 0xFF)
            state = 0 if state < 4 else (state - 3 if state < 10 else state - 6)
            continue

        if rc.bit(pr, IsRep + state):
            if rc.bit(pr, IsRepG0 + state) == 0:
                if rc.bit(pr, IsRep0Long + (state << 4) + ps) == 0:
                    state = 9 if state < 7 else 11
                    out.append(out[len(out) - r0 - 1])
                    continue
            else:
                if rc.bit(pr, IsRepG1 + state) == 0:
                    dist = r1
                else:
                    if rc.bit(pr, IsRepG2 + state) == 0:
                        dist = r2
                    else:
                        dist = r3
                        r3 = r2
                    r2 = r1
                r1 = r0
                r0 = dist
            ln = _declen(rc, pr, RepLenCoder, ps) + 2
            state = 8 if state < 7 else 11
        else:
            r3, r2, r1 = r2, r1, r0
            ln = _declen(rc, pr, LenCoder, ps)
            state = 7 if state < 7 else 10
            lenState = ln if ln < 4 else 3
            posSlot = rc.tree(pr, PosSlot + (lenState << 6), 6)
            if posSlot < 4:
                r0 = posSlot
            else:
                ndb = (posSlot >> 1) - 1
                r0 = (2 | (posSlot & 1)) << ndb
                if posSlot < 14:
                    r0 += rc.rtree(pr, SpecPos + r0 - posSlot - 1, ndb)
                else:
                    r0 += rc.direct(ndb - 4) << 4
                    r0 += rc.rtree(pr, Align, 4)
            if r0 == _M:
                break
            ln += 2

        for _ in range(ln):
            out.append(out[len(out) - r0 - 1])

    return bytes(out), rc.p - off


def decode_blob(data, off):
    """Decode a stream carrying the 8-byte [u32 size][u32 params] prefix."""
    import struct
    outsize, params = struct.unpack_from('<II', data, off)
    out, used = decode_raw(data, off + 8, outsize)
    return out, params, used + 8
