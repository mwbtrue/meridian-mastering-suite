# Meridian Mastering Suite

## 1. Overview

Meridian Mastering Suite is a single stereo mastering plugin (VST3 / AU / Standalone) written in C++20 on JUCE 8. All audio processing runs in 64-bit double precision, and the audio thread is real-time safe: no allocations, locks or file I/O.

The signal chain has 12 modules. You can reorder them by drag-and-drop in the Module Rack. Every module has bypass, delta-solo, wet/dry and Stereo/Mid/Side/L/R routing.

| # | Module | Purpose |
|---|---|---|
| 1 | InputModule | Input trim, polarity, DC blocker |
| 2 | CorrectiveEQ | 8-band parametric EQ (min/linear phase), stereo/M/S per band |
| 3 | LowEndControl | Low-frequency mono below a set frequency, dynamic low-end control |
| 4 | DynamicEQ | 6-band dynamic EQ with external-band detection |
| 5 | GlueCompressor | VCA/SSL-style bus compressor |
| 6 | MultibandDynamics | 4-band compressor on a linear-phase crossover |
| 7 | ToneEQ | Analog-modelled Pultec, Baxandall, passive mid and side-air EQ |
| 8 | Saturation | Oversampled saturation (several models), optional 3-band, auto-gain |
| 9 | StereoImager | 4-band width with correlation guard |
| 10 | Clipper | Soft/hard clipper with drive and ceiling link |
| 11 | TruePeakLimiter | Two-stage look-ahead limiter with a guaranteed true-peak ceiling |
| 12 | Dither | TPDF + noise-shaped dither (incl. High-Shibata) |

The analysis and assistance tools are:
- BS.1770-4 metering: LUFS-M/S/I, LRA, dBTP, PLR, correlation and goniometer.
- A 2048-point spectrum analyzer.
- Reference A/B with a reference spectrum overlay.
- Codec round-trip preview (AAC 256 / MP3 320 / Opus) with post-encode true peak.
- An Intelligent Assistant that analyzes your mix against a target profile and proposes parameter changes.
- A/B/C/D snapshots and a preset browser.

## 2. Build Requirements

- CMake ≥ 3.22
- A C++20 compiler: MSVC 2022, Xcode 14+ / Apple Clang, or GCC 11+ / Clang 14+
- JUCE 8 is fetched automatically by CMake (FetchContent). To use a local copy, pass `-DFETCHCONTENT_SOURCE_DIR_JUCE=/path/to/JUCE`.
- Optional: `ffmpeg` on the `PATH`, for the codec preview (AAC / MP3 / Opus). Without it, the rest of the plugin works normally and the codec preview reports that ffmpeg is unavailable.

## 3. Build Steps (Windows — VST3 + Standalone)

```bat
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Output: `build\MeridianMasteringSuite_artefacts\Release\VST3\Meridian Mastering Suite.vst3`

## 4. Installing in FL Studio 21

1. Copy `Meridian Mastering Suite.vst3` to `C:\Program Files\Common Files\VST3\`.
2. In FL Studio, open **Options → Manage plugins** (Plugin Manager) and click **Find plugins**.
3. Find "Meridian Mastering Suite" in the list and mark it as a favourite if you like.
4. Open the Mixer, select the **Master** track, click an empty effect slot and choose **Meridian Mastering Suite**.

## 5. Build Steps (macOS — AU + VST3 + Standalone)

```bash
cmake -B build -G Xcode
cmake --build build --config Release
```

AU output: `build/MeridianMasteringSuite_artefacts/Release/AU/`. Copy the `.component` to `~/Library/Audio/Plug-Ins/Components/`, and the `.vst3` to `~/Library/Audio/Plug-Ins/VST3/`.

## 6. Build Steps (Linux — Standalone + VST3)

First install the dependencies:

```bash
sudo apt install libasound2-dev libx11-dev libxext-dev libxinerama-dev libxrandr-dev libxcursor-dev libfreetype-dev libglu1-mesa-dev mesa-common-dev
```

Then build:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Copy the `.vst3` bundle to `~/.vst3/`.

## 7. Running Tests

```bash
cd build && ctest --output-on-failure
```

Expected: **4/4 PASS**.

| Test | What it checks |
|---|---|
| `null_test` | Null residual < −140 dBFS |
| `limiter_tp_test` | True peak ≤ ceiling at 4× / 8× / 16× |
| `lufs_test` | −23 dBFS 1 kHz sine reads −23.0 ± 0.2 LUFS at 44.1 / 48 / 96 kHz |
| `crossover_sum_test` | Linear-phase band sum vs delayed input < 0.1 dB |

## 8. Profiles Directory

The five target profiles in `profiles/` are embedded in the binary as factory profiles, so they are always available. To edit them or add your own, copy `profiles/` to:

```
<UserDocuments>/Meridian Mastering Suite/Profiles/
```

A JSON file in that directory overrides a factory profile with the same name, and new files appear in the Assistant's profile list. User presets are saved as XML in `<UserDocuments>/Meridian Mastering Suite/Presets/`.

## 9. Quick-Start: Fallin' Style R&B Master

**a. Load the plugin.** Put it on your stereo master bus.

**b. Choose the profile.** In the Intelligent Assistant panel, select **"R&B Major Label – Fallin' style"**.

**c. Analyze.** Play 20 seconds of your mix, then click **Analyze**. At least 3 seconds of captured audio are required.

**d. Review the proposals.** Accept all of them, or cherry-pick with each row's Accept toggle.

**e. What each meter should read at the end:**
- **LUFS-I: −9 (±1 LU).**
  - If it is higher, reduce `lim_ceiling` or the glue threshold.
  - If it is lower, add makeup gain.
- **LUFS-S peak: −8** (chorus moments).
- **dBTP: ≤ −1.0.** The limiter guarantees this.
- **LRA: 4–7 LU.**
  - Above 7, increase multiband compression slightly.
  - Below 4, back off the glue compressor.
- **Correlation below 150 Hz: ≥ 0.9** (shown green). If it is lower, raise `lec_mono_freq`.
- **PLR: 8–10 dB.** This is the breathing room; don't let it go below 8.
- **GR budget:**
  - Limiter: 3–4 dB peaks at most.
  - Glue compressor: 1–2 dB.
  - Multiband: about 1 dB per band.
  - The Assistant shows a red warning when the budget is exceeded.

**f. Listening check.** Solo the delta button on each module to hear what it adds.
- The Saturation delta should give warm harmonics, not distortion.
- The Stereo Imager delta should show width only above 120 Hz.

**g. Pre-delivery QC checklist:**
- [ ] LUFS-I: −9 ±1
- [ ] dBTP: ≤ −1.0 (verified by limiter + plugin readout)
- [ ] LRA: 4–7
- [ ] PLR: ≥ 8
- [ ] Correlation <150 Hz: ≥ 0.9
- [ ] Codec preview (AAC 256 / MP3 320 / Opus): post-encode TP ≤ −1.0 dBTP
- [ ] Listen on mono (correlation check): vocal stays upfront, sub is tight
- [ ] Reference A/B: spectrum within 2 dB of reference on the SpectrumDisplay
- [ ] Null test (bypass all): clean null, no clicks or pops on bypass
- [ ] Final output: 24-bit / 44.1 or 48 kHz WAV, dither enabled (High-Shibata)

## 10. Architecture

See [ARCH.md](ARCH.md) for:
- the module class diagram;
- the DSP algorithm choices;
- the full parameter table;
- the latency map;
- the thread architecture;
- the CPU budget;
- the as-built deviations.

## 11. License

MIT License. Copyright (c) 2026 Meridian Audio.

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
