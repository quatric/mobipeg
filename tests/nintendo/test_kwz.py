# Copyright (c) 2026 quatric - quatricsoftware@gmail.com
"""Flipnote Studio 3D (.kwz) muxer tests.

Run with python3 -m unittest discover -s tests/nintendo after building FFmpeg.
"""
from pathlib import Path
import hashlib
import math
import os
import random
import struct
import subprocess
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[2]
FFMPEG = os.environ.get('FFMPEG', str(ROOT / 'ffmpeg'))

W, H = 320, 240
# white, black, red, yellow, green, blue
PALETTE = [(0xff, 0xff, 0xff), (0x10, 0x10, 0x10), (0xff, 0x10, 0x10),
           (0xff, 0xe7, 0x00), (0x00, 0x86, 0x31), (0x00, 0x38, 0xce)]
STEPS = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
         50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
         253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
         1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
         3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
         11487, 12635, 13899]
SHA256_PREFIX = bytes.fromhex('3031300d060960864801650304020105000420')
BASE32 = 'cwmfjordvegbalksnthpyxquiz012345'


def frame(shift, colours=PALETTE):
    """Vertical bars of the palette colours, moved right by `shift` pixels."""
    row = bytearray()
    for x in range(W):
        row += bytes(colours[((x - shift) // 20) % len(colours)])
    return bytes(row) * H


def sections(data):
    out, pos = {}, 0
    while pos < len(data) - 256:
        size = struct.unpack_from('<I', data, pos + 4)[0]
        out[data[pos:pos + 3]] = (data[pos + 3], data[pos + 8:pos + 8 + size])
        pos += 8 + size
    return out, pos


def decode_adpcm(track):
    pred, index, out = 0, 40, []
    for byte in track:
        bit = 0
        while bit < 8:
            step = STEPS[index]
            diff = step >> 3
            if index < 18 or bit > 4:
                code = byte & 3
                if code & 1:
                    diff += step
                if code & 2:
                    diff = -diff
                index += (-1, 2, -1, 2)[code]
                byte >>= 2
                bit += 2
            else:
                code = byte & 15
                if code & 1:
                    diff += step >> 2
                if code & 2:
                    diff += step >> 1
                if code & 4:
                    diff += step
                if code & 8:
                    diff = -diff
                index += (-1, -1, -1, -1, 2, 4, 6, 8)[code & 7]
                byte >>= 4
                bit += 4
            index = max(0, min(79, index))
            pred = max(-2048, min(2047, pred + diff))
            out.append(pred * 16)
    return out


def is_prime(n, rng):
    if n % 2 == 0:
        return False
    for p in (3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37):
        if n % p == 0:
            return n == p
    d, r = n - 1, 0
    while d % 2 == 0:
        d //= 2
        r += 1
    for _ in range(12):
        x = pow(rng.randrange(2, n - 1), d, n)
        if x in (1, n - 1):
            continue
        for _ in range(r - 1):
            x = x * x % n
            if x == n - 1:
                break
        else:
            return False
    return True


def make_test_key():
    """A throwaway 2048-bit RSA key, generated from a fixed seed."""
    rng = random.Random(0x4b575a)
    primes = []
    while len(primes) < 2:
        p = rng.getrandbits(1024) | (3 << 1022) | 1
        if p % 65537 != 1 and is_prime(p, rng):
            primes.append(p)
    p, q = primes
    n, e = p * q, 65537
    d = pow(e, -1, (p - 1) * (q - 1))

    def der_int(v):
        raw = v.to_bytes(v.bit_length() // 8 + 1, 'big')
        return b'\x02' + der_len(len(raw)) + raw

    def der_len(size):
        if size < 128:
            return bytes([size])
        raw = size.to_bytes((size.bit_length() + 7) // 8, 'big')
        return bytes([0x80 | len(raw)]) + raw

    body = b''.join(der_int(v) for v in
                    (0, n, e, d, p, q, d % (p - 1), d % (q - 1), pow(q, -1, p)))
    return n, e, b'\x30' + der_len(len(body)) + body


class KwzMuxerTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)

    def mux(self, frames, *options, audio=None, rate=12):
        video = self.root / 'in.rgb'
        video.write_bytes(b''.join(frames))
        out = self.root / 'out.kwz'
        cmd = [FFMPEG, '-v', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'rgb24',
               '-s', '%dx%d' % (W, H), '-r', str(rate), '-i', str(video)]
        if audio is not None:
            pcm = self.root / 'in.s16'
            pcm.write_bytes(audio)
            cmd += ['-f', 's16le', '-ar', '16364', '-ac', '1', '-i', str(pcm),
                    '-c:a', 'pcm_s16le']
        cmd += ['-c:v', 'rawvideo', '-fflags', '+bitexact', *options, '-f', 'kwz', str(out)]
        result = subprocess.run(cmd, capture_output=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
        return out.read_bytes()

    def decode(self, data):
        path = self.root / 'dec.kwz'
        path.write_bytes(data)
        result = subprocess.run([FFMPEG, '-v', 'error', '-i', str(path), '-an',
                                 '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'],
                                capture_output=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
        return result.stdout

    def test_palette_frames_round_trip(self):
        frames = [frame(0), frame(0), frame(8), frame(13)]
        data = self.mux(frames)
        self.assertEqual(self.decode(data), b''.join(frames))
        self.assertEqual(self.mux(frames), data, 'bitexact output is not reproducible')

    def test_paper_colours_round_trip(self):
        frames = [frame(0), frame(5)]
        for index, paper in enumerate(('white', 'black', 'red', 'yellow', 'green', 'blue')):
            with self.subTest(paper=paper):
                data = self.mux(frames, '-kwz_paper', paper)
                self.assertEqual(self.decode(data), b''.join(frames))
                meta = sections(data)[0][b'KMI'][1]
                self.assertEqual(meta[0] & 15, index or 6)

    def test_container_layout(self):
        frames = [frame(0), frame(0), frame(8)]
        data = self.mux(frames, '-kwz_author', 'tester', '-kwz_loop', '1', rate=8)
        secs, end = sections(data)
        self.assertEqual(end, len(data) - 256)
        self.assertEqual(data[-256:], bytes(256), 'no key was given, so no signature')
        self.assertEqual(list(secs), [b'KFH', b'KTN', b'KMC', b'KMI', b'KSN'])
        self.assertEqual([secs[k][0] for k in secs], [0x14, 0x02, 0x02, 0x05, 0x01])
        for name, (_, body) in secs.items():
            self.assertEqual(len(body) % 4, 0, name)
        for name in (b'KFH', b'KTN', b'KMC'):
            body = secs[name][1]
            self.assertEqual(struct.unpack_from('<I', body)[0], zlib.crc32(body[4:]), name)

        kfh = secs[b'KFH'][1]
        self.assertEqual(len(kfh), 204)
        self.assertEqual(kfh[0x2e:0x2e + 22].decode('utf-16le').rstrip('\0'), 'tester')
        self.assertEqual(struct.unpack_from('<HHH', kfh, 0xc4), (3, 0, 2))
        self.assertEqual(kfh[0xca], 6, '8 fps is speed 6')
        fsid = kfh[0x10:0x1a]
        self.assertEqual(fsid, b'\0' + hashlib.sha256(b'tester').digest()[:8] + b'\0')
        value = 0
        for ch in kfh[0x70:0x8c].decode('ascii'):
            value = value << 5 | BASE32.index(ch)
        self.assertEqual((value >> 4).to_bytes(17, 'big'), fsid[:9] + bytes(8))

        jpeg = secs[b'KTN'][1][4:]
        self.assertEqual(jpeg[:2], b'\xff\xd8')
        self.assertEqual(jpeg[2:21], bytes.fromhex('ffc0001108004000500301210002110103'
                                                   '1101'))
        self.assertIn(b'\xff\xd9', jpeg[-6:])

        kmi, kmc = secs[b'KMI'][1], secs[b'KMC'][1]
        self.assertEqual(len(kmi), 3 * 28)
        flags = [struct.unpack_from('<I', kmi, 28 * i)[0] for i in range(3)]
        self.assertEqual(flags, [0x43251076, 0x43251006, 0x43251006])
        sizes = [struct.unpack_from('<3H', kmi, 28 * i + 4) for i in range(3)]
        self.assertEqual(sizes[1], (38, 38, 38), 'an unchanged frame is all skips')
        total = sum(sum(s) for s in sizes)
        self.assertEqual(len(kmc), 4 + total + (-total) % 4)
        self.assertEqual(kmc[4 + sum(sizes[0]):][:38], bytes([0xfd] * 37 + [0x7d]))
        for i in range(3):
            self.assertEqual(kmi[28 * i + 10:28 * i + 20], fsid)

        ksn = secs[b'KSN'][1]
        self.assertEqual(struct.unpack_from('<7I', ksn), (6, 0, 0, 0, 0, 0, 0))

    def test_dither_reduces_to_the_palette(self):
        grey = bytes([128, 128, 128]) * (W * H)
        for dither, colours in (('none', 1), ('2x2', 2), ('4x4', 2), ('8x8', 2)):
            with self.subTest(dither=dither):
                out = self.decode(self.mux([grey], '-kwz_dither', dither))
                used = {out[i:i + 3] for i in range(0, len(out), 3)}
                self.assertEqual(len(used), colours)
                self.assertTrue(used <= {bytes(PALETTE[0]), bytes(PALETTE[1])},
                                'a grey must stay neutral')

    def test_bgm_is_stored_as_adpcm(self):
        count = 16364 // 2
        pcm = [int(12000 * math.sin(2 * math.pi * 440 * i / 16364)) for i in range(count)]
        data = self.mux([frame(0)] * 6, audio=struct.pack('<%dh' % count, *pcm))
        ksn = sections(data)[0][b'KSN'][1]
        speed, size = struct.unpack_from('<II', ksn)
        self.assertEqual(speed, 7)
        self.assertEqual(struct.unpack_from('<4I', ksn, 8), (0, 0, 0, 0))
        self.assertEqual(struct.unpack_from('<I', ksn, 24)[0], zlib.crc32(ksn[28:]))
        out = decode_adpcm(ksn[28:28 + size])
        self.assertGreaterEqual(len(out), count)
        self.assertLess(len(out) - count, 4)
        noise = sum((a - b) ** 2 for a, b in zip(pcm, out))
        signal = sum(a * a for a in pcm)
        self.assertGreater(10 * math.log10(signal / noise), 30)

    def test_signature_verifies(self):
        n, e, der = make_test_key()
        key = self.root / 'key.der'
        key.write_bytes(der)
        pem = self.root / 'key.pem'
        import base64
        pem.write_text('-----BEGIN RSA PRIVATE KEY-----\n'
                       + base64.encodebytes(der).decode() + '-----END RSA PRIVATE KEY-----\n')
        for path in (key, pem):
            with self.subTest(key=path.name):
                data = self.mux([frame(0), frame(3)], '-kwz_key', str(path))
                signature = int.from_bytes(data[-256:], 'big')
                digest = hashlib.sha256(data[:-256]).digest()
                expected = b'\x00\x01' + b'\xff' * (256 - 3 - 19 - 32) + b'\x00' \
                           + SHA256_PREFIX + digest
                self.assertEqual(pow(signature, e, n).to_bytes(256, 'big'), expected)

    def test_bad_inputs_are_rejected(self):
        video = self.root / 'small.rgb'
        video.write_bytes(bytes(160 * 120 * 3))
        out = self.root / 'bad.kwz'
        result = subprocess.run([FFMPEG, '-v', 'error', '-y', '-f', 'rawvideo', '-pix_fmt',
                                 'rgb24', '-s', '160x120', '-r', '12', '-i', str(video),
                                 '-c:v', 'rawvideo', '-f', 'kwz', str(out)],
                                capture_output=True, timeout=60)
        self.assertNotEqual(result.returncode, 0)
        junk = self.root / 'junk.der'
        junk.write_bytes(b'not a key')
        video.write_bytes(frame(0))
        result = subprocess.run([FFMPEG, '-v', 'error', '-y', '-f', 'rawvideo', '-pix_fmt',
                                 'rgb24', '-s', '320x240', '-r', '12', '-i', str(video),
                                 '-c:v', 'rawvideo', '-kwz_key', str(junk), '-f', 'kwz',
                                 str(out)], capture_output=True, timeout=60)
        self.assertNotEqual(result.returncode, 0)


if __name__ == '__main__':
    unittest.main()
