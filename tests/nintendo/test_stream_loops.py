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
                                    coefs = channel_table + u32(channel_table + ci + 4)
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
