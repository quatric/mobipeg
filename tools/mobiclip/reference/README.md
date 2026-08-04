# Driving the reference MODS encoder

`MODS_Encoder_v43_2` (Helwettpackardenterprise) is a translation of the retail
Mobiclip VfW encoder, verified against the original DLL under Unicorn. Running
it is the only route to output that is byte-identical with retail Mobiclip —
the x264-based encoder in quatric/x264 makes its own mode and rate decisions
and can only ever be *decoder*-exact, never *encoder*-exact.

`mods_ref_enc.c` drives it over a raw yuv420p sequence. Four host constraints
had to be solved to get it running on macOS/Apple Silicon, all of which apply
to any non-Windows host:

1. **x86 only.** The rate-control model uses x87 80-bit arithmetic and x86
   inline asm, guarded by `#if defined(__i386__) || defined(__x86_64__)`;
   everything else is `#error`. Build with `-arch x86_64` and run under Rosetta.

2. **`MODS_ABI32_LAYOUT` is required, not optional.** Without it the build mixes
   32-bit context slots with 64-bit pointers and crashes in `mods_eval_mode6`
   reading an address assembled from two adjacent 32-bit pointers.

3. **All encoder memory must live below 4 GB**, because `mods_load_abi32_pointer`
   truncates every stored pointer to 32 bits. macOS reserves the low 4 GB of an
   x86_64 process as `__PAGEZERO`, so link with `-Wl,-pagezero_size,0x1000` and
   serve every allocation from one low arena. (This is also why the shipped
   Python test suite cannot run here: system `python3` has the full 4 GB
   `__PAGEZERO`, so its `low_buffer()` always fails.)

4. **The original DLL image must be mapped at its own base.** The encoder reads
   constant tables that live in the DLL's data section (`ctx+0x344` resolves to
   ~`0x1009E1C8`). Map `reference/controlled/virtualdubmod_10000000.bin` at
   `0x10000000` and the address resolver can stay an identity function.

## Build and run

```sh
R=/path/to/MODS_Encoder_v43_2_Internal_Handover
clang -arch x86_64 -std=c11 -O2 -DMODS_ABI32_LAYOUT -Wl,-pagezero_size,0x1000 \
  -o mods_ref_enc mods_ref_enc.c \
  $R/mods_encoder.c $R/mods_vfw_builtin_data.c $R/mods_codec_tables.c \
  $R/tests/vlc_tables.c $R/compat_heap/mods_win32_shadow_heap.c

arch -x86_64 ./mods_ref_enc in.yuv 256 192 60 24 30 out.bin \
  $R/reference/controlled/virtualdubmod_10000000.bin
```

Arguments are `in.yuv w h frames qp keyint out.bin [dll_image] [yuvmode]`.
QP is passed to the retail `encoder.cq` policy as `Quantizer = qp * 100`, with
`IBoostPercent=40` / `IThreshold=90` (the retail defaults). YuvMode 0 (the
default) applies the YCgCo transform, as retail content does; 1 is the
lossless planar-copy path.

## Output framing

Frames are written as `u32 size, u8 keyframe, payload`. **The payload's first
four bytes are a length prefix that is not part of the Mobiclip frame** — strip
them before handing the frame to a decoder or muxer. With them stripped,
`decode_ref.c` decodes 60/60 frames of a test clip with no errors, which is
what confirms the encoder is producing real Mobiclip.

## Status

Running and decodable. Not yet wired into the mobipeg encode path, and not yet
compared against a retail file encoded with known settings, which is what an
actual byte-for-byte claim would need.
