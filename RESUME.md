# RESUME (updated 2026-10-31)

Status: all spec items implemented. CPU fix for Windows done and verified on Linux; waiting for the user's Windows re-measurement.

Windows results from the user (VS2022, before the CPU fix):
- ctest 8/8 PASS.
- cpu_bench: 24.26 % of one core, worst block 94.29 %: OVER TARGET (Linux read 13.65 % on the same code).

CPU fix (this checkpoint):
- Spectrum FFTs moved from the audio thread to the message thread (`popSpectrumFrame`).
- `common/RealFFT.h` replaces `juce::dsp::FFT` in `LinearPhaseCrossover` (new `fft_test`).
- `common/FastMath.h` inline exp/log/tanh replaces MSVC-slow `tanh`/`log10`/`pow` in per-sample paths (new `fastmath_test`).
- `TruePeakDetector::processAgainst`: skips the polyphase evaluation when a rigorous bound proves the peak is under the threshold (limiter decisions bit-identical).
- `DotProduct.h`: AVX path, used when built with `-DMERIDIAN_ENABLE_AVX2=ON`.

Verified on Linux after the fix:
- ctest 10/10 PASS (default build and AVX2 build).
- cpu_bench: 9.85 % default build, 8.71 % AVX2 build (was 13.65 %).
- pluginval strictness 10 (in-process, GUI on) re-run after the spectrum threading change: SUCCESS.

Not verified yet:
1. Windows cpu_bench after the fix (user to run `git pull`, rebuild, `ctest -C Release`, `build\tests\Release\cpu_bench.exe --per-module`).
2. Windows VST3 inside FL Studio 21.
3. macOS AU build and auval.

If work continues: read the user's new Windows cpu_bench output. If it is still over 15 %, rebuild with `-DMERIDIAN_ENABLE_AVX2=ON` and compare; the next candidates are a transposed polyphase upsampler (measured 1.05–1.5× on the inner loop) and the 16× limiter backstop (largest remaining cost, ~20 % of instructions).
