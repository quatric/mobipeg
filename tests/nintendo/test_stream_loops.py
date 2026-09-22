"""Check streamed Nintendo ADPCM loop contexts against decoded samples."""
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

FFMPEG = os.environ.get('FFMPEG', str(Path(__file__).resolve().parents[2] / 'ffmpeg'))


class StreamLoopTests(unittest.TestCase):
    def encode(self, directory, fmt, endian, start):
        path = Path(directory) / ('audio.' + fmt)
        result = subprocess.run([
            FFMPEG, '-v', 'error', '-y', '-f', 'lavfi', '-i',
            'aevalsrc=sin(731*2*PI*t)|cos(317*2*PI*t):s=32000', '-t', '0.02',
            '-c:a', 'adpcm_thp', '-block_size', '32', '-endian', endian,
            '-loop', '1', '-loop_start', str(start), str(path)], capture_output=True, timeout=15)
        return result, path

    def test_loop_histories(self):
        with tempfile.TemporaryDirectory() as directory:
            for fmt in ('brstm', 'bfstm', 'bcstm'):
                for endian in ('be', 'le'):
                    for start in (0, 1, 13, 14, 55, 56, 57, 639):
                        with self.subTest(fmt=fmt, endian=endian, start=start):
                            result, path = self.encode(directory, fmt, endian, start)
                            self.assertEqual(result.returncode, 0, result.stderr.decode())
                            data = path.read_bytes()
                            order = '>' if endian == 'be' else '<'
                            u32 = lambda off: struct.unpack_from(order + 'I', data, off)[0]
                            body = (u32(0x10) if fmt == 'brstm' else u32(0x18)) + 8
                            channel_table = body + u32(body + 20)
                            stream = body + u32(body + 4)
                            audio = u32(stream + 16) if fmt == 'brstm' else u32(0x30) + 0x20
                            blocks = u32(stream + (20 if fmt == 'brstm' else 16))
                            last_span = u32(stream + (40 if fmt == 'brstm' else 36))
                            frame_byte = start // 14 * 8
                            block, within = divmod(frame_byte, 32)
                            span = last_span if block == blocks - 1 else 32
                            for ch in range(2):
                                ci = u32(channel_table + 8 + ch * 8)
                                if fmt == 'brstm':
                                    coefs = body + u32(body + ci + 4)
                                    state = coefs + 0x28
                                else:
                                    coefs = channel_table + ci + u32(channel_table + ci + 4)
                                    state = coefs + 0x26
                                decoded = subprocess.run([FFMPEG, '-v', 'error', '-i', str(path),
                                                          '-f', 's16le', '-'], capture_output=True, timeout=15)
                                self.assertEqual(decoded.returncode, 0, decoded.stderr.decode())
                                samples = struct.unpack('<%dh' % (len(decoded.stdout) // 2), decoded.stdout)
                                ps, h1, h2 = struct.unpack_from(order + 'Hhh', data, state)
                                self.assertEqual(ps, data[audio + block * 64 + ch * span + within])
                                self.assertEqual(h1, samples[(start - 1) * 2 + ch] if start else 0)
                                self.assertEqual(h2, samples[(start - 2) * 2 + ch] if start > 1 else 0)

    def test_invalid_loop_points(self):
        with tempfile.TemporaryDirectory() as directory:
            for fmt in ('brstm', 'bfstm', 'bcstm'):
                for start in (640, 2**63 - 1):
                    with self.subTest(fmt=fmt, start=start):
                        result, _ = self.encode(directory, fmt, 'be', start)
                        self.assertGreater(result.returncode, 0)

    def test_stream_references_and_seek_description(self):
        # Follow NintendoWare references as external readers do, rather than
        # assuming the contiguous coefficient layout used by our demuxer.
        with tempfile.TemporaryDirectory() as directory:
            for fmt in ('bfstm', 'bcstm'):
                for endian in ('be', 'le'):
                    with self.subTest(fmt=fmt, endian=endian):
                        result, path = self.encode(directory, fmt, endian, 29)
                        self.assertEqual(result.returncode, 0, result.stderr.decode())
                        data = path.read_bytes()
                        order = '>' if endian == 'be' else '<'
                        u32 = lambda off: struct.unpack_from(order + 'I', data, off)[0]
                        ref = lambda off: struct.unpack_from(order + 'HHI', data, off)
                        body = u32(0x18) + 8
                        kind, pad, offset = ref(body)
                        self.assertEqual((kind, pad), (0x4100, 0))
                        stream = body + offset
                        self.assertEqual((u32(stream + 40), u32(stream + 44)), (4, 56))
                        self.assertEqual(ref(stream + 48), (0x1f00, 0, 24))
                        kind, pad, offset = ref(body + 16)
                        self.assertEqual((kind, pad), (0x0101, 0))
                        table = body + offset
                        decoded = subprocess.run([FFMPEG, '-v', 'error', '-i', str(path),
                                                  '-f', 's16le', '-'], capture_output=True, timeout=15)
                        self.assertEqual(decoded.returncode, 0, decoded.stderr.decode())
                        pcm = struct.unpack('<%dh' % (len(decoded.stdout) // 2), decoded.stdout)
                        audio = u32(0x30) + 0x20
                        for ch in range(2):
                            kind, pad, offset = ref(table + 4 + 8 * ch)
                            self.assertEqual((kind, pad), (0x4102, 0))
                            ci = table + offset
                            kind, pad, offset = ref(ci)
                            self.assertEqual((kind, pad), (0x0300, 0))
                            coefs = struct.unpack_from(order + '16h', data, ci + offset)
                            h1 = h2 = 0
                            samples = []
                            for frame in range(4):
                                off = audio + ch * 32 + frame * 8
                                header = data[off]
                                a, b = coefs[(header >> 4) * 2:(header >> 4) * 2 + 2]
                                for n in range(14):
                                    byte = data[off + 1 + n // 2]
                                    nib = (byte & 15) if n % 2 else byte >> 4
                                    if nib >= 8:
                                        nib -= 16
                                    value = ((h1 * a + h2 * b) >> 11) + nib * (1 << (header & 15))
                                    h2, h1 = h1, max(-32768, min(32767, value))
                                    samples.append(h1)
                            self.assertEqual(samples, list(pcm[ch:112:2]))

    def test_reader_follows_coefficient_references(self):
        with tempfile.TemporaryDirectory() as directory:
            for fmt in ('brstm', 'bfstm', 'bcstm'):
                for endian in ('be', 'le'):
                    with self.subTest(fmt=fmt, endian=endian):
                        result, path = self.encode(directory, fmt, endian, 29)
                        self.assertEqual(result.returncode, 0, result.stderr.decode())
                        command = [FFMPEG, '-v', 'error', '-xerror', '-i', str(path), '-f', 's16le', '-']
                        expected = subprocess.run(command, capture_output=True, timeout=15)
                        self.assertEqual(expected.returncode, 0)
                        original = path.read_bytes()
                        data = bytearray(original)
                        order = '>' if endian == 'be' else '<'
                        u32 = lambda off: struct.unpack_from(order + 'I', data, off)[0]
                        body = u32(0x10 if fmt == 'brstm' else 0x18) + 8
                        table = body + u32(body + 20)
                        base = body if fmt == 'brstm' else table
                        cis = [base + u32(table + 8 + 8 * ch) for ch in range(2)]
                        coefs = [(body if fmt == 'brstm' else ci) + u32(ci + 4) for ci in cis]
                        # Relocate contexts without changing channel identity.
                        context_size = 48 if fmt == 'brstm' else 46
                        for ch in range(2):
                            target = coefs[1 - ch]
                            data[target:target + context_size] = original[coefs[ch]:coefs[ch] + context_size]
                            struct.pack_into(order + 'I', data, cis[ch] + 4, target - (body if fmt == 'brstm' else cis[ch]))
                        path.write_bytes(data)
                        actual = subprocess.run(command, capture_output=True, timeout=15)
                        self.assertEqual(actual.returncode, 0, actual.stderr.decode())
                        self.assertEqual(actual.stdout, expected.stdout)
                        piped = subprocess.run([FFMPEG, '-v', 'error', '-xerror', '-i', 'pipe:0',
                                                '-f', 's16le', '-'], input=bytes(data),
                                               capture_output=True, timeout=15)
                        self.assertEqual(piped.returncode, 0, piped.stderr.decode())
                        self.assertEqual(piped.stdout, expected.stdout)

    def test_reader_rejects_invalid_coefficient_references(self):
        with tempfile.TemporaryDirectory() as directory:
            for fmt in ('brstm', 'bfstm', 'bcstm'):
                for endian in ('be', 'le'):
                    result, path = self.encode(directory, fmt, endian, 29)
                    self.assertEqual(result.returncode, 0, result.stderr.decode())
                    original = path.read_bytes()
                    order = '>' if endian == 'be' else '<'
                    u32 = lambda off: struct.unpack_from(order + 'I', original, off)[0]
                    body = u32(0x10 if fmt == 'brstm' else 0x18) + 8
                    table = body + u32(body + 20)
                    ci = (body if fmt == 'brstm' else table) + u32(table + 8)
                    for field in (table + 8, ci + 4):
                        with self.subTest(fmt=fmt, endian=endian, field=field):
                            data = bytearray(original)
                            struct.pack_into(order + 'I', data, field, 0xfffffff0)
                            path.write_bytes(data)
                            result = subprocess.run([FFMPEG, '-v', 'error', '-xerror', '-i', str(path),
                                                     '-f', 'null', '-'], capture_output=True, timeout=15)
                            self.assertGreater(result.returncode, 0)

    def test_reader_rejects_zero_block_geometry(self):
        with tempfile.TemporaryDirectory() as directory:
            for fmt in ('brstm', 'bfstm', 'bcstm'):
                for endian in ('be', 'le'):
                    result, path = self.encode(directory, fmt, endian, 29)
                    self.assertEqual(result.returncode, 0, result.stderr.decode())
                    original = path.read_bytes()
                    order = '>' if endian == 'be' else '<'
                    u32 = lambda off: struct.unpack_from(order + 'I', original, off)[0]
                    body = u32(0x10 if fmt == 'brstm' else 0x18) + 8
                    stream = body + u32(body + 4)
                    geometry = stream + (20 if fmt == 'brstm' else 16)
                    for field in range(3):
                        with self.subTest(fmt=fmt, endian=endian, field=field):
                            data = bytearray(original)
                            struct.pack_into(order + 'I', data, geometry + 4 * field, 0)
                            path.write_bytes(data)
                            result = subprocess.run([FFMPEG, '-v', 'error', '-xerror', '-ss', '0.01',
                                                     '-i', str(path), '-f', 'null', '-'],
                                                    capture_output=True, timeout=15)
                            self.assertGreater(result.returncode, 0, result.stderr.decode())

    def test_pcm_roundtrip(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'source.raw'
            for channels in (1, 2):
                raw = b''.join(struct.pack('<h', ((i * 317 + ch * 19001) % 60000) - 30000)
                               for i in range(79) for ch in range(channels))
                source.write_bytes(raw)
                for fmt in ('brstm', 'bfstm', 'bcstm'):
                    for endian in ('be', 'le'):
                        for input_endian in ('be', 'le'):
                            with self.subTest(fmt=fmt, endian=endian, input_endian=input_endian, channels=channels):
                                path = Path(directory) / ('audio.' + fmt)
                                result = subprocess.run([FFMPEG, '-v', 'error', '-y', '-f', 's16le',
                                                         '-ar', '32000', '-ac', str(channels), '-i', str(source),
                                                         '-c:a', 'pcm_s16' + input_endian + '_planar',
                                                         '-endian', endian, '-block_size', '32', str(path)],
                                                        capture_output=True, timeout=15)
                                self.assertEqual(result.returncode, 0, result.stderr.decode())
                                result = subprocess.run([FFMPEG, '-v', 'error', '-xerror', '-i', str(path),
                                                         '-f', 's16le', '-'], capture_output=True, timeout=15)
                                self.assertEqual(result.returncode, 0, result.stderr.decode())
                                self.assertEqual(result.stdout, raw)
