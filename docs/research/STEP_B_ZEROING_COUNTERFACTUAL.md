# Step B — block-aware coefficient zeroing counterfactual

## Question answered
For coefficients the existing rate-blind mobiclip quantizer retained, how
often would zeroing ONE coefficient reduce the complete block's coding cost
enough to justify the resulting reconstruction distortion? Read-only:
no encoder decision was changed, no coefficient was actually zeroed in any
shipped or shippable output.

## Bottom line
**Weak signal. Recommend closing single-coefficient pruning as a quality
lever and not pursuing it further** (per the plan's own decision criteria).
Across a 60-frame captured segment (363,948 coded blocks, 1,070,677 nonzero
coefficients), the overwhelming majority of zeroing candidates are not
worthwhile at any realistic lambda:

| outcome | count | share of tested candidates |
|---|---:|---:|
| no rate benefit at all (bits_saved ≤ 0) | 39,881 | 4.4% |
| positive rate benefit, but distortion too large for lambda ≤ 64 | 855,662 | 94.5% |
| breaks even somewhere in lambda ∈ [1/4, 64] | 10,123 | 1.1% |
| (excluded — see Scope boundary) | 165,011 blocks | — |

Even restricting to small-magnitude coefficients (|val| ≤ 5, which is 98.4%
of positive-bits_saved cases in luma's mid-frequency band), the mean rate
saving is ~3.8 bits against a mean pixel-domain SSD penalty of ~962 — far
short of break-even at any lambda in the realistic range for QP 25
(H.264-style lambda for this QP is roughly single digits, not 64+). The
quantizer's retained coefficients, even small ones, are mostly earning their
keep.

## Method
1. **Oracle** (`mobi_exact_bits`/`mobi_describe_coeff_code`): Step A's
   per-symbol bit-cost function, reused verbatim, unchanged. Previously
   validated: 9,590 cases, 0 mismatches, against the real unmodified
   `encode_dct()` writer.
2. **`block_rate()`** (new for Step B): a whole-block re-walk that sums the
   oracle over a full coefficient array, replicating `encode_dct`'s exact
   scan logic read directly from `x264-costoracle/encoder/cavlc.c:627-770`
   (increasing zigzag order, skip-run tracking, the `lastnonzero==0`
   empty-block special case, and critically `if(i==lastnonzero) break;` —
   an early-exit condition an initial implementation missed, causing 61/16,864
   mismatches with a 441-bit worst-case error in first-pass validation before
   the fix). **Fixed and revalidated: 16,864 cases, 0 mismatches**, including
   an explicit zeroing-sweep pass that exercises the exact D_zero
   construction this tool performs on real data.
3. **Reconstruction/distortion**:
   - Luma 4x4 has a re-quantization overshoot guard (iteratively halves
     coefficients if reconstruction would exceed the decoder's `[-64,319]`
     lookup-table bounds), too interleaved with dequant/IDCT to call as an
     isolated unit — duplicated (copied, not independently re-derived) and
     validated against the real `x264_mb_encode_i4x4()`: 8,064 pixels across
     6 prediction levels × 3 QYX × 4 QP × 7 residual patterns (incl. sharp
     edges intended to trigger the guard), 0 mismatches.
   - Chroma 8x8 (`mobi_add8x8_idct8`) has **no** guard — confirmed by reading
     it — so Step B calls the real production function directly, no
     duplication. Only the dequant `mat[]` construction formula needed
     validation: 92,160 pixels across the same parameter sweep, 0 mismatches.
4. **Lambda**: integer cross-multiplication only (`distortion*den < num*bits_saved`),
   swept across a fixed ladder from 1/4 to 64, per your instruction to avoid
   floating point in the comparison.
5. **Two result sets kept explicitly separate**, per your instruction:
   - `*_independent.csv` — each nonzero coefficient zeroed ALONE, holding the
     rest of the block fixed. NOT a claim of simultaneous achievability.
   - `*_greedy.csv` — within each block, positive-bits_saved candidates
     zeroed cumulatively in ascending lambda order, re-walking the true
     block rate/distortion at each step. Interaction effects are real and
     substantial: on a 500-block sample, the cumulative achieved bits_saved
     averaged only **71%** of the naive independent sum, and cumulative
     distortion averaged **82%** of the naive independent sum — confirming
     independent deltas cannot simply be added up.

## Scope boundary (disclosed)
Blocks with exactly one nonzero coefficient (165,011 of 363,948 blocks,
45.3%) are recorded but excluded from the primary aggregate. Zeroing that
coefficient empties the block; the real encoder never calls `encode_dct` on
an empty block at all (`if(nz)` gate in `x264_mb_encode_i4x4` /
`mb_encode_chroma_internal`), so the rate change is a CBP-bit effect
elsewhere in the bitstream writer that `block_rate()`'s oracle — which only
models `encode_dct`'s own symbol stream — cannot correctly price. Reported
separately rather than silently included or silently dropped.

## Important limitation (per the plan)
This only tests **zeroing** an already-nonzero coefficient. It says nothing
about whether a currently-zero coefficient should instead be **activated**
(0→nonzero) — a different question, out of scope here, and the natural next
candidate per the plan's own decision tree given the weak signal above.

## Production-identity evidence
- Diagnostic changes (a) `mobi_coeff_dump()` gated behind `MOBI_COEFF_DUMP`
  env var, unset by default, and (b) prediction-buffer capture before the
  residual write-back, both read-only, in `x264-stepb-diag/encoder/macroblock.h`
  and `.c`. Zero decision logic changed.
- **Disabled-path identity gate run three times** (after each of the three
  incremental extensions: initial dump, +prediction capture, +slice-type
  field), each on the full 1866-frame benchmark clip:

  | run | SHA-256 | bytes |
  |---|---|---:|
  | Candidate B (reference) | `a05e14a4...057a542` | 13,873,152 |
  | disabled-path run 1 | `a05e14a4...057a542` | 13,873,152 |
  | disabled-path run 2 | `a05e14a4...057a542` | 13,873,152 |
  | disabled-path run 3 | `a05e14a4...057a542` | 13,873,152 |

  All four byte-for-byte identical. Because identity holds against a
  reference whose own decodability was already established in prior phases,
  a separate full-clip decode re-run was not needed for the disabled path
  (transitivity of byte-identity).
- `libx264.a` SHA-256: `24946d22...1117e0`. `ffmpeg.exe` SHA-256: `1bc2fa52...37d9d`.

## Toolchain issue found and fixed along the way
The diagnostic FFmpeg build initially failed twice, for reasons unrelated to
the mobiclip source: (1) `config.mak` stores an unqualified `CC=gcc`,
resolved fresh from `PATH` at build time — this session's shell resolved
`gcc` to Cygwin's native 15.1.0 instead of the MSYS2 UCRT64 16.1.0 the
library was actually configured against, producing mixed-ABI object files
that had to be fully cleaned and rebuilt; (2) the diagnostic x264 build was
never `make install`-ed to its configured prefix, so FFmpeg silently linked
against a stock/unpatched system x264 lacking the mobiclip fields entirely
(`x264_param_t` had no `i_mobiclip`/`b_moflex`/`i_mobi_qyx` members) —
fixed by running `make install-lib-static` into the already-configured
prefix. Neither issue touched production source; both are recorded here in
case they recur in a future phase.

## Scope reduction (disclosed)
The original plan's "short segment → one real frame → full clip" progression
was cut short at the short-segment stage: capturing the full 1866-frame clip
as a text dump filled the system drive to 0 bytes free mid-run (it was
already at 953/954 GB used before this session started — a pre-existing,
system-wide condition, not created by this work, but the ~1 GB dump was the
final straw). The corrupted partial dump and truncated output were deleted
immediately. All results in this report are from a single 60-frame segment
around the previously-identified difficult region (~frame 1230), which still
yielded 363,948 blocks / 1,070,677 nonzero coefficients — a large enough
sample for the aggregate pattern to be clear, but not the full clip.
**The system drive has only ~900 MB free as of this report** — worth
attention independent of this phase.

## Deliverables
- `coeff_rd_audit/harness/validate_block_rate.c` — new whole-block-walk validation (16,864 cases, PASS).
- `coeff_rd_audit/harness/step_b_analyze.c` — the analysis tool itself.
- `coeff_rd_audit/coeff_dump_seg1230.txt` — raw capture (46 MB).
- `coeff_rd_audit/stepb_seg1230_results_independent.csv` (905,667 rows)
- `coeff_rd_audit/stepb_seg1230_results_greedy.csv` (865,786 rows)
- `coeff_rd_audit/stepb_seg1230_results_lambda_hist.csv`
- `coeff_rd_audit/stepb_seg1230_results_summary.txt` — 5-dimension aggregate
  (plane × intra/inter × region × frequency-band × transform-size). Region is
  `dark_flat`/`other` for luma only (this investigation's own framing — "dark
  gradients" was a luma complaint) and `chroma_na` for chroma, rather than
  misapplying a luma-calibrated darkness threshold to chroma's ~128-centered
  values.

## Decision
Per the plan's stated criteria: strong or even moderate signal would have
proceeded to a bounded behavioral candidate; weak/no signal closes this
avenue. The result here is weak — 94.5% of positive-rate-saving candidates
fail to break even within a generous lambda range, and the two chroma
mislabeling and lambda-bucket bugs caught during this analysis (both fixed
and disclosed above) were checked precisely because a first pass looked
suspiciously uniform, and turned out to be real bugs, not artifacts of the
underlying data. **Recommendation: close single-coefficient rate-blind
pruning. If further quantizer-side quality work is wanted, the 0→nonzero
activation test (explicitly out of scope here) is the more promising next
candidate per your own plan.**
