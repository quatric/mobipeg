"""Cut a retail Mobiclip file down to a small test fixture.

    python3 tests/mobiclip/make_fixtures.py NAME SOURCE [--frames N | --blocks N]

NAME is one of the entries in FIXTURES below; the output goes to
tests/mobiclip/samples/NAME.<ext>.  The cut is done on the container bytes
directly instead of through our muxers, because the point of the fixtures is
to be a faithful retail bitstream: every video/audio chunk that is kept is
copied verbatim, and only the header fields that describe the length of the
file are rewritten.

  .mo      Keep the header verbatim and the first N chunks (each followed by
           its 1-4 alignment bytes).  The TL (length)
           record's frame count is patched to N.  The KI key-frame index,
           the 'pc' signature and everything else stay untouched (KI entries
           past the end are ignored by the demuxer).
  .mods    Keep the 0x30/0x34-byte header and the first N chunks, then
           re-append the tail the demuxer needs: the audio-codec-info block
           (SX codebooks) if present, and the key-frame table limited to the
           frames that were kept.  The frame count (0x08), audio-codec-info
           offset (0x24), key-frame table offset (0x28) and key-frame count
           (0x2C) are patched to match.
  .moflex  Keep the first N fixed-size blocks unmodified.  MOFLEX is a
           streaming format with no index, so no field needs patching; the
           last packet that straddles the cut is simply cut short, exactly as
           when a stream is interrupted.
"""

from pathlib import Path
import argparse
import hashlib
import struct
import sys

HERE = Path(__file__).resolve().parent

# name -> (container, default length).  Lengths are chunk counts for .mo and
# .mods and block counts for .moflex, chosen to keep each file small while
# still spanning at least one key frame after the first where the source
# allows it.
FIXTURES = {
    "mo_vorbis": ("mo", 60),
    "mo_adpcm": ("mo", 12),
    "mo_fastaudio": ("mo", 10),
    "mo_pcm": ("mo", 6),
    "mo_multitrack": ("mo", 3),
    "mo_vorbis_aa": ("mo", 30),
    "mods_sx": ("mods", 35),
    "mods_adpcm": ("mods", 60),
    "mods_fastaudio_mono": ("mods", 30),
    "mods_fastaudio_stereo": ("mods", 40),
    "mods_noaudio": ("mods", 25),
    "moflex_adpcm": ("moflex", 24),
    "moflex_noaudio": ("moflex", 40),
}


def mo_header_records(data):
    header_end = struct.unpack_from("<I", data, 4)[0] + 8
    pos = 8
    records = []
    while pos < header_end:
        marker, length = struct.unpack_from("<2sH", data, pos)
        records.append((marker, pos, length * 4))
        pos += 4 + length * 4
        if marker == b"HE":
            break
    return records, header_end


def trim_mo(data, frames):
    records, header_end = mo_header_records(data)
    tl = [pos for marker, pos, _ in records if marker == b"TL"]
    if not tl:
        raise SystemExit("no TL record")
    total = struct.unpack_from("<I", data, tl[0] + 8)[0]
    if frames > total:
        raise SystemExit(f"source only has {total} frames")
    pos = header_end
    for _ in range(frames):
        chunk_size = struct.unpack_from("<I", data, pos)[0]
        # Chunks are always followed by 1-4 pad bytes, even when already
        # 4-byte aligned.
        end = pos + chunk_size
        pos = end + 4 - end % 4
    out = bytearray(data[:pos])
    struct.pack_into("<I", out, tl[0] + 8, frames)
    return bytes(out)


def mods_layout(data):
    (frames,) = struct.unpack_from("<I", data, 0x08)
    acinfo, kf_off, kf_count = struct.unpack_from("<III", data, 0x24)
    table = [struct.unpack_from("<II", data, kf_off + 8 * i) for i in range(kf_count)]
    return frames, acinfo, kf_off, table


def mods_chunk_offsets(data, first, count):
    offsets = [first]
    pos = first
    for _ in range(count):
        (word,) = struct.unpack_from("<I", data, pos)
        pos += 4 + (word >> 14)
        offsets.append(pos)
    return offsets


def trim_mods(data, frames):
    total, acinfo, kf_off, table = mods_layout(data)
    if frames > total:
        raise SystemExit(f"source only has {total} frames")
    offsets = mods_chunk_offsets(data, table[0][1], total)
    frames_end = acinfo if acinfo else kf_off
    if offsets[-1] != frames_end:
        raise SystemExit(
            f"chunk walk ends at {offsets[-1]:#x}, expected {frames_end:#x}"
        )
    for frame, offset in table:
        if offsets[frame] != offset:
            raise SystemExit(f"key frame {frame} offset mismatch")
    cut = offsets[frames]
    codebooks = data[acinfo:kf_off] if acinfo else b""
    kept = [(f, o) for f, o in table if f < frames]
    out = bytearray(data[:cut])
    struct.pack_into("<I", out, 0x08, frames)
    struct.pack_into(
        "<III", out, 0x24, cut if acinfo else 0, cut + len(codebooks), len(kept)
    )
    out += codebooks
    for frame, offset in kept:
        out += struct.pack("<II", frame, offset)
    return bytes(out)


def moflex_block_size(data):
    if data[:4] != b"L2\xaa\xab":
        raise SystemExit("not a moflex file")
    return struct.unpack_from(">H", data, 12)[0] + 1


def trim_moflex(data, blocks):
    size = moflex_block_size(data)
    if blocks * size > len(data):
        raise SystemExit("source is shorter than requested")
    return data[: blocks * size]


TRIMMERS = {"mo": trim_mo, "mods": trim_mods, "moflex": trim_moflex}


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("name", choices=sorted(FIXTURES))
    parser.add_argument("source", type=Path)
    parser.add_argument(
        "--length", type=int, help="chunks (mo/mods) or blocks (moflex)"
    )
    args = parser.parse_args()
    container, length = FIXTURES[args.name]
    data = args.source.read_bytes()
    out = TRIMMERS[container](data, args.length or length)
    target = HERE / "samples" / f"{args.name}.{container}"
    target.parent.mkdir(exist_ok=True)
    target.write_bytes(out)
    print(
        f"{target.name}: {len(out)} bytes from {args.source.name} "
        f"({len(data)} bytes, sha256 {hashlib.sha256(data).hexdigest()[:16]}...)"
    )


if __name__ == "__main__":
    sys.exit(main())
