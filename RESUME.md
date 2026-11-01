# RESUME (updated 2026-10-31)

Status: all spec items implemented. Second Windows CPU fix done and verified on Linux. Waiting for the user to re-measure on Windows.

Windows results from the user after the first CPU fix (VS2022, `git pull` + rebuild):
- ctest 10/10 PASS.
- cpu_bench: 21.89 % average, worst block 533.90 %: OVER TARGET.
- Per-module (%): Input 0.38, CorrEQ -0.49, LowEnd 1.78, DynEQ 3.19, Glue 1.49, Multiband 3.33, ToneEQ 1.40, Saturation 6.23, Imager 2.39, Clipper 8.25, Limiter 8.53, Dither 5.04, Framework 0.92, Full 22.62.

Second CPU fix (this checkpoint):
- `common/OversamplingEngine`: the single long polyphase filter is replaced by cascaded 2x Kaiser half-band stages (107/27/19/19 taps). Each stage has ≥82 dB stopband and ripple below 0.0013 dB. Round-trip latency is 63 samples for every factor > 1 (was 52). Saturation and Clipper are the main beneficiaries.
- New `tests/oversampling_test`. It checks exact latency, passband gain error under 0.01 dB, phase error under 1e-4 rad, images and aliases below -80 dB, and bit-identical output for any block size, at 2/4/8/16x. Measured: images ≤ -93 dB, aliases -84 dB, gain error ≤ 0.0012 dB.
- `Dither`: `std::nearbyint` replaced by an exact inline round-half-even. MSVC compiles `std::nearbyint` to a CRT call, which was the likely cause of the 5 % on Windows. Checked against `std::nearbyint` on 50M values, ties included: 0 mismatches.
- `cpu_bench`: reports p99 / p99.9 / worst block time, so a one-off OS stall can be told apart from real DSP spikes.

Verified on Linux after this fix:
- ctest 11/11 PASS.
- cpu_bench: 9.44 % average (was 9.85 %). Block time p99 12.4 %, worst 14.7 %. Full-chain latency is 2958 samples.
- Per-module (%): Saturation 2.10 (was 2.86), Clipper 1.95 (was 2.64), Dither 0.23 (was 0.79).

Not verified yet:
1. Windows cpu_bench after this fix. The user needs to run `git pull`, rebuild, `ctest -C Release`, then `build\tests\Release\cpu_bench.exe --per-module`. Also try the build with `-DMERIDIAN_ENABLE_AVX2=ON`.
2. pluginval strictness 10 after the oversampler change. Last SUCCESS was at commit aaca894. Re-run it with the command in the notes.
3. Windows VST3 inside FL Studio 21.
4. macOS AU build and auval.

If work continues: read the user's Windows output. If it is still over 15 %, the remaining big costs are the limiter (16x backstop plus stage-2 TruePeakDetector) and the DynamicEQ/CorrectiveEQ juce::dsp::FFT, which can move to `RealFFT`. If MSVC is still ~2x Linux per module, test `/fp:contract` and check that /GL + /LTCG reach the SharedCode library.
Linux VM note: rebuilding needs `libxrandr-dev libxinerama-dev libxcursor-dev libxext-dev libx11-dev libfreetype-dev libasound2-dev libfontconfig1-dev`. They had disappeared and were reinstalled.
