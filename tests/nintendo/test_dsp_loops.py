"""Check cached loop histories against uninterrupted decoding."""
from pathlib import Path
import os
import struct
import subprocess
import tempfile
import unittest

FFMPEG = os.environ.get('FFMPEG', str(Path(__file__).resolve().parents[2] / 'ffmpeg'))


class DSPLoopTests(unittest.TestCase):
    def encode(self, directory, fmt, start, end=None):
        path = Path(directory) / ('loop.' + fmt)
        args = [FFMPEG, '-v', 'error', '-y', '-f', 'lavfi', '-i',
                'sine=frequency=731:sample_rate=32000', '-t', '0.02',
                '-c:a', 'adpcm_thp', '-loop', '1', '-loop_start', str(start)]
        if fmt == 'bns':
            args += ['-compress', '0']
        if end is not None:
            args += ['-loop_end', str(end)]
        result = subprocess.run(args + [str(path)], capture_output=True, timeout=15)
        return result, path

    def test_loop_histories_match_decoded_samples(self):
        with tempfile.TemporaryDirectory() as directory:
            for fmt in ('dsp', 'bns'):
                for start in (0, 1, 7, 13, 14, 15, 29, 639):
                    with self.subTest(fmt=fmt, start=start):
                        result, path = self.encode(directory, fmt, start)
                        self.assertEqual(result.returncode, 0, result.stderr.decode())
                        data = path.read_bytes()
                        if fmt == 'dsp':
                            state, audio = 0x44, 0x60
                        else:
                            u32 = lambda off: struct.unpack_from('>I', data, off)[0]
                            info = u32(0x10) + 8
                            channel = info + u32(info + u32(info + 0x10))
                            state = info + u32(channel + 4) + 0x28
                            audio = u32(0x18) + 8 + u32(channel)
                        ps, h1, h2 = struct.unpack_from('>Hhh', data, state)
                        decoded = subprocess.run([FFMPEG, '-v', 'error', '-i', str(path),
                                                  '-f', 's16le', '-'], capture_output=True, timeout=15)
                        self.assertEqual(decoded.returncode, 0, decoded.stderr.decode())
                        samples = struct.unpack('<%dh' % (len(decoded.stdout) // 2), decoded.stdout)
                        self.assertEqual(ps, data[audio + start // 14 * 8])
                        self.assertEqual(h1, samples[start - 1] if start else 0)
                        self.assertEqual(h2, samples[start - 2] if start > 1 else 0)

    def test_invalid_loop_ranges_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            for fmt in ('dsp', 'bns'):
                for start in (640, 2**63 - 1):
                    with self.subTest(fmt=fmt, start=start):
                        result, _ = self.encode(directory, fmt, start)
                        self.assertGreater(result.returncode, 0)
            for start, end in ((15, 14), (0, 640), (0, 2**63 - 1)):
                with self.subTest(start=start, end=end):
                    result, _ = self.encode(directory, 'dsp', start, end)
                    self.assertGreater(result.returncode, 0)
