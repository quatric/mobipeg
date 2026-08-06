"""Minimal GDB remote-serial-protocol client for mGBA's -g stub."""
import socket, time

class RSP:
    def __init__(self, host='127.0.0.1', port=2345, timeout=20):
        self.s = socket.create_connection((host, port), timeout=5)
        self.s.settimeout(timeout)
        self.buf = b''
    def _raw(self, n=1):
        while len(self.buf) < n:
            d = self.s.recv(4096)
            if not d: raise EOFError
            self.buf += d
        r, self.buf = self.buf[:n], self.buf[n:]
        return r
    def send(self, cmd):
        pkt = b'$' + cmd.encode() + b'#%02x' % (sum(cmd.encode()) & 0xff)
        self.s.sendall(pkt)
        while True:
            c = self._raw()
            if c == b'+': return
            if c == b'-': self.s.sendall(pkt)
            if c == b'$':      # unsolicited, push back
                self.buf = c + self.buf; return
    def recv(self):
        while True:
            c = self._raw()
            if c == b'$': break
        out = b''
        while True:
            c = self._raw()
            if c == b'#': break
            out += c
        self._raw(2)
        self.s.sendall(b'+')
        return out.decode(errors='replace')
    def cmd(self, c):
        self.send(c); return self.recv()
    def regs(self):
        h = self.cmd('g')
        return [int.from_bytes(bytes.fromhex(h[i*8:i*8+8]), 'little') for i in range(17)]
    def mem(self, addr, n):
        out = b''
        while n:
            k = min(n, 512)
            r = self.cmd('m%x,%x' % (addr, k))
            if r.startswith('E'): raise RuntimeError('mem read %s at %x' % (r, addr))
            out += bytes.fromhex(r); addr += k; n -= k
        return out
    def bp(self, addr, kind=4): return self.cmd('Z0,%x,%x' % (addr, kind))
    def rmbp(self, addr, kind=4): return self.cmd('z0,%x,%x' % (addr, kind))
    def cont(self):
        self.send('c'); return self.recv()
