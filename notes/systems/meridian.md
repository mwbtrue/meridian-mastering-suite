# System Notes: Meridian Mastering Suite

| Field | Value |
|---|---|
| Repo | [mwbtrue/meridian-mastering-suite](https://github.com/mwbtrue/meridian-mastering-suite) |
| Notes current as of | commit `04f4be3` (2026-11-01) |
| Last reviewed | 2026-11-21 |
| Stack | C++20, JUCE 8.0.4 (fetched by CMake), CMake, CTest, pluginval |
| Targets | VST3 and Standalone; AU added on Apple platforms (CMakeLists.txt) |
| Primary host | FL Studio 21 on Windows, on the master channel (README) |

---

## 1. Architecture at a glance

Source layout (`Source/`): `PluginProcessor`, `PluginEditor`, `ParameterIDs.h`, plus folders `dsp/`, `analysis/`, `assistant/`, `common/`, `ui/`.

Processing: double precision internally, real-time-safe audio thread (ARCH.md).

### Signal chain (12 modules, reorderable)

| # | Module | Default latency @ 48 kHz | Notes |
|---|---|---|---|
| 1 | Input trim | 0 | |
| 2 | Corrective EQ (8 band) | 0 (min-phase) / 4096 (linear) / about half the FIR length (natural) | RBJ biquads, 8192-tap FIR, or a minimum-phase FIR made with the cepstrum method |
| 3 | Low-end control | 0 | |
| 4 | Dynamic EQ | see ARCH.md latency map | Uses juce::dsp::FFT (a candidate to move to RealFFT) |
| 5 | Glue compressor | 0 | |
| 6 | Multiband dynamics (4 band) | 1280 crossover | Linear-phase FIR crossover, partitioned FFT convolution, P = 256 |
| 7 | Tone EQ | 0 | |
| 8 | Saturation | oversampler 63 | LR4 IIR band split, zero latency |
| 9 | Stereo imager | see ARCH.md | |
| 10 | Clipper | oversampler 63 | |
| 11 | True-peak limiter | 256 (248 @ 44.1 kHz) | Two lookahead stages plus a backstop. Low-latency mode keeps 160 samples of true-peak kernels |
| 12 | Dither | 0 | Exact inline round-half-even |

Every module has bypass, delta audition, wet/dry, and stereo/M/S/L/R routing.

Full-chain latency: 2958 samples (Linux cpu_bench, RESUME.md). It is computed in `PluginProcessor::computeChainLatency()` and reported through `setLatencySamples()`. Bypassed modules keep a delay-compensated dry path, so the reported latency never changes when bypass toggles (ARCH.md §5).

### Analysis and workflow

- BS.1770-4 loudness metering, spectrum (FFT runs off the audio thread), reference A/B, codec preview.
- A/B/C/D snapshots, presets, undo/redo, loudness-matched bypass.
- Intelligent Assistant: analyzes the mix against a target profile and proposes parameter changes.
- Profiles (`profiles/`): `apple_digital_masters`, `club_loud`, `neo_soul_dynamic`, `rb_fallin_style`, `streaming_normalized`.
  - `rb_fallin_style` is a sound reconstruction, not the original session values. Treat its numbers as `UNVERIFIED` targets.

---

## 2. Build

| Platform | Command notes | Status |
|---|---|---|
| Linux | Needs `libxrandr-dev libxinerama-dev libxcursor-dev libxext-dev libx11-dev libfreetype-dev libasound2-dev libfontconfig1-dev` (see F-004) | `VERIFIED` builds |
| Windows | VS2022 Release build; optional `-DMERIDIAN_ENABLE_AVX2=ON` | `VERIFIED` builds at `a50a065`; C4458 warnings fixed in `04f4be3` |
| macOS | AU target exists | `UNVERIFIED`: not built or auval-tested yet |

---

## 3. Test suite (`tests/`)

| Test | Checks |
|---|---|
| `lufs_test`, `ebu_loudness_test` | BS.1770-4 / EBU loudness accuracy |
| `limiter_tp_test` | True-peak ceiling holds |
| `crossover_sum_test` | Multiband sum is flat within ±0.05 dB (measured 0.000000 dB) |
| `chain_null_test`, `null_test` | Latency-aligned nulls (limiter null measured −256.5 dBFS; pass < −140) |
| `lm_undo_test` | Loudness-matched bypass and undo |
| `oversampling_test` | Exact latency, gain error < 0.01 dB, phase < 1e-4 rad, images/aliases < −80 dB, bit-identical output at any block size, at 2/4/8/16x |
| `fft_test`, `fastmath_test` | FFT and fast-math correctness |
| `cpu_bench` | Average CPU plus p99, p99.9, and worst block time; `--per-module` breakdown; per-module latency |

Shared harnesses: `TestHarness.h`, `PluginHarness.h`.

CPU target: under 15% average (cpu_bench).

---

## 4. Platform validation status

| Check | Linux | Windows | macOS |
|---|---|---|---|
| CTest | 11/11 PASS (`a50a065`) | 10/10 PASS before the oversampler change; needs a re-run with 11 tests | Not run |
| cpu_bench average | 9.44%, p99 12.4%, worst 14.7% | 21.89% after the first fix (over target); needs a re-measure after `a50a065` | Not run |
| pluginval strictness 10 | SUCCESS (`a50a065`) | Not run | Not run |
| Host test | n/a | FL Studio 21 VST3: pending | auval: pending |

### Open items (from RESUME.md)

1. Windows: `git pull`, rebuild, `ctest -C Release`, then `build\tests\Release\cpu_bench.exe --per-module`. Also try `-DMERIDIAN_ENABLE_AVX2=ON`.
2. Windows: load the VST3 in FL Studio 21 and run the host checklist (section 5).
3. macOS: AU build and `auval`.
4. If Windows is still over 15%, the next costs to attack are the limiter (16x backstop plus stage-2 TruePeakDetector) and the DynamicEQ/CorrectiveEQ juce::dsp::FFT, which could move to RealFFT. If MSVC is still about 2x Linux per module, test `/fp:contract`.

---

## 5. FL Studio 21 host checklist (fill in when tested)

| Check | Result | Date / build |
|---|---|---|
| Plugin scans and loads on the master channel | | |
| Reported latency matches cpu_bench (2958 @ 48 kHz) and FL's PDC aligns it | | |
| Latency stays constant when modules are bypassed | | |
| Automation of key parameters works and records | | |
| Project save/reload restores the full state (chain order, snapshots) | | |
| Offline render matches the real-time output (null test on the render) | | |
| CPU in FL's meter at a 512-sample buffer | | |
| Sample-rate change (44.1 / 48 / 96 kHz) handled without glitches | | |

---

## 6. Change log for this note

| Date | Change | Source |
|---|---|---|
| 2026-11-21 | Initial seed | Repo at `04f4be3` |
