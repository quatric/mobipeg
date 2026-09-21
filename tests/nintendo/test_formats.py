"""Run with python3 -m unittest discover -s tests/nintendo after building FFmpeg."""
from pathlib import Path
import os
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
FFMPEG = os.environ.get('FFMPEG', str(ROOT / 'ffmpeg'))


def lz_literals(data):
    return b'\x10' + len(data).to_bytes(3, 'little') + b''.join(
        b'\0' + data[i:i + 8] for i in range(0, len(data), 8))


def rvid_frame(payload, mode=1, compressed=True):
    header = bytearray(0x200)
    struct.pack_into('<4sII', header, 0, b'RVID', 5, 1)
    header[12:16] = bytes([30, 1, 0, 0])
    header[19] = mode
    if compressed:
        struct.pack_into('<I', header, 20, 0x204)
        sizes = struct.pack('<H' if mode == 0 else '<I',
                            len(payload) - (512 if mode == 0 else 0))
    else:
        sizes = b''
    return header + struct.pack('<I', 0x204 + len(sizes)) + sizes + payload


class NintendoFormatTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)

    def decode(self, data, extension):
        path = self.root / ('input.' + extension)
        path.write_bytes(data)
        return subprocess.run([FFMPEG, '-v', 'error', '-xerror', '-f', extension, '-i', str(path),
                               '-f', 'null', '-'], capture_output=True, timeout=15)

    def test_nested_bns_invalid_size_is_rejected_without_crash(self):
        result = self.decode(lz_literals(b'\x10\x01\0\0\0\0\0\0'), 'bns')
        self.assertGreater(result.returncode, 0, result.stderr.decode(errors='replace'))

    def test_nested_bns_invalid_backreference_is_rejected_without_crash(self):
        result = self.decode(lz_literals(b'\x10\x08\0\0\x80\0\0\0'), 'bns')
        self.assertGreater(result.returncode, 0, result.stderr.decode(errors='replace'))

    def test_rvid_short_decompressed_frame_is_rejected(self):
        for mode in (0, 1, 2):
            with self.subTest(mode=mode):
                palette = bytes(512) if mode == 0 else b''
                result = self.decode(rvid_frame(palette + lz_literals(b'\0'), mode), 'rvid')
                self.assertGreater(result.returncode, 0, result.stderr.decode(errors='replace'))

    def test_rvid_complete_compressed_frames_decode(self):
        for mode in (0, 1, 2):
            with self.subTest(mode=mode):
                palette = bytes(512) if mode == 0 else b''
                pixels = bytes(256 if mode == 0 else 512)
                result = self.decode(rvid_frame(palette + lz_literals(pixels), mode), 'rvid')
                self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))

    def test_rvid_invalid_pixel_mode_is_rejected(self):
        result = self.decode(rvid_frame(bytes(512), 3, False), 'rvid')
        self.assertGreater(result.returncode, 0, result.stderr.decode(errors='replace'))

    def test_bns_nested_valid_wrappers_preserve_audio(self):
        output = self.root / 'sound.bns'
        result = subprocess.run([FFMPEG, '-v', 'error', '-f', 'lavfi', '-i',
                                 'sine=frequency=440:sample_rate=32000', '-t', '0.1',
                                 '-c:a', 'adpcm_thp', '-compress', '0', str(output)],
                                capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
        bare = output.read_bytes()
        expected = None
        for data in (bare, lz_literals(bare), lz_literals(lz_literals(bare))):
            output.write_bytes(data)
            result = subprocess.run([FFMPEG, '-v', 'error', '-xerror', '-f', 'bns',
                                     '-i', str(output), '-f', 's16le', '-'],
                                    capture_output=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
            self.assertTrue(result.stdout)
            if expected is None:
                expected = result.stdout
            self.assertEqual(result.stdout, expected)

    def test_rvid_roundtrips_all_storage_modes(self):
        for mode in (0, 1, 2):
            for compressed in (0, 1):
                for interlaced in (0, 1):
                    with self.subTest(mode=mode, compressed=compressed, interlaced=interlaced):
                        output = self.root / 'roundtrip.rvid'
                        result = subprocess.run([
                            FFMPEG, '-v', 'error', '-y', '-f', 'lavfi', '-i',
                            'color=red:size=256x4:rate=30', '-frames:v', '2',
                            '-c:v', 'rvid', '-pix_fmt', 'rgb24', '-mode', str(mode),
                            '-compress', str(compressed), '-interlaced', str(interlaced),
                            '-dither', '0', str(output)], capture_output=True, timeout=15)
                        self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
                        result = subprocess.run([
                            FFMPEG, '-v', 'error', '-xerror', '-i', str(output),
                            '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'],
                            capture_output=True, timeout=15)
                        self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
                        self.assertEqual(len(result.stdout), 2 * 256 * 4 * 3)
                        # The second field completes the persistent image.
                        frame = result.stdout[256 * 4 * 3:]
                        self.assertEqual(frame, bytes([255, 0, 0]) * (256 * 4))
