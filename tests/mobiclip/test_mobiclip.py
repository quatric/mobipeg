"""Regression tests for the Mobiclip containers (.mo, .mods, .moflex), the
audio codecs they carry and the Mobiclip video decoder.

Run after building ffmpeg/ffprobe:

    python3 -m unittest discover -s tests/mobiclip        (or)
    python3 -m pytest tests/mobiclip

Regenerate the golden files in tests/mobiclip/ref/ after an intentional
change with

    python3 tests/mobiclip/test_mobiclip.py --update-refs
    python3 -m pytest tests/mobiclip --update-refs
    MOBIPEG_UPDATE_REFS=1 python3 -m unittest discover -s tests/mobiclip

Three kinds of input are covered:

  * samples/     small cuts of retail files (see samples/README.md), checked
                 against the reference files for stream layout, per-packet
                 layout and payload hashes, per-frame decoded video/audio
                 md5s, and a stream-copy remux through our own muxers;
  * synthetic    clips encoded at test time by our encoders for every
                 container/audio-codec combination, checked for a stable
                 decode, lossless PCM, and round-trip fidelity;
  * $MOBIPEG_SAMPLES  an optional directory of larger retail files, checked
                 against the checksums in ref/external.json (skipped when the
                 variable is unset).

Tests marked expectedFailure pin known bugs; each says what is wrong.  When a
fix lands they start reporting "unexpected success" and the marker should go.
"""

from pathlib import Path
import difflib
import hashlib
import json
import math
import os
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import make_fixtures  # noqa: E402

SAMPLES = HERE / "samples"
REF = HERE / "ref"
FFMPEG = os.environ.get("FFMPEG", str(ROOT / "ffmpeg"))
FFPROBE = os.environ.get("FFPROBE", str(ROOT / "ffprobe"))
TIMEOUT = 120


def updating():
    return os.environ.get("MOBIPEG_UPDATE_REFS") == "1"


def run(args, timeout=TIMEOUT, check=True):
    result = subprocess.run(args, capture_output=True, timeout=timeout)
    if check and result.returncode:
        raise AssertionError(
            f"{' '.join(map(str, args))} failed ({result.returncode}):\n"
            + result.stderr.decode(errors="replace")
        )
    return result


def ffmpeg(*args, **kwargs):
    return run(
        [FFMPEG, "-nostdin", "-v", "error", "-threads", "1", *map(str, args)], **kwargs
    )


def ffprobe(*args, **kwargs):
    return run([FFPROBE, "-v", "error", *map(str, args)], **kwargs)


# --------------------------------------------------------------------------
# Probing helpers.  Everything is reduced to plain text so the references are
# diffable and a failure points at the first frame/packet that changed.


def stream_info(path):
    return ffprobe(
        "-show_entries",
        "stream=index,codec_type,codec_name,width,height,pix_fmt,"
        "sample_fmt,sample_rate,channels",
        "-of",
        "csv",
        path,
    ).stdout.decode()


def packet_info(path, streams=None):
    args = ["-show_packets", "-show_data_hash", "MD5"]
    if streams:
        args += ["-select_streams", streams]
    args += [
        "-show_entries",
        "packet=stream_index,pts,dts,duration,size,flags,data_hash",
        "-of",
        "csv",
        path,
    ]
    return ffprobe(*args).stdout.decode()


def packet_payloads(path, streams):
    """Just the payload hashes, in order - what a stream copy must keep."""
    return [line.rsplit(",", 1)[-1] for line in packet_info(path, streams).splitlines()]


def decode(path, maps=("0:v?", "0:a?")):
    """Per-frame md5 of every decoded video and audio frame.

    Packets the demuxer flags as corrupt (the one a cut .moflex ends on) are
    dropped so the reference only holds frames the bitstream fully describes.
    """
    args = ["-fflags", "+discardcorrupt", "-i", path]
    for m in maps:
        args += ["-map", m]
    result = ffmpeg(
        *args, "-flags", "+bitexact", "-fflags", "+bitexact", "-f", "framemd5", "-"
    )
    return result.stdout.decode(), result.stderr.decode(errors="replace")


def audio_md5(path, index):
    return (
        ffmpeg(
            "-fflags",
            "+discardcorrupt",
            "-i",
            path,
            "-map",
            f"0:a:{index}",
            "-f",
            "md5",
            "-",
        )
        .stdout.decode()
        .strip()
    )


def audio_samples(path, channels):
    raw = ffmpeg(
        "-i",
        path,
        "-map",
        "0:a:0",
        "-ac",
        channels,
        "-f",
        "s16le",
        "-c:a",
        "pcm_s16le",
        "-",
    ).stdout
    return struct.unpack(f"<{len(raw) // 2}h", raw)


def count_streams(info, kind):
    return sum(1 for line in info.splitlines() if f",{kind}," in line)


def report(path, maps=("0:v?", "0:a?")):
    info = stream_info(path)
    frames, errors = decode(path, maps)
    text = ["# streams", info.rstrip(), "# packets", packet_info(path).rstrip()]
    text += ["# decode", frames.rstrip()]
    audio = [m for m in maps if m.startswith("0:a:")]
    indices = (
        [int(m.split(":")[2]) for m in audio]
        if audio
        else range(count_streams(info, "audio"))
    )
    for i in indices:
        text += [f"# audio {i} md5", audio_md5(path, i)]
    return "\n".join(text) + "\n", errors


class RefMixin:
    def check_ref(self, name, text):
        path = REF / name
        if updating():
            REF.mkdir(exist_ok=True)
            path.write_text(text)
            return
        if not path.exists():
            self.fail(f"missing reference {path.name}; run with --update-refs")
        expected = path.read_text()
        if expected != text:
            diff = difflib.unified_diff(
                expected.splitlines(),
                text.splitlines(),
                f"ref/{name}",
                "current",
                lineterm="",
                n=1,
            )
            lines = list(diff)
            self.fail(
                f"{name} differs from the reference:\n"
                + "\n".join(lines[:60])
                + ("\n..." if len(lines) > 60 else "")
            )


# --------------------------------------------------------------------------
# Retail fixtures.
#
# name -> options.  "maps" limits the decode reference to streams that decode
# correctly today, so a known bug is not baked into the golden output.


RETAIL = {
    "mo_vorbis.mo": {},
    "mo_vorbis_aa.mo": {},
    "mo_adpcm.mo": {},
    "mo_fastaudio.mo": {},
    "mo_pcm.mo": {},
    # The third track is decoded in the multitrack test below.
    "mo_multitrack.mo": {"maps": ("0:v", "0:a:0", "0:a:1")},
    "mods_sx.mods": {},
    "mods_adpcm.mods": {},
    "mods_fastaudio_mono.mods": {},
    "mods_fastaudio_stereo.mods": {},
    "mods_noaudio.mods": {},
    "moflex_adpcm.moflex": {},
    "moflex_noaudio.moflex": {},
}


class RetailDecodeTests(RefMixin, unittest.TestCase):
    """Container parsing and decoding of the retail cuts, pinned per frame."""

    def check_fixture(self, name, maps=("0:v?", "0:a?")):
        path = SAMPLES / name
        text, errors = report(path, maps)
        self.assertEqual(errors, "", f"{name} logged decode errors")
        self.check_ref(name + ".txt", text)

    def test_every_sample_has_a_case(self):
        present = {
            p.name for p in SAMPLES.iterdir() if p.suffix in (".mo", ".mods", ".moflex")
        }
        self.assertEqual(present, set(RETAIL))

    def test_expected_streams(self):
        # What each cut must expose, independent of the references, so a
        # reference regenerated over a demuxer regression still gets caught.
        expected = {
            "mo_vorbis.mo": ("mobiclip", "vorbis"),
            "mo_adpcm.mo": ("mobiclip", "adpcm_ima_mobiclip_wii"),
            "mo_fastaudio.mo": ("mobiclip", "fastaudio"),
            "mo_pcm.mo": ("mobiclip", "pcm_s16le"),
            "mo_multitrack.mo": ("mobiclip",) + ("adpcm_ima_mobiclip_wii",) * 3,
            "mods_sx.mods": ("mobiclip", "vx_audio"),
            "mods_adpcm.mods": ("mobiclip", "adpcm_ima_nds"),
            "mods_fastaudio_mono.mods": ("mobiclip", "fastaudio"),
            "mods_fastaudio_stereo.mods": ("mobiclip", "fastaudio"),
            "mods_noaudio.mods": ("mobiclip",),
            "moflex_adpcm.moflex": ("mobiclip", "adpcm_ima_moflex", "unknown"),
            "moflex_noaudio.moflex": ("mobiclip",),
        }
        for name, codecs in expected.items():
            with self.subTest(name=name):
                info = stream_info(SAMPLES / name)
                found = tuple(line.split(",")[2] for line in info.splitlines())
                self.assertEqual(sorted(found), sorted(codecs))

    def test_ds_video_is_converted_from_ycgco_to_rgb(self):
        # DS .mods video is YCgCo and leaves the decoder as RGB24; the Wii and
        # 3DS bitstreams are plain YCbCr and stay YUV420P.
        for name, pix_fmt in (
            ("mods_noaudio.mods", "rgb24"),
            ("mods_sx.mods", "rgb24"),
            ("mo_adpcm.mo", "yuv420p"),
            ("moflex_noaudio.moflex", "yuv420p"),
        ):
            with self.subTest(name=name):
                frame = (
                    ffprobe(
                        "-select_streams",
                        "v",
                        "-read_intervals",
                        "%+#1",
                        "-show_entries",
                        "frame=pix_fmt",
                        "-of",
                        "csv=p=0",
                        SAMPLES / name,
                    )
                    .stdout.decode()
                    .split()
                )
                self.assertEqual(frame[0], pix_fmt)

    def test_mo_multitrack_last_track_decodes(self):
        # BUG (libavformat/modec.c, multi-track branch of mo_read_packet):
        # the last track's size is computed as audio_size - 4 * num_tracks,
        # without the 1-4 chunk pad bytes that the single-track path adds back
        # (audio_padding).  Every packet of the last track is 1-4 bytes short
        # of a whole 132-byte-per-channel ADPCM block, so the decoder rejects
        # it with "invalid number of samples in packet".
        _, errors = decode(SAMPLES / "mo_multitrack.mo", ("0:a:2",))
        self.assertEqual(errors, "")

    def test_mods_key_flags_follow_the_key_frame_table(self):
        # BUG (libavformat/mods.c, end of mods_read_packet): every video
        # packet gets AV_PKT_FLAG_KEY unconditionally, so a P-frame looks like
        # a valid cut/seek point.  The key-frame table (and the .mo/.moflex
        # demuxers) say otherwise.
        for name in RETAIL:
            if not name.endswith(".mods"):
                continue
            with self.subTest(name=name):
                data = (SAMPLES / name).read_bytes()
                table = {frame for frame, _ in make_fixtures.mods_layout(data)[3]}
                lines = packet_info(SAMPLES / name, "v").splitlines()
                keys = {
                    i for i, line in enumerate(lines) if line.split(",")[6][0] == "K"
                }
                self.assertEqual(keys, table)

    def test_mo_aa_record_audio_is_exposed(self):
        # BUG (libavformat/modec.c): some Nintendo Channel clips describe
        # their Vorbis track with an 'AA' header record (same payload as 'AV':
        # the three Vorbis headers).  The demuxer skips 'AA' as unknown, so
        # the audio carried in every chunk from frame 20 on is silently
        # dropped and the file probes as video-only.
        info = stream_info(SAMPLES / "mo_vorbis_aa.mo")
        self.assertEqual(count_streams(info, "audio"), 1)

    def test_mo_header_without_end_marker_fails_instead_of_hanging(self):
        # BUG (libavformat/modec.c, mo_read_header): a file that ends exactly
        # at the declared header length but before the 'HE' record (what
        # moenc leaves behind when it fails on the first packet) makes the
        # header loop read zero-length records at EOF forever.  ffmpeg does
        # not even react to SIGTERM; the test harness has to SIGKILL it.
        data = (SAMPLES / "mo_adpcm.mo").read_bytes()
        header_end = struct.unpack_from("<I", data, 4)[0] + 8
        # Drop the HE record (the last 4 header bytes) and shrink the
        # declared header length to match.
        cut = bytearray(data[: header_end - 4])
        struct.pack_into("<I", cut, 4, header_end - 12)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "cut.mo"
            path.write_bytes(cut)
            try:
                result = ffmpeg("-i", path, "-f", "null", "-", timeout=10, check=False)
            except subprocess.TimeoutExpired:
                self.fail("demuxer hangs on a header without 'HE'")
            self.assertNotEqual(result.returncode, 0)


def _add_decode_test(name, options):
    def test(self):
        self.check_fixture(name, options.get("maps", ("0:v?", "0:a?")))

    test.__doc__ = f"decode + packet layout of samples/{name}"
    setattr(RetailDecodeTests, "test_decode_" + name.replace(".", "_"), test)


for _name, _options in RETAIL.items():
    _add_decode_test(_name, _options)


class RetailRemuxTests(unittest.TestCase):
    """Stream-copy each retail cut through the matching muxer.

    None of our three muxers can reproduce a retail file byte for byte, for
    reasons that are not bugs:

      * all three take *PCM* on their audio input and run their own audio
        encoder, so the retail audio bitstream cannot be stream-copied at all;
      * moflexenc packs packets into blocks its own way and writes a seek
        table / timeline descriptor retail files do not have;
      * modsenc always writes the N3 header layout (DS SDK files are N2).

    So what is asserted is the part that must survive a copy: the video
    packet payloads, byte for byte, and therefore the decoded pictures.
    """

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)

    def remux(self, name, *extra):
        source = SAMPLES / name
        target = self.root / ("remux" + source.suffix)
        ffmpeg("-y", "-i", source, "-map", "0:v", "-c", "copy", *extra, target)
        return source, target

    def assert_video_copied(self, name, *extra):
        source, target = self.remux(name, *extra)
        self.assertEqual(packet_payloads(source, "v"), packet_payloads(target, "v"))
        self.assertEqual(decode(source, ("0:v",))[0], decode(target, ("0:v",))[0])

    def assert_pictures_copied(self, name):
        source, target = self.remux(name)

        def pictures(path):
            frames = decode(path, ("0:v",))[0].splitlines()
            return [l.rsplit(",", 1)[-1] for l in frames if not l.startswith("#")]

        self.assertEqual(pictures(source), pictures(target))

    def test_moflex_adpcm_video_payloads_survive_remux(self):
        self.assert_video_copied("moflex_adpcm.moflex")

    def test_moflex_noaudio_video_payloads_survive_remux(self):
        self.assert_video_copied("moflex_noaudio.moflex", "-mo_block", "2048")

    def test_moflex_remux_is_idempotent(self):
        # Once a file has been through our muxer, a second copy must be
        # byte-identical: nothing about the layout may drift per generation.
        _, first = self.remux("moflex_noaudio.moflex")
        second = self.root / "second.moflex"
        ffmpeg("-y", "-i", first, "-c", "copy", second)
        self.assertEqual(first.read_bytes(), second.read_bytes())

    def test_mods_pictures_survive_remux(self):
        for name in RETAIL:
            if name.endswith(".mods"):
                with self.subTest(name=name):
                    self.assert_pictures_copied(name)

    def test_mods_video_payloads_survive_remux(self):
        # BUG (libavformat/modsenc.c + mods.c): modsenc appends its 4-byte
        # [pad][0][audio size] split suffix to every chunk, including on
        # video-only files, and the demuxer hands it back as part of the video
        # packet.  Each remux generation grows every video packet by 4 bytes
        # (14748 -> 14752 -> 14756 ...).  The decoder ignores the tail, so
        # the pictures still match (test above), but the copy is not 1:1.
        self.assert_video_copied("mods_noaudio.mods")

    def test_mods_frame_rate_survives_remux(self):
        # BUG (libavformat/modsenc.c): on a stream copy the muxer does not
        # take the frame rate from the input; every remuxed file gets
        # 0x01E00000 at 0x14, i.e. 15/8 = 1.875 fps (12.5 and 29.97 fps
        # sources alike).  The pictures survive, the timing does not.
        for name in ("mods_noaudio.mods", "mods_adpcm.mods"):
            with self.subTest(name=name):
                source, target = self.remux(name)
                rate = "-show_entries", "stream=r_frame_rate", "-of", "csv=p=0"
                self.assertEqual(
                    ffprobe("-select_streams", "v", *rate, source).stdout,
                    ffprobe("-select_streams", "v", *rate, target).stdout,
                )

    def test_mo_video_payloads_survive_remux(self):
        # Regression (libavformat/moenc.c, mo_write_packet): raw Mobiclip
        # packets from a demuxer used to go through ff_extract_mobiclip_payload()
        # and fail.  A copy must keep every video packet 1:1: payload, size,
        # timestamps and key flags, and a second generation must be
        # byte-identical to the first.
        for name in ("mo_adpcm.mo", "mo_multitrack.mo", "mo_vorbis_aa.mo"):
            with self.subTest(name=name):
                source, target = self.remux(name)
                self.assertEqual(packet_info(source, "v"), packet_info(target, "v"))
                self.assertEqual(
                    decode(source, ("0:v",))[0], decode(target, ("0:v",))[0]
                )
                second = self.root / "second.mo"
                ffmpeg("-y", "-i", target, "-c", "copy", second)
                self.assertEqual(target.read_bytes(), second.read_bytes())

    def test_compressed_audio_copy_is_rejected(self):
        # BUG (moflexenc.c / modsenc.c): the muxers treat whatever arrives on
        # the audio stream as interleaved s16 PCM and re-encode it.  A
        # "-c:a copy" of FastAudio/ADPCM/SX is accepted and produces noise
        # (e.g. FastAudio .mods in -> adpcm_ima_nds of garbage out) instead of
        # being refused as an unsupported input codec.
        for name in ("mods_fastaudio_stereo.mods", "moflex_adpcm.moflex"):
            with self.subTest(name=name):
                source = SAMPLES / name
                target = self.root / ("copy" + source.suffix)
                result = ffmpeg(
                    "-y",
                    "-i",
                    source,
                    "-map",
                    "0:v",
                    "-map",
                    "0:a",
                    "-c",
                    "copy",
                    target,
                    check=False,
                )
                self.assertNotEqual(result.returncode, 0)


# --------------------------------------------------------------------------
# Synthetic fixtures, encoded at test time.

SOURCE_FRAMES = 30
SOURCE_RATE = 32000


# The DS bitstream is YCgCo and the encoder takes the planes as they come, so
# .mods sources are converted first, exactly as encode.py does.
YCGCO = (
    "format=gbrp,geq=g='(r(X,Y)+2*g(X,Y)+b(X,Y))/4'"
    ":b='(2*g(X,Y)-r(X,Y)-b(X,Y))/4+128':r='(r(X,Y)-b(X,Y))/2+128',"
    "mergeplanes=0x000102:yuv444p,format=yuv420p"
)


def source_inputs(size, channels, ycgco=False):
    layout = "mono" if channels == 1 else "stereo"
    video = f"testsrc2=size={size}:rate=30," + (YCGCO if ycgco else "format=yuv420p")
    return [
        "-f",
        "lavfi",
        "-i",
        video,
        "-f",
        "lavfi",
        "-i",
        f"sine=frequency=440:sample_rate={SOURCE_RATE},"
        f"aformat=channel_layouts={layout},volume=0.5",
    ]


# name -> (extension, size, audio channels or 0, encoder/muxer options)
SYNTHETIC = {
    "mo_fastaudio": ("mo", "624x352", 2, ["-mobiclip", "1", "-mo_audio", "fastaudio"]),
    "mo_adpcm": ("mo", "624x352", 2, ["-mobiclip", "1", "-mo_audio", "adpcm"]),
    "mo_adpcm_mono": ("mo", "624x352", 1, ["-mobiclip", "1", "-mo_audio", "adpcm"]),
    "mo_pcm": ("mo", "624x352", 2, ["-mobiclip", "1", "-mo_audio", "pcm"]),
    "mo_pcm_mono": ("mo", "624x352", 1, ["-mobiclip", "1", "-mo_audio", "pcm"]),
    "mo_vorbis": ("mo", "624x352", 2, ["-mobiclip", "1", "-mo_audio", "vorbis"]),
    "mo_noaudio": ("mo", "624x352", 0, ["-mobiclip", "1", "-mo_audio", "none"]),
    "mods_fastaudio": ("mods", "256x192", 2, ["-mo_audio", "fastaudio"]),
    "mods_fastaudio_mono": ("mods", "256x192", 1, ["-mo_audio", "fastaudio"]),
    "mods_adpcm": ("mods", "256x192", 2, ["-mo_audio", "adpcm"]),
    "mods_pcm": ("mods", "256x192", 2, ["-mo_audio", "pcm"]),
    "mods_sx": (
        "mods",
        "256x192",
        2,
        ["-mo_audio", "codebook", "-c:a", "vx_audio", "-ltp", "0"],
    ),
    "mods_noaudio": ("mods", "256x192", 0, ["-mo_audio", "none"]),
    "moflex_fastaudio": (
        "moflex",
        "400x240",
        2,
        ["-mobiclip", "1", "-mo_audio", "fastaudio"],
    ),
    "moflex_adpcm": ("moflex", "400x240", 2, ["-mobiclip", "1", "-mo_audio", "adpcm"]),
    "moflex_pcm": ("moflex", "400x240", 2, ["-mobiclip", "1", "-mo_audio", "pcm"]),
    "moflex_noaudio": ("moflex", "400x240", 0, ["-mobiclip", "1"]),
}

MODS_VIDEO = ["-mobiclip", "2", "-moflex", "0", "-g", "100000"]

# Codecs whose decoded samples must equal the source exactly.  The .mo PCM
# cases are checked in test_mo_pcm_is_lossless.
LOSSLESS = {"mods_pcm", "moflex_pcm"}


def encode_synthetic(name, directory):
    ext, size, channels, options = SYNTHETIC[name]
    target = Path(directory) / f"{name}.{ext}"
    args = ["-y"] + source_inputs(size, max(channels, 1), ycgco=ext == "mods")
    args += ["-map", "0:v"] + (["-map", "1:a"] if channels else [])
    args += ["-frames:v", SOURCE_FRAMES, "-t", "1", "-c:v", "mobiclip", "-qp", "24"]
    if ext == "mods":
        args += MODS_VIDEO
    args += options + [target]
    ffmpeg(*args)
    return target


class SyntheticTests(RefMixin, unittest.TestCase):
    """Our encoders + muxers for every container/audio combination."""

    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.files = {}
        cls.errors = {}
        for name in SYNTHETIC:
            try:
                cls.files[name] = encode_synthetic(name, cls.directory.name)
            except AssertionError as error:
                cls.errors[name] = str(error)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def encoded(self, name):
        if name in self.errors:
            self.fail(f"encoding {name} failed:\n{self.errors[name]}")
        return self.files[name]

    def check_synthetic(self, name):
        path = self.encoded(name)
        _, _, channels, _ = SYNTHETIC[name]
        text, errors = report(path)
        self.assertEqual(errors, "", f"{name} logged decode errors")

        info = stream_info(path)
        self.assertEqual(count_streams(info, "video"), 1)
        self.assertEqual(count_streams(info, "audio"), 1 if channels else 0)
        frames = [l for l in text.splitlines() if l.startswith("0,")]
        self.assertEqual(len(frames), SOURCE_FRAMES)
        self.check_video_fidelity(path)
        if channels:
            self.check_audio_fidelity(name, path, channels)
        self.check_ref(f"syn_{name}.txt", text)

    def check_video_fidelity(self, path):
        # Encode -> decode must look like the source, not merely decode.
        ext = path.suffix
        size = {".mo": "624x352", ".mods": "256x192", ".moflex": "400x240"}[ext]
        result = run(
            [
                FFMPEG,
                "-nostdin",
                "-v",
                "info",
                "-threads",
                "1",
                "-i",
                str(path),
                "-f",
                "lavfi",
                "-i",
                f"testsrc2=size={size}:rate=30",
                "-lavfi",
                # Compare in RGB: .mo/.mods decode to RGB24, .moflex to YUV.
                "[0:v]format=rgb24[a];[1:v]format=rgb24[b];[a][b]psnr",
                "-frames:v",
                str(SOURCE_FRAMES),
                "-f",
                "null",
                "-",
            ]
        )
        line = [l for l in result.stderr.decode().splitlines() if "PSNR" in l][-1]
        average = float(line.split("average:")[1].split()[0])
        # .mods is judged more leniently: the test pattern's saturated edges
        # lose more to 4:2:0 in YCgCo than in YCbCr (flat colours round-trip
        # exactly; even qp 12 stays at ~23 dB).
        self.assertGreater(average, 20.0 if ext == ".mods" else 28.0, line)

    def source_audio(self, path, channels):
        raw = ffmpeg(
            "-f",
            "lavfi",
            "-i",
            source_inputs("16x16", channels)[-1],
            "-t",
            "1",
            "-ar",
            self.sample_rate(path),
            "-f",
            "s16le",
            "-",
        ).stdout
        return struct.unpack(f"<{len(raw) // 2}h", raw)

    def assert_lossless(self, name):
        path = self.encoded(name)
        channels = SYNTHETIC[name][2]
        self.assertEqual(self.sample_rate(path), SOURCE_RATE)
        source = self.source_audio(path, channels)
        decoded = audio_samples(path, channels)
        n = min(len(source), len(decoded))
        self.assertGreater(n, 0.9 * len(source))
        if decoded[:n] != source[:n]:
            first = next(i for i in range(n) if decoded[i] != source[i])
            self.fail(f"PCM differs from the source at sample {first // channels}")

    def check_audio_fidelity(self, name, path, channels):
        decoded = audio_samples(path, channels)
        source = self.source_audio(path, channels)
        seconds = len(decoded) / channels / self.sample_rate(path)
        self.assertGreater(seconds, 0.9, f"only {seconds:.3f}s of audio decoded")
        self.assertLess(seconds, 1.2, f"{seconds:.3f}s of audio decoded")
        if name in LOSSLESS:
            self.assert_lossless(name)
            return

        # Lossy: the tone has to come back at roughly the same level.
        def rms(values):
            return math.sqrt(sum(v * v for v in values) / max(len(values), 1))

        ratio = rms(decoded) / rms(source)
        self.assertGreater(ratio, 0.5, f"level ratio {ratio:.2f}")
        self.assertLess(ratio, 2.0, f"level ratio {ratio:.2f}")

    def test_mo_pcm_is_lossless(self):
        # BUG (libavformat/moenc.c, PCM path): the first chunk carries only
        # the first 1024 input samples and is then filled up to its sample
        # budget with ~40 zero samples; the second chunk continues at input
        # sample 1024.  So ~1.3 ms of silence is inserted after the first
        # 1024 samples and everything after it is delayed (mono and stereo).
        # .mods and .moflex PCM round-trip bit-exactly.
        for name in ("mo_pcm", "mo_pcm_mono"):
            with self.subTest(name=name):
                self.assert_lossless(name)

    @staticmethod
    def sample_rate(path):
        info = ffprobe(
            "-select_streams",
            "a:0",
            "-show_entries",
            "stream=sample_rate",
            "-of",
            "csv=p=0",
            path,
        ).stdout.decode()
        return int(info.split()[0])


def _add_synthetic_test(name):
    def test(self):
        self.check_synthetic(name)

    test.__doc__ = f"encode + decode round trip: {name}"
    setattr(SyntheticTests, "test_" + name, test)


for _name in SYNTHETIC:
    _add_synthetic_test(_name)


# --------------------------------------------------------------------------
# Optional larger retail files.

EXTERNAL = os.environ.get("MOBIPEG_SAMPLES")
EXTERNAL_REF = REF / "external.json"


@unittest.skipUnless(EXTERNAL, "set MOBIPEG_SAMPLES to a directory of retail files")
class ExternalSampleTests(unittest.TestCase):
    """Whole retail files kept outside the tree; only checksums are committed.

    ref/external.json maps each file's path relative to $MOBIPEG_SAMPLES to
    the md5 of its packet layout and of its per-frame decode.  Files without
    an entry are skipped until --update-refs records them.
    """

    def test_external_samples(self):
        base = Path(EXTERNAL)
        files = sorted(
            p
            for p in base.rglob("*")
            if p.suffix.lower() in (".mo", ".mods", ".moflex")
        )
        if not files:
            self.skipTest(f"no .mo/.mods/.moflex files under {base}")
        refs = json.loads(EXTERNAL_REF.read_text()) if EXTERNAL_REF.exists() else {}
        for path in files:
            key = path.relative_to(base).as_posix()
            with self.subTest(file=key):
                frames, errors = decode(path)
                current = {
                    "packets": hashlib.md5(packet_info(path).encode()).hexdigest(),
                    "decode": hashlib.md5(frames.encode()).hexdigest(),
                    "decode_errors": bool(errors),
                }
                if updating():
                    refs[key] = current
                elif key not in refs:
                    self.skipTest(f"{key} has no reference; run with --update-refs")
                else:
                    self.assertEqual(current, refs[key])
        if updating():
            REF.mkdir(exist_ok=True)
            EXTERNAL_REF.write_text(json.dumps(refs, indent=2, sort_keys=True) + "\n")


if __name__ == "__main__":
    if "--update-refs" in sys.argv:
        sys.argv.remove("--update-refs")
        os.environ["MOBIPEG_UPDATE_REFS"] = "1"
    unittest.main()
