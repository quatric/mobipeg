"""Run with python3 -m unittest discover -s tests/nintendo after building FFmpeg."""
from pathlib import Path
import os
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
FFMPEG = os.environ.get('FFMPEG', str(ROOT / 'ffmpeg'))
FFPROBE = os.environ.get('FFPROBE', str(ROOT / 'ffprobe'))


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

    def encode_rvid_audio(self, options):
        output = self.root / 'audio.rvid'
        result = subprocess.run([
            FFMPEG, '-v', 'error', '-y', '-f', 'lavfi', '-i',
            'color=red:size=256x4:rate=30', '-f', 'lavfi', '-i',
            'sine=sample_rate=32000', '-t', '0.1', '-c:v', 'rvid',
            '-c:a', 'pcm_s16le'] + options + [str(output)],
            capture_output=True, timeout=15)
        return result, output

    def test_rvid_audio_first_keeps_video_frame_rate(self):
        result, output = self.encode_rvid_audio(['-map', '1:a', '-map', '0:v'])
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
        # v5 stores the nominal frame rate in byte 12 of the header.
        self.assertEqual(output.read_bytes()[12], 30)

    def test_rvid_rejects_unrepresentable_audio_and_duplicate_streams(self):
        for options in (['-ac', '3'], ['-ar', '96000'], ['-c:a', 'pcm_s16be'],
                        ['-map', '0:v', '-map', '1:a', '-map', '1:a'],
                        ['-map', '0:v', '-map', '0:v']):
            with self.subTest(options=options):
                result, _ = self.encode_rvid_audio(options)
                self.assertGreater(result.returncode, 0)

    def test_rvid_pcm_roundtrips(self):
        for channels in (1, 2):
            for codec, bps in (('pcm_u8', 1), ('pcm_s16le', 2)):
                with self.subTest(channels=channels, codec=codec):
                    raw = bytes((i * 37 + i // 17) % 256 for i in range(3200 * channels * bps))
                    source = self.root / 'source.pcm'
                    source.write_bytes(raw)
                    output = self.root / 'pcm.rvid'
                    result = subprocess.run([
                        FFMPEG, '-v', 'error', '-y', '-f', codec[4:],
                        '-ar', '32000', '-ac', str(channels), '-i', str(source),
                        '-f', 'lavfi', '-i', 'color=red:size=256x4:rate=30',
                        '-map', '0:a', '-map', '1:v', '-frames:v', '3',
                        '-c:a', 'copy', '-c:v', 'rvid', str(output)],
                        capture_output=True, timeout=15)
                    self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
                    result = subprocess.run([
                        FFMPEG, '-v', 'error', '-xerror', '-i', str(output), '-map', '0:a',
                        '-c:a', 'copy', '-f', codec[4:], '-'],
                        capture_output=True, timeout=15)
                    self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
                    self.assertEqual(result.stdout, raw)

    def test_rvid_rejects_invalid_tables_and_audio_ranges(self):
        valid = rvid_frame(bytes(512), compressed=False)
        cases = []
        short_table = bytearray(valid[:0x204])
        struct.pack_into('<I', short_table, 8, 4)
        cases.append(short_table)
        invalid_frame = bytearray(valid)
        struct.pack_into('<I', invalid_frame, 0x200, len(valid) - 1)
        cases.append(invalid_frame)
        invalid_fps = bytearray(valid)
        invalid_fps[12] = 0x80
        cases.append(invalid_fps)
        invalid_audio = bytearray(valid)
        struct.pack_into('<H', invalid_audio, 16, 32000)
        struct.pack_into('<II', invalid_audio, 24, len(valid) + 10, len(valid) + 20)
        cases.append(invalid_audio)
        short_sizes = bytearray(rvid_frame(lz_literals(bytes(512))))
        struct.pack_into('<I', short_sizes, 20, len(short_sizes) - 1)
        cases.append(short_sizes)
        for index, data in enumerate(cases):
            with self.subTest(case=index):
                path = self.root / 'invalid.rvid'
                path.write_bytes(data)
                result = subprocess.run([FFPROBE, '-v', 'error', '-f', 'rvid',
                                         '-show_streams', str(path)],
                                        capture_output=True, timeout=15)
                self.assertGreater(result.returncode, 0, result.stderr.decode(errors='replace'))

    def test_rvid_audio_packets_are_bounded_and_timestamped(self):
        # Both channel blocks are long enough to need several packets.
        frames = bytearray(rvid_frame(bytes(512), compressed=False))
        left = bytes(range(256)) * 160
        right = bytes(reversed(range(256))) * 160
        struct.pack_into('<H', frames, 16, 32000)
        frames[18] = 1
        struct.pack_into('<II', frames, 24, len(frames), len(frames) + len(left))
        path = self.root / 'packets.rvid'
        path.write_bytes(frames + left + right)
        result = subprocess.run([FFPROBE, '-v', 'error', '-select_streams', 'a',
                                 '-show_packets', '-show_entries', 'packet=pts,duration,size',
                                 '-of', 'json', str(path)], capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
        import json
        packets = json.loads(result.stdout)['packets']
        self.assertGreater(len(packets), 1)
        samples = 0
        for packet in packets:
            self.assertLessEqual(int(packet['size']), 16384)
            self.assertEqual(packet['pts'], samples)
            samples += packet['duration']
        self.assertEqual(samples, len(left) // 2)
