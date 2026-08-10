#!/usr/bin/env python3
"""Caimans Pro audio decoder — reference implementation.

Direct transcription of the ARM routine at IWRAM 0x03004a98 in the Caimans
Pro player (Pooh's Heffalump Movie sample), recovered with Ghidra. See
doc/caimans_handoff.md for how the player image maps from ROM to IWRAM.

It is IMA ADPCM with two deviations from the standard formulation:

  1. An extra `diff += step >> 1` term when the low 3 magnitude bits are all
     set ((n & 7) == 7).  Standard IMA has no such term.
  2. An index-adjust table of {-1,-1,-1,-1,2,4,7,12} rather than FFmpeg's
     canonical ff_adpcm_index_table {-1,-1,-1,-1,2,4,6,8}.

The 89-entry step table is byte-identical to ff_adpcm_step_table.

Output is signed 8-bit PCM: the routine stores only the high byte of the
16-bit predictor (`*dst = pred >> 8`), matching the GBA's 8-bit DMA audio.

NOTE: this reproduces the sample-decoding kernel only. Block/packet framing,
channel count and how the codec state is initialised per stream are still
unknown, so this cannot yet be pointed at a raw file. It is here to be
validated against captured state once framing is understood.
"""

# Byte-identical to ff_adpcm_step_table; read from IWRAM 0x030004d8.
STEP_TABLE = [
        7,     8,     9,    10,    11,    12,    13,    14,    16,    17,
       19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
       50,    55,    60,    66,    73,    80,    88,    97,   107,   118,
      130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
      337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
      876,   963,  1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
     2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
     5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
]

# Read from IWRAM 0x0300058a. Only the low 8 entries are reachable: the
# routine indexes with (nibble & 7), not (nibble & 15).
INDEX_TABLE = [-1, -1, -1, -1, 2, 4, 7, 12]


def decode(src, nsamples, predictor=0, step_index=0):
    """Decode `nsamples` 4-bit samples from `src` to signed 8-bit PCM.

    Returns (pcm_bytes, predictor, step_index) so the caller can carry codec
    state across calls, exactly as the ARM routine does via its 2-word state
    struct at IWRAM 0x03007050.

    `nsamples` is expected to be even. The original keys nibble selection off
    the parity of the remaining-sample countdown, so an odd count would make
    the first sample read a stale/zero byte latch rather than fetching one.
    """
    out = bytearray()
    src_pos = 0
    latch = 0
    remaining = nsamples

    while remaining != 0:
        # Clamp step index into [0, 88].
        if step_index < 0:
            step_index = 0
        elif step_index > 0x57:
            step_index = 0x58

        step = STEP_TABLE[step_index]

        # Low nibble on even countdown (fetching a fresh byte), high nibble
        # on odd countdown (reusing the latched byte).
        if remaining % 2 == 0:
            latch = src[src_pos]
            src_pos += 1
            nibble = latch & 0xF
        else:
            nibble = (latch >> 4) & 0xF

        mag = nibble & 7

        diff = step >> 3
        if mag & 1:
            diff += step >> 2
        if mag & 2:
            diff += step >> 1
        if mag & 4:
            diff += step
        if mag == 7:                 # deviation (1) — absent from standard IMA
            diff += step >> 1
        if nibble & 8:
            diff = -diff

        predictor += diff
        if predictor < -0x8000:
            predictor = -0x8000
        elif predictor > 0x7FFE:
            predictor = 0x7FFF

        step_index += INDEX_TABLE[mag]

        # Store the high byte only: signed 8-bit PCM.
        out.append((predictor >> 8) & 0xFF)

        remaining -= 1

    return bytes(out), predictor, step_index


if __name__ == "__main__":
    assert len(STEP_TABLE) == 89, len(STEP_TABLE)
    pcm, pred, idx = decode(bytes(range(64)), 64)
    print(f"decoded {len(pcm)} samples; final predictor={pred} step_index={idx}")
    print("first 32 samples:", list(pcm[:32]))
