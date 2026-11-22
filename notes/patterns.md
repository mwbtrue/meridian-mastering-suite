# Patterns That Worked

Reusable techniques, each backed by proof. Add new entries at the bottom and add a row to the index.

## Index

| ID | Pattern | Area | Status |
|---|---|---|---|
| P-001 | Move FFT analysis off the audio thread | Threading / CPU | VERIFIED (Linux), PARTIAL (Windows) |
| P-002 | Cascaded half-band oversampling instead of one long filter | DSP / CPU | VERIFIED (Linux) |
| P-003 | Report CPU percentiles, not just the average | Testing | VERIFIED |
| P-004 | Latency-aligned null tests as the correctness backbone | Testing | VERIFIED |
| P-005 | Delay-compensated bypass keeps reported latency constant | Host integration | VERIFIED (code + tests); host check pending |
| P-006 | Complementary crossover: top band = delayed input minus lower bands | DSP | VERIFIED |
| P-007 | Replace CRT math calls with exact inline versions, then prove they're identical | CPU / MSVC | PARTIAL |
| P-008 | One test per performance fix | Process | VERIFIED |

---

## Entry template

```markdown
### P-XXX: <short name>
- Status: VERIFIED | PARTIAL | UNVERIFIED  ·  Platforms: <Linux / Windows / macOS>
- Use when: <one line>
- What: <2-4 lines describing the technique>
- Proof: <commit, test name, measured numbers>
- Gotchas: <limits, costs, side effects>
- Related: <F-xxx, R-xxx, file paths>
```

---

### P-001: Move FFT analysis off the audio thread
- Status: VERIFIED (Linux), PARTIAL (Windows needs a re-measure)  ·  Platforms: Linux
- Use when: any spectrum display or analysis FFT is running inside `processBlock`.
- What: the audio thread only pushes samples into a lock-free FIFO. A background or UI thread runs the FFT. This is part of the CPU fix that also moved crossovers to RealFFT, inlined FastMath, bounded true-peak evaluation, and added an AVX dot product.
- Proof: commit `600a68f`; added `fft_test` and `fastmath_test`. Windows CTest 10/10 afterwards (`aaca894`), but CPU was still 21.89%, which led to P-002.
- Gotchas: off-thread work alone did not reach the target on MSVC.
- Related: F-001, `Source/analysis/`.

### P-002: Cascaded half-band oversampling instead of one long filter
- Status: VERIFIED (Linux)  ·  Platforms: Linux
- Use when: saturation or clipping needs 2x to 16x oversampling at low CPU.
- What: chain 2x Kaiser half-band stages (107/27/19/19 taps). Each stage has at least 82 dB stopband and under 0.0013 dB ripple. Round-trip latency is 63 samples for every factor above 1.
- Proof: commit `a50a065`; `oversampling_test` measured images ≤ −93 dB, aliases −84 dB, gain error ≤ 0.0012 dB. Saturation CPU went from 2.86% to 2.10% and Clipper from 2.64% to 1.95% on Linux.
- Gotchas: latency rose from 52 to 63 samples, so `computeChainLatency()` must stay in sync.
- Related: F-001, `Source/common/OversamplingEngine`.

### P-003: Report CPU percentiles, not just the average
- Status: VERIFIED  ·  Platforms: all
- Use when: benchmarking any real-time DSP.
- What: `cpu_bench` reports average, p99, p99.9, and worst block time, and has a `--per-module` mode.
- Proof: commit `a50a065`. It explained the Windows worst-block figure of 533.90%: percentiles separate a one-off OS stall from a real DSP spike.
- Gotchas: run benchmarks on Release builds with nothing else heavy running.
- Related: F-002.

### P-004: Latency-aligned null tests as the correctness backbone
- Status: VERIFIED  ·  Platforms: Linux
- Use when: changing any module that should be transparent at neutral settings, or any bypass and routing logic.
- What: run a signal (for example pink noise at −6 dBFS) through the path, align for the reported latency, subtract it from the input, and require the residual to be under a threshold.
- Proof: `null_test` limiter null measured −256.5 dBFS (pass < −140); `chain_null_test`.
- Gotchas: if the reported latency is wrong, the null fails, which makes this test a free latency check.
- Related: P-005.

### P-005: Delay-compensated bypass keeps reported latency constant
- Status: VERIFIED in code and tests; FL Studio host check pending  ·  Platforms: Linux
- Use when: any module can be bypassed while the host is running.
- What: bypassed modules still delay their dry path by their latency. Total latency is recomputed only when phase mode or oversampling changes, then reported with `setLatencySamples()`.
- Proof: ARCH.md §5; `chain_null_test`.
- Gotchas: hosts handle latency changes during playback differently. Confirm in FL Studio 21 (systems note, section 5).
- Related: P-004.

### P-006: Complementary crossover: top band = delayed input minus lower bands
- Status: VERIFIED  ·  Platforms: Linux
- Use when: building multiband splits that must sum back to flat.
- What: compute the lower bands with linear-phase FIR (2049 taps, Kaiser β=6, partitioned FFT convolution, P = 256), then make the top band the latency-matched input minus those bands. The sum is flat by construction.
- Proof: `crossover_sum_test` measured 0.000000 dB deviation (threshold ±0.05 dB). Fixed latency of 1280 samples at 44.1/48 kHz.
- Gotchas: a short kernel (64 taps) has a transition band of about 4 kHz and cannot separate a 120 Hz crossover.
- Related: ARCH.md crossover section.

### P-007: Replace CRT math calls with exact inline versions, then prove they're identical
- Status: PARTIAL (verified as identical; Windows CPU benefit not yet measured)  ·  Platforms: Linux
- Use when: a per-sample hot loop calls `std::nearbyint`, `std::round`, or similar on MSVC.
- What: write an exact inline round-half-even and compare it to the library call on a large sample set, including ties.
- Proof: commit `a50a065`. 50 million values with ties included gave 0 mismatches. Dither CPU went from 0.79% to 0.23% on Linux.
- Gotchas: the Windows cause is `UNVERIFIED` (see F-003).
- Related: F-003.

### P-008: One test per performance fix
- Status: VERIFIED  ·  Platforms: all
- Use when: making any optimization.
- What: every speed change ships with a correctness test for the code it touched, so optimizations can't silently change the sound.
- Proof: `600a68f` added `fft_test` and `fastmath_test`; `a50a065` added `oversampling_test`.
- Related: R-002.
