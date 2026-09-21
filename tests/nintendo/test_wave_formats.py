"""Nintendo wave container regressions using the built command-line tools."""
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
FFMPEG = os.environ.get('FFMPEG', str(ROOT / 'ffmpeg'))
FFPROBE = os.environ.get('FFPROBE', str(ROOT / 'ffprobe'))


class WaveFormatTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.source = self.root / 'source.s16le'
        self.raw = b''.join(struct.pack('<hh', i % 30000 - 15000, 14000 - i % 27000)
                            for i in range(18000))
        self.source.write_bytes(self.raw)

    def encode(self, fmt, codec, endian, rate=32000):
        path = self.root / ('output.' + fmt)
        result = subprocess.run([
            FFMPEG, '-v', 'error', '-y', '-f', 's16le', '-ar', str(rate), '-ac', '2',
            '-i', str(self.source), '-c:a', codec, '-endian', endian, str(path)],
            capture_output=True, timeout=15)
        return result, path

    def test_wave_pcm_roundtrips_both_layouts_and_byte_orders(self):
        for fmt in ('rwav', 'fwav', 'cwav'):
            for endian in ('be', 'le'):
                for planar in (False, True):
                    with self.subTest(fmt=fmt, endian=endian, planar=planar):
                        codec = 'pcm_s16' + endian + ('_planar' if planar else '')
                        result, path = self.encode(fmt, codec, endian)
                        self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
                        # Do not force the input format: probes must resolve to the wave demuxer.
                        result = subprocess.run([
                            FFMPEG, '-v', 'error', '-xerror', '-i', str(path), '-f', 's16le', '-'],
                            capture_output=True, timeout=15)
                        self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
                        self.assertEqual(result.stdout, self.raw)

    def test_rwav_rejects_sample_rate_overflow(self):
        result, _ = self.encode('rwav', 'pcm_s16be_planar', 'be', 96000)
        self.assertGreater(result.returncode, 0)

    def test_wave_adpcm_probes_and_byte_orders(self):
        for fmt in ('rwav', 'fwav', 'cwav'):
            decoded = []
            for endian in ('be', 'le'):
                with self.subTest(fmt=fmt, endian=endian):
                    result, path = self.encode(fmt, 'adpcm_thp', endian)
                    self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
                    result = subprocess.run([
                        FFMPEG, '-v', 'error', '-xerror', '-i', str(path), '-f', 's16le', '-'],
                        capture_output=True, timeout=15)
                    self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
                    self.assertEqual(len(result.stdout), len(self.raw))
                    decoded.append(result.stdout)
            self.assertEqual(decoded[0], decoded[1])

    def test_wave_rejects_wrapping_channel_offsets(self):
        for fmt in ('rwav', 'fwav', 'cwav'):
            with self.subTest(fmt=fmt):
                result, path = self.encode(fmt, 'pcm_s16be_planar', 'be')
                self.assertEqual(result.returncode, 0)
                data = bytearray(path.read_bytes())
                if fmt == 'rwav':
                    info = struct.unpack_from('>I', data, 16)[0] + 8
                    table = info + struct.unpack_from('>I', data, info + 16)[0]
                    channel = info + struct.unpack_from('>I', data, table)[0]
                    offset_field = channel
                else:
                    info = struct.unpack_from('>I', data, 24)[0] + 8
                    anchor = info + 20
                    channel = anchor + struct.unpack_from('>I', data, anchor + 8)[0]
                    offset_field = channel + 4
                struct.pack_into('>I', data, offset_field, 0xfffffff0)
                path.write_bytes(data)
                result = subprocess.run([FFPROBE, '-v', 'error', '-show_streams', str(path)],
                                        capture_output=True, timeout=15)
                self.assertGreater(result.returncode, 0)

    def test_cwav_rejects_wrapping_sample_byte_count(self):
        result, path = self.encode('cwav', 'pcm_s16le_planar', 'le')
        self.assertEqual(result.returncode, 0)
        data = bytearray(path.read_bytes())
        info = struct.unpack_from('<I', data, 24)[0] + 8
        struct.pack_into('<I', data, info + 12, 0x80000000)
        path.write_bytes(data)
        result = subprocess.run([FFPROBE, '-v', 'error', '-show_streams', str(path)],
                                capture_output=True, timeout=15)
        self.assertGreater(result.returncode, 0)

    def test_wave_signed_pcm8_roundtrips(self):
        raw = bytes((i * 29 + i // 13) % 256 for i in range(4096))
        source = self.root / 'source.s8'
        source.write_bytes(raw)
        for fmt in ('rwav', 'fwav', 'cwav'):
            for codec in ('pcm_s8', 'pcm_s8_planar'):
                with self.subTest(fmt=fmt, codec=codec):
                    path = self.root / ('pcm8.' + fmt)
                    result = subprocess.run([
                        FFMPEG, '-v', 'error', '-y', '-f', 's8', '-ar', '32000', '-ac', '2',
                        '-i', str(source), '-c:a', codec, '-endian', 'le', str(path)],
                        capture_output=True, timeout=15)
                    self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
                    result = subprocess.run([
                        FFMPEG, '-v', 'error', '-xerror', '-i', str(path), '-c:a', 'pcm_s8', '-f', 's8', '-'],
                        capture_output=True, timeout=15)
                    self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
                    self.assertEqual(result.stdout, raw)
