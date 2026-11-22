#!/usr/bin/env python3
"""Tests for mobipeg_tools.py and the encode.py options built on it.

Pure-Python checks run anywhere. The encode.py integration tests drive the real
script with a stub ffmpeg (so they need no MobiClip-capable build) and the
repo's own ffprobe/ffmpeg only to make a small multi-audio test clip; they
skip themselves if those binaries are missing.

    python3 test_mobipeg_tools.py
"""

import json
import os
import stat
import struct
import subprocess
import sys
import tempfile
import textwrap
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import mobipeg_tools as mt  # noqa: E402

REAL_FFMPEG = os.environ.get("FFMPEG", os.path.join(HERE, "ffmpeg"))
REAL_FFPROBE = os.environ.get("FFPROBE", os.path.join(HERE, "ffprobe"))
HAVE_BINARIES = os.path.isfile(REAL_FFMPEG) and os.path.isfile(REAL_FFPROBE)


class AudioHelpers(unittest.TestCase):
    def test_offset_filter(self):
        self.assertIsNone(mt.audio_offset_filter(0))
        self.assertIsNone(mt.audio_offset_filter("x"))
        self.assertEqual(mt.audio_offset_filter(250), "adelay=250:all=1")
        self.assertEqual(
            mt.audio_offset_filter(-1500), "atrim=start=1.500,asetpts=PTS-STARTPTS"
        )

    def test_normalize_gain(self):
        self.assertEqual(mt.normalize_gain_db(None), 0.0)
        self.assertEqual(mt.normalize_gain_db(0.0), 0.0)  # already louder than target
        self.assertAlmostEqual(mt.normalize_gain_db(-11.0), 10.0)
        self.assertEqual(mt.normalize_gain_db(-60.0), mt.NORMALIZE_MAX_GAIN_DB)

    def test_filter_chain_and_merge(self):
        self.assertEqual(mt.audio_filter_chain(0, 0.0), "")
        self.assertEqual(
            mt.audio_filter_chain(100, 6.0), "volume=6.00dB,adelay=100:all=1"
        )
        self.assertEqual(mt.merge_af(["-an"], ""), ["-an"])
        self.assertEqual(mt.merge_af(["-c:a", "x"], "volume=3dB"),
                         ["-c:a", "x", "-af", "volume=3dB"])
        self.assertEqual(
            mt.merge_af(["-af", "apad=pad_dur=0.25"], "volume=3dB"),
            ["-af", "volume=3dB,apad=pad_dur=0.25"],
        )


class HdrHelpers(unittest.TestCase):
    def test_off(self):
        self.assertEqual(mt.hdr_tonemap_chain("off", set()), (None, None))

    def test_missing_filters_warn(self):
        chain, warn = mt.hdr_tonemap_chain("auto", {"tonemap"})
        self.assertIsNone(chain)
        self.assertIn("zscale", warn)

    def test_chain(self):
        chain, warn = mt.hdr_tonemap_chain("hable", {"zscale", "tonemap"})
        self.assertIsNone(warn)
        self.assertIn("tonemap=tonemap=hable", chain)
        self.assertTrue(chain.endswith("format=yuv420p"))
        auto, _ = mt.hdr_tonemap_chain("auto", {"zscale", "tonemap"})
        self.assertIn("tonemap=tonemap=mobius", auto)


class SizeBudget(unittest.TestCase):
    def test_budget_and_retry(self):
        # 60 s into 10 MiB with 64 kb/s audio.
        bps = mt.video_bps_for_budget(10 * 1048576, 60, 64000)
        total = (bps + 64000) * 60 / 8
        self.assertLess(total, 10 * 1048576)
        self.assertGreater(total, 9 * 1048576)
        self.assertEqual(mt.video_bps_for_budget(1, 0, 0), 0)
        self.assertLess(mt.retry_bps(1_000_000, 12_000_000, 10_000_000), 1_000_000)
        self.assertEqual(mt.format_bps(123_456), "123k")

    def test_fit_loop(self):
        with tempfile.TemporaryDirectory() as d:
            out = os.path.join(d, "o.bin")

            def run_once(bps):
                with open(out, "wb") as f:
                    f.write(b"\0" * int(bps * 1.5 / 8))  # always 1.5x over budget
                return True

            ok, size, _ = mt.fit_to_size(run_once, out, 100_000, 400_000, log=lambda *_: None)
            self.assertTrue(ok)
            self.assertLessEqual(size, 100_000)

            ok, size, _ = mt.fit_to_size(
                lambda bps: open(out, "wb").write(b"\0" * 500_000) and True,
                out, 100_000, 400_000, attempts=2, log=lambda *_: None,
            )
            self.assertFalse(ok)


def make_moflex(path, blocks=6, block=2048, super_trailer=False, backwards=False):
    """A structurally valid-looking MOFLEX: 4C32 sync header on every other block."""
    data = bytearray()
    ts = 0
    for i in range(blocks):
        b = bytearray(block)
        if i % 2 == 0:
            t = (blocks - i if backwards else i) * 1_000_000
            b[0:2] = b"\x4c\x32"
            b[4:12] = struct.pack(">Q", t)
            b[12:14] = struct.pack(">H", block - 1)
        data += b
    if super_trailer:
        poff = len(data)
        data += b"META" + bytes(12) + struct.pack("<Q", poff) + b"CSXTRA01"
    with open(path, "wb") as f:
        f.write(data)


class MoflexInspect(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)

    def path(self, name="a.moflex"):
        return os.path.join(self.dir.name, name)

    def test_good_file(self):
        p = self.path()
        make_moflex(p)
        info = mt.inspect_moflex(p)
        self.assertEqual(info["errors"], [])
        self.assertEqual(info["block_size"], 2048)
        self.assertEqual(info["sync_blocks"], 3)
        self.assertFalse(info["super"])
        self.assertIn("result:      ok", mt.format_inspection(info))

    def test_not_moflex(self):
        p = self.path("b.moflex")
        with open(p, "wb") as f:
            f.write(b"RIFF" + bytes(100))
        info = mt.inspect_moflex(p)
        self.assertTrue(any("not a MOFLEX" in e for e in info["errors"]))

    def test_timestamps_backwards(self):
        p = self.path()
        make_moflex(p, backwards=True)
        info = mt.inspect_moflex(p)
        self.assertTrue(any("backwards" in e for e in info["errors"]))

    def test_super_trailer(self):
        p = self.path()
        make_moflex(p, super_trailer=True)
        info = mt.inspect_moflex(p)
        self.assertTrue(info["super"])
        self.assertEqual(info["errors"], [])

    def test_truncated_block(self):
        p = self.path()
        make_moflex(p)
        with open(p, "ab") as f:
            f.write(b"\0" * 100)
        info = mt.inspect_moflex(p)
        self.assertTrue(any("whole number" in w for w in info["warnings"]))

    def test_missing_file(self):
        self.assertTrue(mt.inspect_moflex(self.path("nope"))["errors"])


class Store(unittest.TestCase):
    def test_roundtrip_and_corruption(self):
        with tempfile.TemporaryDirectory() as d:
            s = mt.JsonStore("s.json", d)
            self.assertEqual(s.load(), {})
            self.assertTrue(s.save({"a": 1}))
            self.assertEqual(s.load(), {"a": 1})
            with open(s.path, "w") as f:
                f.write("{not json")
            self.assertEqual(s.load(), {})

    def test_builtin_presets_shape(self):
        keys = {"hq", "quant", "subme", "skip", "me", "bitrate", "multipass", "max_size_mb"}
        for name, p in mt.BUILTIN_PRESETS.items():
            self.assertEqual(set(p), keys, name)


class Progress(unittest.TestCase):
    def test_parse_progress(self):
        self.assertAlmostEqual(
            mt.parse_progress_seconds("frame= 10 fps=0 time=00:01:02.50 bitrate=1"), 62.5
        )
        self.assertEqual(mt.parse_progress_seconds("time=01:00:00.00"), 3600.0)
        self.assertIsNone(mt.parse_progress_seconds("size=N/A time=N/A"))
        self.assertIsNone(mt.parse_progress_seconds(">> pass 1/2"))


    def test_eta(self):
        self.assertEqual(mt.eta_text(0.0, 100), "")        # no progress yet
        self.assertEqual(mt.eta_text(0.5, 2), "")          # too early to say
        self.assertEqual(mt.eta_text(1.0, 100), "")        # finished
        text = mt.eta_text(0.25, 60, now=0)                # 60 s for 25% -> 180 s left
        self.assertTrue(text.startswith("ETA 3:00 ("), text)

    def test_clock(self):
        self.assertEqual(mt.format_clock(65), "1:05")
        self.assertEqual(mt.format_clock(3725), "1:02:05")
        self.assertEqual(mt.format_clock(None), "?")

    def test_describe_source(self):
        info = {"duration": 3725.0, "width": 1920, "height": 1080, "fps": 23.976,
                "hdr": "pq", "audio": [{}, {}]}
        text = mt.describe_source(info)
        for part in ("1920x1080", "23.976 fps", "1:02:05", "HDR (PQ)", "2 audio tracks"):
            self.assertIn(part, text)


class QueueModel(unittest.TestCase):
    def make(self):
        q = mt.JobQueue()
        for n in "abc":
            q.add(["cmd", n], n, output=n + ".out")
        return q

    def test_order_and_status(self):
        q = self.make()
        self.assertEqual(q.next_pending()["label"], "a")
        q.set_status(1, mt.QUEUE_RUNNING)
        self.assertEqual(q.next_pending()["label"], "b")
        q.set_status(1, mt.QUEUE_DONE)
        q.set_status(2, mt.QUEUE_FAILED, "exit 1")
        self.assertEqual(q.counts(), {"done": 1, "failed": 1, "pending": 1})
        q.retry_failed()
        self.assertEqual(q.get(2)["status"], mt.QUEUE_PENDING)
        q.clear_finished()
        self.assertEqual([j["label"] for j in q.jobs], ["b", "c"])

    def test_move_and_remove(self):
        q = self.make()
        q.move([3], -1)
        self.assertEqual([j["label"] for j in q.jobs], ["a", "c", "b"])
        q.move([1], -1)  # already first: no change
        self.assertEqual([j["label"] for j in q.jobs], ["a", "c", "b"])
        q.move([1, 2], 1)  # a moves down; b is already last
        self.assertEqual([j["label"] for j in q.jobs][-1], "b")
        q.set_status(1, mt.QUEUE_RUNNING)
        q.remove([1, 2])  # a is running so it stays; b goes
        self.assertEqual({j["label"] for j in q.jobs}, {"a", "c"})

    def test_persistence(self):
        q = self.make()
        q.set_status(1, mt.QUEUE_RUNNING)
        q.set_status(2, mt.QUEUE_DONE)
        r = mt.JobQueue.from_list(json.loads(json.dumps(q.to_list())))
        self.assertEqual(r.get(1)["status"], mt.QUEUE_PENDING)  # was running
        self.assertEqual(r.get(2)["status"], mt.QUEUE_DONE)
        self.assertEqual(r.add(["x"], "d")["id"], 4)
        self.assertEqual(mt.JobQueue.from_list("garbage").jobs, [])
        self.assertEqual(mt.JobQueue.from_list([{"id": "x"}, 5]).jobs, [])


@unittest.skipUnless(HAVE_BINARIES, "needs the repo's ffprobe")
class ProbeSource(unittest.TestCase):
    def test_probe_clip(self):
        with tempfile.TemporaryDirectory() as d:
            clip = os.path.join(d, "c.mkv")
            subprocess.run(
                [REAL_FFMPEG, "-nostdin", "-y", "-v", "error",
                 "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=25:duration=2",
                 "-f", "lavfi", "-i", "sine=duration=2",
                 "-c:v", "mpeg4", "-c:a", "aac", clip],
                check=True,
            )
            info = mt.probe_source(REAL_FFPROBE, clip)
            self.assertEqual((info["width"], info["height"]), (320, 180))
            self.assertAlmostEqual(info["fps"], 25.0, places=1)
            self.assertAlmostEqual(info["duration"], 2.0, delta=0.3)
            self.assertEqual(len(info["audio"]), 1)
            self.assertIsNone(info["hdr"])
        self.assertIsNone(mt.probe_source(REAL_FFPROBE, "/nonexistent")["hdr"])


STUB = textwrap.dedent(
    """\
    #!/usr/bin/env python3
    # Stand-in ffmpeg: logs its command line, and for an encode writes a file
    # whose size is proportional to -b:v (3x over, to force a size retry).
    import json, os, sys
    a = sys.argv[1:]
    log = os.environ["STUB_LOG"]
    with open(log, "a") as f:
        f.write(json.dumps(a) + "\\n")
    if "-filters" in a or "-version" in a:
        sys.exit(0)
    if "volumedetect" in " ".join(a):
        sys.stderr.write("max_volume: -20.0 dB\\n")
        sys.exit(0)
    if "-i" in a and a[-1].endswith((".moflex", ".mo", ".mods")) and "-f" not in a:
        bps = 1_000_000
        if "-b:v" in a:
            v = a[a.index("-b:v") + 1]
            bps = int(v[:-1]) * 1000
        with open(a[-1], "wb") as f:
            f.write(b"\\0" * int(bps * 3 / 8 * 6))
    sys.exit(0)
    """
)


@unittest.skipUnless(HAVE_BINARIES, "needs the repo's ffmpeg/ffprobe")
class EncodeCli(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.TemporaryDirectory()
        d = cls.dir.name
        cls.clip = os.path.join(d, "multi.mkv")
        subprocess.run(
            [REAL_FFMPEG, "-nostdin", "-y", "-v", "error",
             "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=30:duration=6",
             "-f", "lavfi", "-i", "sine=frequency=440:duration=6:sample_rate=32000",
             "-f", "lavfi", "-i", "sine=frequency=880:duration=6:sample_rate=32000",
             "-map", "0", "-map", "1", "-map", "2",
             "-metadata:s:a:0", "language=eng", "-metadata:s:a:1", "language=jpn",
             "-c:v", "mpeg4", "-q:v", "5", "-c:a", "aac", cls.clip],
            check=True,
        )
        cls.stub = os.path.join(d, "ffmpeg_stub")
        with open(cls.stub, "w") as f:
            f.write(STUB)
        os.chmod(cls.stub, os.stat(cls.stub).st_mode | stat.S_IXUSR)
        cls.log = os.path.join(d, "stub.log")

    @classmethod
    def tearDownClass(cls):
        cls.dir.cleanup()

    def run_encode(self, *args, stub=True):
        env = dict(os.environ, STUB_LOG=self.log)
        env.setdefault("FFPROBE", REAL_FFPROBE)
        env["FFMPEG"] = self.stub if stub else REAL_FFMPEG
        if os.path.exists(self.log):
            os.remove(self.log)
        p = subprocess.run(
            [sys.executable, os.path.join(HERE, "encode.py")] + list(args)
            + ["--jobs", "1"],  # one whole-file encode: the stub cannot join chunks
            capture_output=True, text=True, env=env,
        )
        calls = []
        if os.path.exists(self.log):
            with open(self.log) as f:
                calls = [json.loads(l) for l in f if l.strip()]
        return p, calls

    def encodes(self, calls):
        return [c for c in calls if "-c:v" in c]

    def test_list_audio_tracks(self):
        p, _ = self.run_encode("moflex", "adpcm", self.clip, "--list-audio-tracks")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("track 0", p.stdout)
        self.assertIn("jpn", p.stdout)

    def test_bad_track_rejected(self):
        p, _ = self.run_encode("moflex", "adpcm", self.clip, "--audio-track", "9",
                               "-o", os.path.join(self.dir.name, "x"))
        self.assertEqual(p.returncode, 2)
        self.assertIn("2 audio track", p.stdout)

    def test_track_offset_normalize_reach_ffmpeg(self):
        out = os.path.join(self.dir.name, "t1")
        p, calls = self.run_encode(
            "moflex", "adpcm", self.clip, "--audio-track", "1",
            "--audio-offset-ms", "250", "--normalize-audio", "-o", out,
        )
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        enc = self.encodes(calls)[0]
        self.assertIn("0:a:1?", enc)
        af = enc[enc.index("-af") + 1]
        self.assertIn("volume=19.00dB", af)  # peak -20 dB -> -1 dBFS
        self.assertIn("adelay=250:all=1", af)

    def test_max_size_converges(self):
        out = os.path.join(self.dir.name, "fit")
        p, calls = self.run_encode(
            "moflex", "adpcm", self.clip, "--max-size-mb", "0.4", "-o", out,
        )
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertGreater(len(self.encodes(calls)), 1)  # first try was too big
        self.assertLessEqual(os.path.getsize(out + ".moflex"), int(0.4 * 1048576))
        # qp must not be passed alongside the derived bitrate
        self.assertNotIn("-qp", self.encodes(calls)[-1])

    def test_max_size_impossible_fails(self):
        out = os.path.join(self.dir.name, "tiny")
        p, _ = self.run_encode(
            "moflex", "adpcm", self.clip, "--max-size-mb", "0.001", "-o", out,
        )
        self.assertEqual(p.returncode, 1)
        self.assertIn("could not get the output under", p.stdout)

    def test_hdr_flag_is_inert_for_sdr_source(self):
        out = os.path.join(self.dir.name, "sdr")
        p, calls = self.run_encode("moflex", "adpcm", self.clip, "-o", out)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        enc = self.encodes(calls)[0]
        self.assertNotIn("zscale", " ".join(enc))

    def test_native_format_with_real_ffmpeg(self):
        out = os.path.join(self.dir.name, "real")
        p, _ = self.run_encode(
            "vx", "codebook", self.clip, "--audio-track", "1",
            "--audio-offset-ms", "-200", "--fast-audio", "-o", out, stub=False,
        )
        self.assertEqual(p.returncode, 0, p.stdout[-800:] + p.stderr[-800:])
        self.assertTrue(os.path.getsize(out + ".vx") > 1000)

    def test_inspect_cli(self):
        mo = os.path.join(self.dir.name, "i.moflex")
        make_moflex(mo)
        # The synthetic file has no stream descriptors, so run without ffprobe:
        # only the block-level checks apply and they must pass.
        env_probe = os.environ.get("FFPROBE")
        os.environ["FFPROBE"] = "/nonexistent/ffprobe"
        try:
            p, _ = self.run_encode("inspect", mo, "--json")
        finally:
            if env_probe is None:
                os.environ.pop("FFPROBE", None)
            else:
                os.environ["FFPROBE"] = env_probe
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        data = json.loads(p.stdout)
        self.assertEqual(data["block_size"], 2048)
        bad = os.path.join(self.dir.name, "bad.moflex")
        with open(bad, "wb") as f:
            f.write(b"x" * 64)
        p, _ = self.run_encode("inspect", bad)
        self.assertEqual(p.returncode, 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
