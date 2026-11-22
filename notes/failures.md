# Failure Log

Every bug, dead end, and surprise, with root cause and how to prevent it next time. Add new entries at the bottom and add a row to the index.

## Index

| ID | Failure | Area | Status |
|---|---|---|---|
| F-001 | Windows CPU far over target while Linux was fine | CPU / MSVC | OPEN: re-measure pending |
| F-002 | Worst-block CPU of 533.90% on Windows | Benchmarking | RESOLVED (diagnosis) |
| F-003 | Dither cost about 5% CPU on Windows | CPU / MSVC | PARTIAL: fix landed, Windows result pending |
| F-004 | Linux VM lost the JUCE system dev libraries | Build env | RESOLVED |
| F-005 | MSVC C4458 shadowing warnings | Build / MSVC | RESOLVED |

---

## Entry template

```markdown
### F-XXX: <symptom in a few words>
- Status: OPEN | PARTIAL | RESOLVED  ·  Platform: <where it happened>  ·  Date: <YYYY-MM-DD>
- Symptom: <what was observed, with numbers>
- Root cause: <confirmed cause, or "UNVERIFIED: <hypothesis>">
- Fix: <what changed, with commit>
- Proof of fix: <test or measurement>
- Prevention rule: <one sentence a future agent can follow>
- Related: <P-xxx, R-xxx, files>
```

---

### F-001: Windows CPU far over target while Linux was fine
- Status: OPEN  ·  Platform: Windows, VS2022 Release  ·  Date: 2026-11-01
- Symptom: first Windows measurement about 24% average CPU. After the first fix (`600a68f`), 21.89% average, still over the 15% target. Per-module highlights: Limiter 8.53%, Clipper 8.25%, Saturation 6.23%, Dither 5.04%. Full chain 22.62%. Linux measured 9.85% before the second fix.
- Root cause: partly confirmed. The spectrum FFT on the audio thread and the long oversampling filter were real costs. MSVC running about 2x slower than Linux per module is `UNVERIFIED` (codegen, floating-point contraction, or CRT calls).
- Fix: `600a68f` (P-001), then `a50a065` (P-002, P-007).
- Proof of fix: Linux 9.44% average, p99 12.4%, worst 14.7%. Windows not yet re-measured.
- Prevention rule: never declare a performance target met until it is measured on the target platform and compiler (see R-001).
- Next if still over: limiter 16x backstop plus stage-2 TruePeakDetector; DynamicEQ/CorrectiveEQ juce::dsp::FFT → RealFFT; try `/fp:contract` and `-DMERIDIAN_ENABLE_AVX2=ON`.
- Related: P-001, P-002, P-003, R-001.

### F-002: Worst-block CPU of 533.90% on Windows
- Status: RESOLVED (diagnosis tooling added)  ·  Platform: Windows  ·  Date: 2026-11-01
- Symptom: `cpu_bench` reported a single worst block at 533.90% of the time budget.
- Root cause: `UNVERIFIED`. Most likely a one-off OS scheduling stall rather than DSP, but averages alone couldn't tell them apart.
- Fix: added p99, p99.9, and worst-block reporting to `cpu_bench` (`a50a065`).
- Proof of fix: Linux shows p99 12.4% against worst 14.7%, a tight spread. Windows percentiles pending.
- Prevention rule: judge real-time safety with p99 and p99.9, and treat a lone worst-block outlier as a lead to investigate, not a verdict.
- Related: P-003.

### F-003: Dither cost about 5% CPU on Windows
- Status: PARTIAL  ·  Platform: Windows  ·  Date: 2026-11-01
- Symptom: Dither measured 5.04% on Windows against 0.79% on Linux.
- Root cause: `UNVERIFIED: MSVC compiles std::nearbyint to a CRT call inside the per-sample loop.`
- Fix: exact inline round-half-even (`a50a065`, P-007).
- Proof of fix: bit-identical on 50 million values; Linux Dither 0.23%. Windows pending.
- Prevention rule: avoid standard-library math calls in per-sample loops on MSVC; profile per module on Windows.
- Related: P-007, F-001.

### F-004: Linux VM lost the JUCE system dev libraries
- Status: RESOLVED  ·  Platform: Linux VM  ·  Date: 2026-10/11
- Symptom: rebuild failed because X11, freetype, ALSA, and fontconfig development headers had disappeared.
- Root cause: the VM environment was reset or recreated.
- Fix: reinstall `libxrandr-dev libxinerama-dev libxcursor-dev libxext-dev libx11-dev libfreetype-dev libasound2-dev libfontconfig1-dev`.
- Proof of fix: Linux build and CTest 11/11 afterwards.
- Prevention rule: at session start on a fresh Linux box, install the package list from `systems/meridian.md` section 2 before configuring CMake.
- Related: systems note, section 2.

### F-005: MSVC C4458 shadowing warnings
- Status: RESOLVED  ·  Platform: Windows, VS2022  ·  Date: 2026-11-01
- Symptom: five C4458 warnings (declaration hides a class member) in GlueCompressor `dryL`/`dryR` and PresetBrowser entries. GCC and Clang on Linux didn't flag them.
- Root cause: local variables named the same as members.
- Fix: renamed in `04f4be3`.
- Proof of fix: clean Windows Release build reported in RESUME.md.
- Prevention rule: compile with MSVC `/W4` (or GCC/Clang `-Wshadow`) in CI so Windows-only warnings show up on Linux too.
- Related: R-003.
