# Mobiclip encoder bit-exactness check

The Mobiclip encoder predicts from its own reconstructed frames. If that
reconstruction differs from what the decoder produces, the error is fed into
every following P-frame and drifts through the GOP, so "it looks fine" is not
evidence of correctness — the two have to be identical.

`reconcheck.c` encodes a raw yuv420p sequence while pulling the encoder's own
reconstructed frames (`AV_CODEC_FLAG_RECON_FRAME`) and writes them out.
`bitexact.sh` runs the same encode through the muxer, decodes it, and compares
the two byte for byte across QPs, transform sizes, table sets and clips.

Build (adjust the paths to your tree):

```sh
gcc -O2 -o reconcheck reconcheck.c -I../.. \
    -L../../libavcodec -L../../libavutil -L../../libswresample \
    -L../../libavformat -L../../libswscale \
    -lavcodec -lavutil -lswresample -lswscale -lavformat \
    -L"$HOME/Downloads/x264-install/lib" -lx264 -lz -lm -lbz2 -liconv -llzma \
    -framework CoreFoundation -framework CoreMedia -framework CoreVideo \
    -framework VideoToolbox -framework AudioToolbox -framework CoreAudio \
    -framework CoreServices -framework Security
```

`bitexact.sh` expects `src.yuv` (256x192), `src2.yuv` (320x240),
`src3.yuv` (384x224) and `eb.yuv` (384x288) beside it; any yuv420p rawvideo
of those sizes works. Produce them with

```sh
ffmpeg -i clip.mp4 -t 2 -vf scale=256:192 -pix_fmt yuv420p -f rawvideo src.yuv
```

Both sides must be configured identically — `reconcheck` pins `-g 30` and
30 fps, so the `ffmpeg` half passes `-g 30 -r 30`. A mismatch there shows up
as every case failing, which is a harness bug, not an encoder bug.

Extra encoder AVOptions can be appended as `key=value` arguments to mirror a
particular command line, e.g. `partitions=i4x4,i8x8,p8x8`.

## Known non-exact configuration

Sub-8x8 inter partitions (`p4x4`, i.e. 8x4/4x8/4x4) still produce occasional
macroblocks the decoder cannot reproduce. They are disabled by default;
`MOBI_PSUB8x8=1` re-enables them for diagnosis. 16x8/8x16/8x8 (`p8x8`) is
exact and is on by default.
