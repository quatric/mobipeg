"""AST stream-copy alignment regressions."""
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

FFMPEG = os.environ.get('FFMPEG', str(Path(__file__).resolve().parents[2] / 'ffmpeg'))


class ASTPacketTests(unittest.TestCase):
    def copy_block(self, codec, channel_bytes):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'input.ast'
            target = Path(directory) / 'output.ast'
            header = bytearray(64)
            struct.pack_into('>4sIHHHHII', header, 0, b'STRM', 32 + channel_bytes * 2,
                             codec, 16, 2, 0, 32000, 16 if codec == 0 else channel_bytes // 2)
            struct.pack_into('>I', header, 32, channel_bytes)
            source.write_bytes(header + b'BLCK' + struct.pack('>I', channel_bytes) +
                               bytes(24 + channel_bytes * 2))
            return subprocess.run([FFMPEG, '-v', 'error', '-y', '-i', str(source),
                                   '-c:a', 'copy', str(target)], capture_output=True, timeout=15)

    def test_incomplete_channel_frames_are_rejected(self):
        for codec, size in ((1, 3), (0, 8)):
            with self.subTest(codec=codec):
                result = self.copy_block(codec, size)
                self.assertGreater(result.returncode, 0, result.stderr.decode())

    def test_complete_channel_frames_are_accepted(self):
        for codec, size in ((1, 32), (0, 9)):
            with self.subTest(codec=codec):
                result = self.copy_block(codec, size)
                self.assertEqual(result.returncode, 0, result.stderr.decode())

    def test_sample_count_bounds_playback(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'input.ast'
            for declared in (0, 1, 2):
                with self.subTest(declared=declared):
                    header = bytearray(64)
                    struct.pack_into('>4sIHHHHII', header, 0, b'STRM', 72, 1, 16, 1,
                                     0, 32000, declared)
                    block = b'BLCK' + struct.pack('>I', 4) + bytes(24) + struct.pack('>hh', 1234, -2345)
                    source.write_bytes(header + block + block)
                    result = subprocess.run([FFMPEG, '-v', 'error', '-i', str(source),
                                             '-f', 's16le', '-'], capture_output=True, timeout=15)
                    self.assertEqual(result.returncode, 0, result.stderr.decode())
                    self.assertEqual(result.stdout, struct.pack('<hh', 1234, -2345)[:declared * 2])

    def test_truncated_block_does_not_emit_partial_packet(self):
        ffprobe = os.environ.get('FFPROBE', str(Path(__file__).resolve().parents[2] / 'ffprobe'))
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'truncated.ast'
            header = bytearray(64)
            struct.pack_into('>4sIHHHHII', header, 0, b'STRM', 40, 1, 16, 1, 0, 32000, 4)
            source.write_bytes(header + b'BLCK' + struct.pack('>I', 8) + bytes(24) + bytes(4))
            result = subprocess.run([ffprobe, '-v', 'error', '-show_entries', 'packet=size',
                                     '-of', 'csv=p=0', str(source)], capture_output=True, timeout=15)
            self.assertEqual(result.stdout, b'')

    def test_pcm_seek_restores_sample_accounting(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'seek.ast'
            samples = list(range(1000, 1096))
            header = bytearray(64)
            struct.pack_into('>4sIHHHHII', header, 0, b'STRM', 288, 1, 16, 1, 0, 32000, 96)
            blocks = b''.join(b'BLCK' + struct.pack('>I', 64) + bytes(24) +
                              struct.pack('>32h', *samples[i:i + 32]) for i in range(0, 96, 32))
            source.write_bytes(header + blocks)
            for offset in (0, 32, 64):
                with self.subTest(offset=offset):
                    result = subprocess.run([FFMPEG, '-v', 'error', '-ss', str(offset / 32000),
                                             '-i', str(source), '-f', 's16le', '-'],
                                            capture_output=True, timeout=15)
                    self.assertEqual(result.returncode, 0, result.stderr.decode())
                    self.assertEqual(result.stdout, struct.pack('<%dh' % (96 - offset), *samples[offset:]))

    def test_afc_seek_preserves_predictor_history(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'audio.ast'
            encoded = subprocess.run([FFMPEG, '-v', 'error', '-f', 'lavfi', '-i',
                                      'sine=frequency=731:sample_rate=32000', '-t', '0.02',
                                      '-c:a', 'adpcm_afc', str(source)], capture_output=True, timeout=15)
            self.assertEqual(encoded.returncode, 0, encoded.stderr.decode())
            full = subprocess.run([FFMPEG, '-v', 'error', '-i', str(source), '-f', 's16le', '-'],
                                  capture_output=True, timeout=15)
            self.assertEqual(full.returncode, 0, full.stderr.decode())
            self.assertEqual(len(full.stdout), 1280)
            seek = subprocess.run([FFMPEG, '-v', 'error', '-ss', '0.001', '-i', str(source),
                                   '-f', 's16le', '-'], capture_output=True, timeout=15)
            self.assertEqual(seek.returncode, 0, seek.stderr.decode())
            self.assertEqual(seek.stdout, full.stdout[64:])

    def test_nonseekable_output_is_rejected_before_header(self):
        result = subprocess.run([FFMPEG, '-v', 'error', '-f', 'lavfi', '-i',
                                 'sine=frequency=440:sample_rate=32000', '-t', '0.01',
                                 '-c:a', 'pcm_s16be_planar', '-f', 'ast', 'pipe:1'],
                                capture_output=True, timeout=15)
        self.assertGreater(result.returncode, 0, result.stderr.decode())
        self.assertEqual(result.stdout, b'')
