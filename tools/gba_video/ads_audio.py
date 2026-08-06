"""ADS GBA Video type-2 audio decoder, transcribed from IWRAM 0x03002b4c."""
import struct
M32 = 0xFFFFFFFF
VLC = [0x01,0x01,0x01,0x01,0x12,0x12,0x23,0x33]   # IWRAM 0x03002db8

def _s32(v):
    v &= M32
    return v - (1 << 32) if v & 0x80000000 else v

def asr(v, n):  return _s32(v) >> n
def lsr(v, n):  return (v & M32) >> n

class Ctx:
    def __init__(self, ctx_table, data, pos=0):
        self.step=0; self.prev=0; self.h0=0; self.h1=0
        self.f0=0; self.f1=0; self.f2=0
        self.acc=0; self.nbits=0; self.pend=0; self.pendn=0
        self.data=data; self.p=pos
        self.ctab=list(ctx_table); self.ctx=0
        self.s0=0; self.s1=0

    def _word(self):
        if self.p+4 <= len(self.data):
            w = struct.unpack_from('<I', self.data, self.p)[0]
        else:
            w = 0
        self.p += 4
        return w

    def _refill(self):
        # ARM shifts by 32 yield 0 (register-specified shift amount)
        def shl(v, n): return 0 if n >= 32 else (v << n) & M32
        def shr(v, n): return 0 if n >= 32 else (v & M32) >> n
        if self.pendn:
            tot = self.pendn + self.nbits
            newpend = shl(self.pend, 32 - self.nbits)
            self.acc = (self.acc | shr(self.pend, self.nbits)) & M32
            if tot > 32:
                self.pend = newpend
                self.pendn = tot - 32
                self.nbits = 32
                return
            self.nbits = tot
            self.pendn = 0
        # falls through: pull a fresh word
        w = self._word()
        n = self.nbits
        self.acc = (self.acc | shr(w, n)) & M32
        self.pend = shl(w, 32 - n)
        self.pendn = n
        self.nbits = 32

    def decode(self, n):
        out = bytearray()
        r5, r6, r7 = self.f0, self.f1, self.f2
        r3 = self.prev
        for _ in range(n):
            while self.nbits <= 2:
                self._refill()
            sl = self.acc
            sb = VLC[lsr(sl, 29)]
            ln = sb & 0xf
            self.acc = (sl << ln) & M32
            self.nbits -= ln
            cx = self.ctx
            sh = sb >> 3
            sym = (asr(self.ctab[cx], sh)) & 3
            step = self.step
            self.ctx = (sym | ((cx << 2) & 0x3f))
            fp = self.ctx
            r4 = asr(step, 8) + 0x74
            if fp & 1: r4 += 0xf9
            mag = ((r4 & 0x7f) | 0x80) << 7
            mag = asr(mag, 0xe - ((asr(r4,7)) & 0xf))
            if fp & 2: mag = -mag
            lr = asr(step, 6)
            if fp & 1:
                t = 0x36c0 - lr + 0x20
                nf = lr + asr(t, 5)
                if lr > 0x12e0: nf = 0x1400
            else:
                t = -(lr + 0x2c0)
                nf = lr + asr(t, 5)
                if lr < 0x248: nf = 0x220
            self.step = step + nf + asr(-step, 6)
            lr2 = _s32(r3 * r7)
            sb2 = self.h1; ip = self.h0
            slv = _s32(sb2 * asr(r6,2))
            sb3 = _s32(ip * asr(r5,2))
            acc = asr(slv,11) + asr(sb3,11) + asr(lr2,11)
            newh = mag + asr(acc,1)
            self.h1 = ip
            if newh == 0: newh = 1
            self.h0 = newh
            v = asr(newh + lsr(asr(newh,31), 26), 6)
            v = 127 if v > 127 else (-128 if v < -128 else v)
            out.append(v & 0xff)
            sb4 = mag + asr(lr2, 12)
            r4b = r5 - asr(r5, 8)
            r6 = r6 - asr(r6, 7)
            r5 = r4b
            sgn = lsr(sb4, 31)
            old = self.s0
            if sb4 != 0:
                if sgn == old: r5 = r4b + 0xc0
                else:          r5 = r4b - 0xc0
                r4c = -r4b if sgn == old else r4b
                if ((r4c + 0x1fffffff) & 0xffffffff) <= 0x3fffffff:
                    r6 += asr(r4c, 5)
                else:
                    r6 += -0x100 if r4c < 0 else 0xff
                r6 += 0x80 if sgn == self.s1 else -0x80
            lim = 0x3c00 - r6
            if r5 > lim: r5 = lim
            r7 = r7 - asr(r7, 8)
            r7 += 0x20 if (_s32(mag) ^ _s32(r3)) >= 0 else -0x20
            self.s1 = old; self.s0 = sgn
            r3 = mag
        self.prev, self.f0, self.f1, self.f2 = r3, r5, r6, r7
        return out
