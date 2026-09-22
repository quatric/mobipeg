# Mobiclip retail sample cuts

Short cuts of retail Mobiclip files, used by `../test_mobiclip.py`.  Each
one was produced from the original with

    python3 tests/mobiclip/make_fixtures.py NAME SOURCE

which copies the kept chunks/blocks byte for byte and only rewrites the
header fields that describe the file length (see the docstring of
`make_fixtures.py` for the exact fields per container).  They were *not*
remuxed through our muxers: none of them can reproduce a retail layout, and
the point is to keep the retail bitstream.  "Source sha256" identifies the
full original file the cut was taken from.

| Fixture | Size | Source file | Origin | Container / codecs | Cut |
|---|---|---|---|---|---|
| `mo_vorbis.mo` | 138216 | `GoldenEye_007-E3_Trailer_2035953581-h.mo` (sha256 `606c34d6df95277b…`) | Wii Nintendo Channel video (GoldenEye 007 E3 trailer) | MO, Mobiclip 384x288 @ 29.97, Vorbis 48 kHz stereo (`AV` record) | first 60 of 2835 chunks |
| `mo_vorbis_aa.mo` | 42604 | `New_Super_Mario_Bros._Wii-Challenge_2_1501121898-h.mo` (sha256 `3918b94469eef490…`) | Wii Nintendo Channel video (New Super Mario Bros. Wii challenge) | MO, Mobiclip 384x288, Vorbis declared by an `AA` record (audio from chunk 20 on) | first 30 of 6653 chunks |
| `mo_adpcm.mo` | 46080 | `testE.mo` (sha256 `0712a747a306a394…`) | Wii Internet Channel (USA, v1) intro movie | MO, Mobiclip 624x352, IMA-ADPCM (Wii) 48 kHz stereo (`A9`) | first 12 of 7136 chunks |
| `mo_fastaudio.mo` | 84528 | `CBR2Mbps.mo` (sha256 `a93bc7e430548605…`) | Mobiclip SDK 3.3.0 sample (`dvddata/content2`) | MO, Mobiclip 640x480, FastAudio 32 kHz stereo (`A3`) | first 10 of 1647 chunks |
| `mo_pcm.mo` | 32092 | `snr_cE4_1300.mo` (sha256 `4f08e0760a371943…`) | Wii game cutscene (origin not recorded; the `AM` single-track PCM layout matches the Pandora's Tower case described in `modec.c`) | MO, Mobiclip 640x480, PCM s16le 32 kHz stereo in a one-track `AM` record | first 6 of 11989 chunks |
| `mo_multitrack.mo` | 167156 | `Drive_Multitrack.mo` (sha256 `a6e5772b5d3efb0e…`) | Mobiclip SDK 3.3.0 sample (`dvddata`) | MO, Mobiclip 640x480 @ 24, three IMA-ADPCM (Wii) 32 kHz stereo tracks (`AM`) | first 3 of 1360 chunks |
| `mods_sx.mods` | 40980 | `movie_16.mods` (sha256 `773b391c3c501f83…`) | DS game *Family Cooke* (BKCE), `Datafiles/actimg/` | MODS N3, Mobiclip 256x192 (YCgCo), FastAudio-codebook / SX 22050 Hz stereo | first 35 of 692 frames (key frames 0, 8, 9) + SX codebooks + key-frame table |
| `mods_adpcm.mods` | 82672 | `NintendoDSi.mods` (sha256 `cdd0a6beb8ba03aa…`) | Nintendo DSi system movie (Nintendo DSi + Internet, USA) | MODS N3, Mobiclip 256x192, IMA-ADPCM 22050 Hz stereo interleaved in the video chunks (the "MODS split") | first 60 of 5073 frames (key frames 0, 30: covers the ADPCM re-prime on a key frame) |
| `mods_fastaudio_mono.mods` | 78688 | `DS_up.mods` (sha256 `5153827d4b27da77…`) | TWL libMobiclip 1.2.2 SDK demo data | MODS N2, Mobiclip 256x192 @ 12.5, FastAudio 16 kHz mono | first 30 of 735 frames |
| `mods_fastaudio_stereo.mods` | 44652 | `DS_stereo.mods` (sha256 `d4742f027ebce89a…`) | TWL libMobiclip 1.2.2 SDK demo data | MODS N2, Mobiclip 256x192 @ 25, FastAudio 16 kHz stereo | first 40 of 2631 frames |
| `mods_noaudio.mods` | 67020 | `DS_down.mods` (sha256 `724dcfe75a6dc4c4…`) | TWL libMobiclip 1.2.2 SDK demo data | MODS N2, Mobiclip 256x192 @ 12.5, no audio | first 25 of 735 frames |
| `moflex_adpcm.moflex` | 98304 | `ESJ_MD4.2011-10-26.1.0.boss.moflex` (sha256 `5ceeefa22dd9fb72…`) | 3DS Nintendo eShop SpotPass (BOSS) video | MOFLEX, 4096-byte blocks, Mobiclip 400x240, IMA-ADPCM (moflex) 48 kHz stereo, timeline data stream | first 24 blocks |
| `moflex_noaudio.moflex` | 81920 | `Jimmy_ed.moflex` (sha256 `b021bb7261754bf9…`) | 3DS game *WarioWare Gold* (CTR-P-AWXA, v2.3.0), `romfs/Demo/Jimmy/` | MOFLEX, 2048-byte blocks, Mobiclip 400x240 @ 29.97, no audio | first 40 blocks |

A `.moflex` cut ends inside a packet; the demuxer flags that last packet
corrupt and the tests decode with `-fflags +discardcorrupt`.

## Combinations without a retail sample

No retail file on hand carries these, so they are covered only by the
synthetic encodes in `test_mobiclip.py`:

* MODS with PCM audio
* MOFLEX with FastAudio or PCM audio
* MO with mono IMA-ADPCM or mono PCM

Not covered at all: stereoscopic (two-video) MOFLEX and `.mods` with H.264
video (no retail sample found, and not part of the synthetic set).

Larger retail files can be tested without committing them: point
`MOBIPEG_SAMPLES` at a directory of `.mo`/`.mods`/`.moflex` files and run
the suite with `--update-refs` once to record their checksums in
`../ref/external.json`.
