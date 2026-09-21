"""Front-end regressions. Run with python3 -m unittest discover -s tests/frontend."""
import contextlib
import io
import os
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
import encode


class FrontendTests(unittest.TestCase):
    def test_explicit_output_ignores_outdir(self):
        self.assertEqual(encode.resolve_output_stem('movie.mp4', 'default', '/unused'),
                         os.path.join('.', 'movie'))

    def test_completion_needs_no_external_program(self):
        with tempfile.NamedTemporaryFile() as output:
            output.write(b'123')
            output.flush()
            with patch.object(subprocess, 'run', side_effect=AssertionError), \
                    contextlib.redirect_stdout(io.StringIO()) as log:
                encode.report_outputs(output.name)
            self.assertIn('(3 bytes)', log.getvalue())

    def test_failed_preprocess_removes_partial_file(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory, 'source.mp4')
            source.touch()
            with patch.object(encode, '_decodes_cleanly', return_value=False), \
                    patch.object(encode.shutil, 'which', return_value='/separate/ffmpeg'), \
                    patch.object(subprocess, 'Popen', side_effect=FileNotFoundError), \
                    contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(encode.preprocess_input(str(source), directory), str(source))
            self.assertEqual(list(Path(directory).iterdir()), [source])


@unittest.skipUnless(Path(encode.FFENC).is_file() and Path(encode.FFPROBE).is_file(),
                     'requires built ffmpeg and ffprobe')
class IntegrationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / 'source.mkv'
        self.run_process([encode.FFENC, '-v', 'error', '-f', 'lavfi', '-i',
                          'testsrc2=size=64x48:rate=10', '-f', 'lavfi', '-i',
                          'sine=frequency=440:sample_rate=16000', '-t', '0.3',
                          '-c:v', 'mpeg4', '-c:a', 'pcm_s16le', str(self.source)])

    def run_process(self, command):
        result = subprocess.run(command, cwd=self.root, capture_output=True,
                                text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        return result

    def test_decode_creates_explicit_parent_and_ignores_outdir(self):
        output = self.root / 'new' / 'movie.mp4'
        self.run_process([sys.executable, encode.__file__, 'decode', str(self.source),
                          '--stereo', 'none', '--outdir', str(self.source / 'invalid'),
                          '-o', str(output)])
        self.assertGreater(output.stat().st_size, 0)

    def test_audio_encode_relative_output_ignores_outdir(self):
        self.run_process([sys.executable, encode.__file__, 'btsnd', 'pcm',
                          str(self.source), '--outdir', str(self.source / 'invalid'),
                          '-o', 'sound.btsnd'])
        self.assertGreater((self.root / 'sound.btsnd').stat().st_size, 0)

    def test_preprocess_encoder_exit_does_not_hang(self):
        driver = r"""
import subprocess
import sys
from unittest.mock import patch
import encode
real_popen = subprocess.Popen
calls = 0
def launch(command, **kwargs):
    global calls
    calls += 1
    code = ('import os;\nwhile True: os.write(1, b"x" * 65536)'
            if calls == 1 else 'raise SystemExit(1)')
    return real_popen([sys.executable, '-c', code], **kwargs)
with patch.object(encode, '_decodes_cleanly', return_value=False), \
     patch.object(encode.shutil, 'which', return_value='/separate/ffmpeg'), \
     patch.object(subprocess, 'Popen', side_effect=launch):
    assert encode.preprocess_input(sys.argv[1], sys.argv[2]) == sys.argv[1]
"""
        env = dict(os.environ, PYTHONPATH=str(Path(encode.__file__).parent))
        result = subprocess.run([sys.executable, '-c', driver, str(self.source),
                                 str(self.root)], env=env, capture_output=True,
                                text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(list(self.root.glob('.preprocessed_*')), [])

    def test_preprocess_preserves_audio_and_uses_unique_files(self):
        # A separate copy of the bundled binary exercises the actual pipe.
        alias = self.root / 'decoder'
        shutil.copy2(encode.FFENC, alias)
        with patch.object(encode, '_decodes_cleanly', return_value=False), \
                patch.object(encode.shutil, 'which', return_value=str(alias)):
            first = encode.preprocess_input(str(self.source), str(self.root))
            second = encode.preprocess_input(str(self.source), str(self.root))
        self.assertNotEqual(first, str(self.source))
        self.assertNotEqual(first, second)
        result = self.run_process([encode.FFPROBE, '-v', 'error', '-show_entries',
                                   'stream=codec_name', '-of', 'csv=p=0', first])
        self.assertIn('h264', result.stdout)
        self.assertIn('pcm_s16le', result.stdout)
