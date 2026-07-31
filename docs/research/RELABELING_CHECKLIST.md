# Relabeling Checklist

User-facing items to rename. Internal codec identifiers, muxer names, library
symbols, and format identifiers (`mobiclip`, `moflex`, `x264_param_t` fields, etc.)
are explicitly NOT renamed — only what a user or contributor actually sees.

- [ ] Repository name (`mobipeg-3ds-public` working name, or the final chosen public name)
- [ ] README title and subtitle ("Mobipeg 3DS" / "A dedicated Nintendo 3DS MOFLEX encoder")
- [ ] GUI window title / application name
- [ ] Executable / package name (currently would build as `mobipeg-gui.exe`-equivalent; rename to a `mobipeg3ds`-branded binary)
- [ ] CLI help heading (`mobipeg3ds --help` banner)
- [ ] About dialog text
- [ ] Documentation title(s) under `docs/`
- [ ] Release archive names (e.g. `mobipeg3ds-windows-x86_64.zip`)
- [ ] GitHub release titles
- [ ] Screenshot branding (once real screenshots exist)
- [ ] Any "About"/version string embedded at build time

Not renamed, deliberately:
- `mobiclip` encoder name, `moflex` muxer/format name — these are the real format's
  actual names, changing them would misrepresent the file format to anyone else
  working with it.
- Internal x264/FFmpeg symbols, function names, table names.
- Upstream credit files (`CREDITS_MOBICLIP.md`) — content stays attributed to its
  real authors.

Nintendo ownership/endorsement: the product must not imply either. The proposed
README opening explicitly disclaims this ("not affiliated with or endorsed by
Nintendo") — keep that disclaimer prominent, not buried.
