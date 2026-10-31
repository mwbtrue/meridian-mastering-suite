# RESUME (updated 2026-10-31)

Status: all spec items implemented and verified on Linux (GCC, Release + LTO).

Verified this checkpoint:
- ctest 8/8 PASS (null, limiter_tp, lufs, ebu_loudness, crossover_sum ±0.05 dB, chain_null, lm_undo, cpu_bench).
- cpu_bench default chain 48 kHz / 512: 13.15–13.65 % of one core (target < 15 %).
- pluginval strictness 10, in-process, GUI tests on (DISPLAY=:1): SUCCESS. Steinberg validator not run (path not set).

Not verified (needs hardware or tools this Linux VM does not have):
1. MSVC / Visual Studio 2022 build and the Windows VST3 inside FL Studio 21.
2. macOS AU build and auval.
3. CPU on Windows / in a DAW.

If work continues: build on Windows (`cmake -B build -G "Visual Studio 17 2022" -A x64`), run `ctest -C Release`, then run pluginval at strictness 10 against the Windows VST3 and load it in FL Studio on the Master track.
