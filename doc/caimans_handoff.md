# Caimans GBA Video — handoff

State as of 2026-08-09. This is the next non-retail GBA video family to
reverse after the five Game Boy Advance Video lineages. It is a separate
commercial codec, not an ADS, Hydrogen, VX, or FVMV variant. No Caimans
demuxer or decoder has been added to FFmpeg yet.

## Repository state

- Repository: `/Volumes/SSD/dlz/Folders/mobipeg`
- Branch: `master`
- Current commit: `c1b360dcb14` (`Revert "avformat/moflex: default mo_block to 2048"`)
- The GBA Video GUI listing was committed earlier as `d78a14d24ac` and is an
  ancestor of the current branch.
- Preserve unrelated dirty files: `checkasm_config_generated.h`,
  `checkasm_header_config_generated.h`, `mobipeg-gui.spec`, and the untracked
  `tools/mobiclip/` material.

## Reference ROMs

Inputs are intentionally untracked in `build_caimans/`; re-fetch them from the
publisher rather than committing them.

| generation | sample | local ROM | SHA-256 | publisher archive |
|---|---|---|---|---|
| Caimans 2.2 | Bad Boys 2 trailer | `build_caimans/roms/caimans_badboys2.gba` | `7f6b2a01cb0ba2328ef01bfb067b9b75c021f7db081cc450cae69c0966a39a1d` | `https://www.caimans.net/gbavideo/caimans_badboys2_hi.zip` |
| Caimans Pro | Pooh's Heffalump Movie trailer | `build_caimans/roms/caimans_pro_pooh_hq.gba` | `e81ed048fe1c07e9dd00cf9d0814808dea5386b2861fdbb3ae728fbc551679f4` | `https://www.caimans.net/gbavideo/caimans_pro_pooh_hq.zip` |

The archives were downloaded directly from the publisher's demo pages:

- `https://www.caimans.net/gbavideo/demos2.shtml` (version 2.2)
- `https://www.caimans.net/gbavideo/demos.shtml` (Pro)
- `https://www.caimans.net/gbavideo/tech.shtml` (publisher's technical claims)

## What is established

- The two samples are distinct player/data generations. Do not build a shared
  parser merely because both are Caimans.
- Version 2.2's ROM has no ordinary title string in the GBA header; Pro uses
  the title `CAIMANSH` at ROM `0xA0`.
- Both boot from Thumb code at ROM `0x08000100`.
- In the Pro sample, after startup the IWRAM trampoline at `0x03000000` is ARM
  `LDR r0,[pc]; BX r0` and transfers to `0x08000219` (Thumb). This gives a
  stable debugger waypoint and confirms that the player is not one of the
  existing cartridge decoder images.
- The publisher describes version 2.x as 24-bit internal colour with runtime
  dithering and proprietary compressed audio. It describes Pro as its
  low-bitrate successor. Independent format notes describe the older family
  as 4x4-codebook YUV and Pro as recursive 8x8/8x4/4x4/4x2 blocks with motion
  compensation. Treat the latter as a hypothesis to verify against the ARM
  routines, not as a decoder specification.

## Useful commands

```sh
# Fetch and unpack the two baseline samples.
mkdir -p build_caimans/roms
curl -L --fail -o build_caimans/caimans_badboys2_hi.zip \
  https://www.caimans.net/gbavideo/caimans_badboys2_hi.zip
curl -L --fail -o build_caimans/caimans_pro_pooh_hq.zip \
  https://www.caimans.net/gbavideo/caimans_pro_pooh_hq.zip
unzip -jo build_caimans/caimans_badboys2_hi.zip -d build_caimans/roms
unzip -jo build_caimans/caimans_pro_pooh_hq.zip -d build_caimans/roms

# Start mGBA paused with its GDB remote stub.
/Applications/mGBA.app/Contents/MacOS/mGBA -g \
  build_caimans/roms/caimans_pro_pooh_hq.gba
```

`tools/gba_video/gdbrsp.py` is the small client already used for the VX work.
Set an execution breakpoint at `0x03000000`, continue from reset, and dump
registers/IWRAM after the breakpoint. mGBA reported PC `0x03000000` and CPSR
`0x20000092` at that waypoint.

## Recommended next work

1. Build a ROM scanner that separates executable/player regions from the video
   payload and finds any stream table or frame index. Begin with the small Pro
   sample, then compare the equivalent structures in the 2.2 sample.
2. Use mGBA GDB breakpoints around the display/DMA path to capture the decoded
   framebuffer and the input pointer for several frames. This supplies an
   exact reference before translating the codec.
3. Identify the player call that advances one frame, then make a Python
   reference runner (Unicorn or mGBA-assisted) before adding a demuxer.
4. Keep version 2.2 and Pro as separate targets until their framing and state
   layout demonstrably coincide. Add more official samples only after the
   first packet boundary tiles exactly.

## Scope boundary

The current FFmpeg tree supports all five known retail GBA Video lineages:
ADS, Hydrogen (including its LZMA prototype revision), VXGB, VX++, and FVMV.
Caimans and METEO/Avi2GBA are separate aftermarket/commercial GBA video
families and should not be folded into the retail GBA Video demuxers.
