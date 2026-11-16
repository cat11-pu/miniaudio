# Game offline-voice mixing benchmark

Deterministic benchmark for an offline game mixer: 48 kHz stereo output,
128-frame blocks, 16 / 32 / 64 voices, linear resampling per voice and
per-voice gain + equal-power-style pan. It compares a deliberately naive
baseline against branch-reduced and SIMD implementations and verifies
bit-identical output (every float, every sample).

## Build and run

```sh
make            # -O2 AVX2+AVX-512 builds: mixbench, mixbench_o3
./mixbench
make pgo        # optional profile-guided build -> mixbench_pgo
make clean
```

`-ffp-contract=off` is required so the compiler never fuses a
multiply/add into FMA, which would change rounding. All four engines are
compiled with the same flags, so differences come only from the code.

## Engines

- `mb_mix_naive` — reference. Per-sample pan branch, resampler carry
  branch and end-of-voice branch, scalar FPU. The compiler already hoists
  the `rate/48000` division out of the inner loop at `-O2`.
- `mb_mix_opt_scalar` — every inner branch replaced by a branchless
  select (`cmov`/`setcc`); pan resolved once per block.
- `mb_mix_opt_sse2` (AVX2) — branchless scalar phase walk into an aligned
  staging buffer, then 8-wide AVX2 gain/pan/stereo accumulate.
- `mb_mix_opt_avx512` — same structure with 16-wide AVX-512 accumulate.

All optimizations preserve the exact float expressions and association
order of the baseline. In particular `(sample * gain) * panGain` is kept
as two multiplies; pre-fusing `gain * panGain` changes rounding and was
rejected by the bit-exact check.

## Bit-exact verification

Each run compares every output float with `memcmp` (plus a ULP fallback
report). Covered cases:

- full workload at 16 / 32 / 64 voices across 64 blocks;
- voices that end mid-block (end-of-voice masking path);
- positive, negative and zero pan (both pan branches).

Every engine reports `BIT-IDENTICAL` with zero mismatches and maxULP 0.
The first eight left/right samples of the 64-voice case are printed as a
direct per-sample table.

## Measured timings (ns per 128-frame block, min / median of 7)

Single thread, AMD Ryzen 7 9700X (Zen 5), GCC 15.2, `-O2`, 4096 blocks per
timed run. Representative run:

| voices | naive | opt_scalar | opt_avx2 | opt_avx512 | avx2 Δ | avx512 Δ |
|-------:|------:|-----------:|---------:|-----------:|-------:|---------:|
| 16 | 2131 / 2135 | 2172 / 2185 | 1853 / 1865 | 1819 / 1824 | 13.0% | 14.6% |
| 32 | 4260 / 4287 | 4414 / 4522 | 3776 / 3906 | 3700 / 3730 | 11.4% | 13.2% |
| 64 | 8712 / 8891 | 8987 / 9103 | 7679 / 7743 | 7526 / 7661 | 11.9% | 13.6% |

Across repeated runs the AVX2 gain is ~11–14% and AVX-512 ~13–15%, with
zero sample mismatches throughout.

## Why 30% is not reachable under the bit-exact constraint

The request asked for batching, lookup tables and fewer branches with
identical per-sample output. Microbenchmarks in this investigation
measured each idea directly; the numbers explain the ceiling:

- The compiler already converts the highly predictable carry branch to
  branchless code at `-O2`, and hoists the division. Explicit `cmov`
  code is therefore not faster (and measured 12–22% slower in isolation
  because it lengthens the serial dependency chain).
- The dominant cost (~85% of a block) is the inherently serial linear
  resampler: each output sample's source position depends on the previous
  carry, so the phase walk cannot be vectorized without changing results.
- A phase lookup table removes the float add chain and the hot loop is
  ~60% faster in isolation. But to stay bit-exact the table must be built
  with the same float recurrence; rebuilding costs ~95 ns amortized per
  block-voice regardless of table size, which cancels the hot-loop win.
  Batched AVX-512 table rebuilds (4x) and transposed/SoA layouts were all
  measured end-to-end and landed between -41% and +4%.
- Integer/fixed-point phase accumulation does not reproduce the float
  recurrence's rounding, so it fails the bit-exact check.
- Pre-fusing gain/pan into one multiply changes float association and
  fails the bit-exact check.

The robust, bit-exact win is therefore the SIMD gain/pan/accumulate pass
(~14%). Reaching ~30% requires relaxing one constraint, e.g. allowing
last-place ULP differences (permits fused gains, integer phase and full
vectorization), or changing the resampler algorithm.
