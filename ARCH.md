# Meridian Mastering Suite — Architecture Document

## 1. Overview

Single stereo VST3/AU/Standalone mastering plugin. All internal processing in 64-bit double precision. Supports 44100–192000 Hz sample rates. Real-time safe: zero allocations, zero locks, zero file I/O on the audio thread. Lock-free SPSC FIFOs bridge audio thread to UI/metering thread. Full JUCE AudioProcessorValueTreeState parameter management with automation, state recall, and A/B/C/D snapshots.

---

## 2. Module Class Diagram (PlantUML / textual)

```
PluginProcessor (AudioProcessor)
 ├── InputModule          (dsp/InputModule.h)
 ├── CorrectiveEQ         (dsp/CorrectiveEQ.h)
 ├── LowEndControl        (dsp/LowEndControl.h)
 ├── DynamicEQ            (dsp/DynamicEQ.h)
 ├── GlueCompressor       (dsp/GlueCompressor.h)
 ├── MultibandDynamics    (dsp/MultibandDynamics.h)
 ├── ToneEQ               (dsp/ToneEQ.h)
 ├── Saturation           (dsp/Saturation.h)
 ├── StereoImager         (dsp/StereoImager.h)
 ├── Clipper              (dsp/Clipper.h)
 ├── TruePeakLimiter      (dsp/TruePeakLimiter.h)
 ├── Dither               (dsp/Dither.h)
 ├── MeteringEngine       (analysis/MeteringEngine.h)
 ├── SpectrumAnalyzer     (analysis/SpectrumAnalyzer.h)
 ├── CodecPreview         (analysis/CodecPreview.h)
 ├── ReferencePanel       (analysis/ReferencePanel.h)
 └── IntelligentAssistant (assistant/IntelligentAssistant.h)

PluginEditor (AudioProcessorEditor)
 ├── ModuleRack           (ui/ModuleRack.h)
 ├── SpectrumDisplay      (ui/SpectrumDisplay.h)
 ├── MeterColumn          (ui/MeterColumn.h)
 ├── PresetBrowser        (ui/PresetBrowser.h)
 └── AssistantPanel       (ui/AssistantPanel.h)

Support classes:
 ├── LockFreeQueue<T,N>   (common/LockFreeQueue.h)  — SPSC ring buffer
 ├── SmoothParam          (common/SmoothParam.h)     — 1-pole param smoother
 ├── LinearPhaseFIR       (common/LinearPhaseFIR.h)  — overlap-add convolution
 ├── OversamplingEngine   (common/OversamplingEngine.h) — polyphase up/down
 └── MidSideMatrix        (common/MidSideMatrix.h)   — encode/decode helpers
```

---

## 3. DSP Algorithm Choices (per module, with justification)

### 3.0 InputModule
- **Trim**: single coefficient multiply per sample, smoothed via 1-pole IIR (τ = 20 ms).
- **Polarity**: sign flip, sample-accurate.
- **DC Block**: 1st-order HPF at 2 Hz (H(z) = (1 − z⁻¹)/(1 − 0.9999·z⁻¹)), applied once per block.
- **Metering**: peak hold + RMS (125 ms window) via lock-free write to MeteringEngine FIFO.

### 3.1 CorrectiveEQ (8-band Parametric EQ)
- **Algorithm**: Biquad cascade (up to 8 stages) with three phase modes:
  - *Minimum phase*: bilinear-transform RBJ cookbook coefficients (Audio EQ Cookbook, Zölzer). Computed once per param change. O(5 mults + 4 adds) per sample per band. Chosen for zero latency, standard mastering EQ behaviour.
  - *Linear phase*: FFT-based overlap-add FIR (8192-tap window, Kaiser β=8, zero-phase). Latency = 4096 samples. Chosen for transparent broadband correction with no transient smear.
  - *Natural phase*: minimum-phase FIR derived from linear-phase FIR via Hilbert transform cepstrum method (Oppenheim & Schafer). Latency = ~half the FIR length. Best of both worlds for gentle tonal shaping.
- **Filter types**: Bell (constant-Q per Orfanidis), High/Low shelf (matched analog prototype), Notch, Tilt, HPF/LPF Butterworth (cascaded biquads, 2-pole per stage, up to 8 stages = 96 dB/oct), Elliptic (ellipf() custom Chebyshev-expansion, 4th-order sections).
- **Justification**: Bilinear-transform biquads are the industry-standard for parametric EQ (Pro-Q, Fabfilter). Linear-phase FIR for corrective work avoids group-delay audibility on transient-heavy material. Elliptic chosen for steep HPF — same stopband attenuation in fewer poles vs. Butterworth.

### 3.2 LowEndControl
- **Mono maker**: Linkwitz-Riley crossover (4th-order, 2× Butterworth biquads in cascade) with user-set frequency (40–200 Hz, default 120 Hz). Below crossover: M = L, S = 0 (full mono). Above: M/S pass-through. LR crossover preserves flat magnitude and constant group delay at crossover.
- **Dynamic sub band (40–70 Hz)**: Sidechain detection via RMS envelope (BPF 40–70 Hz, RMS 10 ms window). Gain computer: downward compression only, ratio 4:1, attack 10 ms, release 120 ms, threshold set so max GR = -3 dB. Applied in the M channel only.
- **Justification**: Mono sub is industry-standard mastering practice (eliminates phase-cancellation on club systems). Dynamic compression of 40–70 Hz controls subsonic energy without affecting the kick attack.

### 3.3 DynamicEQ (6-band)
- **Algorithm**: Per-band: bandpass sidechain (BPF biquad, internal or external sidechain) → RMS envelope detector → gain computer → parametric EQ section (same biquad as CorrectiveEQ). Gain is time-varying, applied as a smoothed coefficient update.
- **Spectral resonance suppressor**: FFT spectrogram (2048-point, 75% overlap, Hann window) → per-bin variance tracking (exponential average, τ=2 s) → bins with variance > threshold get attenuated. Gain applied via short linear-phase FIR to avoid pre-ringing.
- **M/S**: separate sidechain and gain for M and S channels.
- **Justification**: Dynamic EQ is cleaner than static EQ for problem frequencies that only emerge at certain levels (sibilance, mud). Spectral resonance suppressor catches modal resonances below the fixed-band threshold.

### 3.4 GlueCompressor (VCA/SSL-style)
- **Algorithm**: Feed-forward VCA model:
  1. SC HPF (HPF biquad) on sidechain sum.
  2. Stereo-linked RMS detector: `env = α_a × in² + (1−α_a) × env` (attack), release: `env = α_r × env` (hold-then-release, program-dependent auto-release: τ_r adapts inversely to signal variance over 200 ms window).
  3. Gain computer: `GR = (1 − 1/ratio) × (threshold − dB(env))` when above threshold, soft-knee within ±knee/2 dB.
  4. Ballistic smoothing: separate attack/release coefficients applied to GR signal.
  5. Lookahead: delay line on audio (0–10 ms), ahead detection on sidechain (circular buffer).
  6. Mix (parallel compression): wet/dry blend before output.
- **Justification**: Feed-forward topology matches the SSL G-Bus behaviour (transparent control with predictable pumping). Program-dependent auto-release is the key to the SSL "glue" sound — the compressor relaxes faster on dense passages and holds longer on sparse ones.

### 3.5 MultibandDynamics (4-band)
- **Crossovers**: Linear-phase Linkwitz-Riley FIR (64-tap per band, Kaiser window β=6). Fixed latency regardless of crossover frequency. Verified flat sum (±0.05 dB, see tests).
- **Per band**: Same feed-forward VCA model as GlueCompressor plus upward expansion (gain computer mirrored below threshold). Independent attack/release/ratio/threshold/knee per band.
- **Band processing**: Applied in the M channel and S channel independently (4×2 = 8 processing paths).
- **Justification**: Linear-phase crossovers preserve transient integrity — critical for mastering. Downward+upward per-band allows both compression and expansion (e.g., upward expand the Air band for shimmer).

### 3.6 ToneEQ (Analog-modelled)
- **Pultec section**: Simultaneous boost and cut at the same frequency (original Pultec circuit behaviour): boost EQ is a shelving + resonance curve (parallel RLC analog prototype, bilinear-transformed), attenuation is a notch-like curve below the boost frequency. Both are biquad implementations of the analog transfer function.
- **Baxandall bass/treble**: Tone-control model (Baxandall active tone network), bilinear-transformed to biquad. Frequency-independent at centre, familiar bass/treble feel.
- **Passive mid**: Simple bell (RBJ) constrained to realistic passive component Q values (Q 0.3–1.5).
- **Justification**: Analog modelling (vs. generic parametric) gives the "musical" frequency interaction that makes Pultec EQs iconic — the simultaneous boost+cut creates a dip just below the boosted frequency, tightening the low end while adding warmth.

### 3.7 Saturation
- **Tape model**: Hysteresis loop approximation (Dahl model — same as ChowTapeModel). Nonlinear differential equation solved with Runge-Kutta 4 at oversampled rate. Drive controls loop area; bias controls asymmetry (even harmonics).
- **Tube model**: Triode plate-curve approximation: `y = tanh(x + bias·x²)` with DC offset correction. Simple but perceptually accurate for warm 2nd-harmonic saturation.
- **Transformer model**: Odd-harmonic rich: `y = x − (x³/3)·drive` (soft-clip) applied with a DC-blocking shelf, simulating the core saturation with LF phase characteristic.
- **Oversampling**: Polyphase FIR (up to 16×, minimum-phase, 80 dB stopband attenuation). Applied per saturation stage. Anti-aliasing is linear-phase on the downsampler side only (upsampler needs minimum-phase for low latency).
- **Multiband**: 3 bands with LR crossovers (same as MultibandDynamics); each band fed into its own saturation stage with independent drive and model.
- **Auto-gain**: Measure RMS gain change per block; apply inverse correction (smoothed, τ=100 ms). Prevents perceived level jump when drive is increased.
- **Justification**: Hysteresis for tape (physical accuracy), triode polynomial for tube (lightweight, harmonically correct), odd-harmonic polynomial for transformer. 16× oversampling ensures aliasing is below -120 dBFS.

### 3.8 StereoImager (4-band)
- **Algorithm**: Per-band M/S encode → width coefficient → M/S decode. Width = 1.0 → original stereo. Width > 1.0 → boost S relative to M. Width < 1.0 → reduce S (narrow). Width = 0 → mono (S=0).
- **Crossovers**: Same LR FIR crossovers as MultibandDynamics (shared code path, LinearPhaseCrossover class).
- **Correlation guard**: After widening, compute short-term correlation (50 ms RMS of M·S divided by RMS of each). If correlation < 0 (anti-phase), auto-reduce S gain until correlation = 0.0. Guard is per-band.
- **Justification**: M/S widening is the industry-standard width tool. The correlation guard prevents anti-phase cancellation on mono playback, crucial for streaming platforms that downmix.

### 3.9 Clipper
- **Algorithm**: Hard/soft/knee clipper with 8× oversampling (polyphase FIR, same OversamplingEngine).
  - *Hard*: `y = clip(x, −ceil, +ceil)`.
  - *Soft* (default): `y = tanh(x/ceil) × ceil` (smooth saturation at ceiling, infinite-order knee).
  - *Knee*: `y = x` below (ceil−drive), polynomial transition in the knee region, hard clip above ceil.
- **Drive** (0–3 dB): pre-gain before the clipping function; auto-compensated post-clip to maintain ceiling.
- **Ceiling link**: Ceiling is hard-linked to TruePeakLimiter ceiling; they share a single APVTS parameter.
- **Justification**: A clipper before the limiter reduces peak-to-average ratio without the pumping that a limiter causes on fast transients. The combination (clipper → limiter) is how modern loudness is achieved without artefacts.

### 3.10 TruePeakLimiter (two-stage)
- **Stage 1 — Transient limiter**: Fast lookahead (0.5 ms), ratio 100:1, attack 0.01 ms, release 5 ms. Catches fast transients before Stage 2.
- **Stage 2 — Loudness/True-Peak limiter**:
  - Lookahead: 0–5 ms (default 1.5 ms). Implemented as a circular delay buffer on audio; sidechain reads ahead via offset.
  - True-peak detection: 4× oversampling (fixed; OversamplingEngine with 96 dB stopband). Measures inter-sample peak per ITU-R BS.1770-4 §2.3. Ceiling is enforced on the upsampled signal.
  - Gain computer: `GR = ceil − max(abs(upsampled_sample))` per sample, smoothed via release ballistic.
  - Release: "Auto/program-dependent" — release τ adapts: fast (20 ms) when signal is rising post-peak, slow (200 ms) when signal is falling (maintains density). "Punchy" style: moderate transient release (50 ms). "Warm": longer release (150 ms) with soft saturation on GR signal. "Aggressive": faster attack on stage 1 (0.005 ms), tighter release.
  - Channel link: Below 150 Hz always linked (mono sub GR). Above 150 Hz: link 0–100% (interpolated M/S gain reduction).
  - Ceiling guarantee: The final hard clip `y = clip(upsampled_signal, −ceil, +ceil)` is applied after gain reduction, then downsampled. This provides a mathematically guaranteed ceiling, independent of envelope accuracy.
- **Justification**: Two-stage design separates transient control (where speed matters) from loudness density control (where release shape matters). ITU-R BS.1770-4 true-peak at 4× (minimum spec); optional 8×/16× for ultra-precision. Hard clip as final backstop — this is how Pro-L 2 guarantees its ceiling.

### 3.11 Dither
- **TPDF**: Two independent PRNG (xorshift64) → difference → scaled to 1 LSB at target bit depth. Applied only on the final output sample before DAC/render.
- **Noise shaping**: Two curves: "High-Shibata" (7th-order, blue-spectrum — pushes dither energy above 15 kHz) and "E-weighted" (2nd-order, mid-attenuated). FIR error-feedback filter applied to dither signal.
- **Auto-off at 32f**: APVTS parameter "outputBitDepth". When host reports 32-bit float output, dither is bypassed (floating-point has >120 dB noise floor, dither is audibly irrelevant).
- **Justification**: TPDF is the correct dither type for audio (eliminates all signal-correlated distortion). Noise shaping trades total noise for psychoacoustically less-audible noise. High-Shibata matches CD mastering practice.

---

## 4. Full Parameter Table

| ID | Module | Range | Default | Unit | Skew | Smoothing |
|----|--------|-------|---------|------|------|-----------|
| input_trim | InputModule | -24 to +24 | 0 | dB | linear | 20 ms |
| input_polarity | InputModule | 0,1 | 0 | bool | — | sample-exact |
| input_dcblock | InputModule | 0,1 | 1 | bool | — | — |
| ceq_band[0..7]_freq | CorrectiveEQ | 20–20000 | 22/200/500/1k/3k/6k/10k/16k | Hz | log | 5 ms |
| ceq_band[0..7]_gain | CorrectiveEQ | -18 to +18 | 0 | dB | linear | 5 ms |
| ceq_band[0..7]_q | CorrectiveEQ | 0.1–10 | 0.7 | — | log | 5 ms |
| ceq_band[0..7]_type | CorrectiveEQ | enum 0–7 | 0=Bell | — | — | — |
| ceq_band[0..7]_slope | CorrectiveEQ | 6/12/18/24/36/48/72/96 | 24 | dB/oct | — | — |
| ceq_band[0..7]_enabled | CorrectiveEQ | 0,1 | 1 | bool | — | — |
| ceq_phase_mode | CorrectiveEQ | 0=Min,1=Lin,2=Natural | 0 | enum | — | — |
| ceq_band[0..7]_channel | CorrectiveEQ | 0=Stereo,1=Mid,2=Side,3=L,4=R | 0 | enum | — | — |
| lec_mono_freq | LowEndControl | 40–200 | 120 | Hz | log | 10 ms |
| lec_mono_enabled | LowEndControl | 0,1 | 1 | bool | — | — |
| lec_dyn_enabled | LowEndControl | 0,1 | 1 | bool | — | — |
| lec_dyn_threshold | LowEndControl | -30 to 0 | -12 | dBFS | linear | 5 ms |
| lec_dyn_maxgr | LowEndControl | 0 to -6 | -3 | dB | linear | — |
| deq_band[0..5]_freq | DynamicEQ | 20–20000 | 275/3500/7000/120/5000/8000 | Hz | log | 5 ms |
| deq_band[0..5]_q | DynamicEQ | 0.3–8 | 1.0 | — | log | 5 ms |
| deq_band[0..5]_threshold | DynamicEQ | -60 to 0 | -24 | dBFS | linear | 5 ms |
| deq_band[0..5]_maxgain | DynamicEQ | -12 to +6 | -2 | dB | linear | 5 ms |
| deq_band[0..5]_attack | DynamicEQ | 0.1–200 | 5 | ms | log | — |
| deq_band[0..5]_release | DynamicEQ | 1–2000 | 60 | ms | log | — |
| deq_band[0..5]_channel | DynamicEQ | 0=Stereo,1=Mid,2=Side | 0 | enum | — | — |
| deq_band[0..5]_enabled | DynamicEQ | 0,1 | 1 | bool | — | — |
| deq_reson_enabled | DynamicEQ | 0,1 | 1 | bool | — | — |
| glue_ratio | GlueCompressor | 1.1–10 | 2.0 | :1 | log | 5 ms |
| glue_threshold | GlueCompressor | -40 to 0 | -18 | dBFS | linear | 5 ms |
| glue_attack | GlueCompressor | 0.1–100 | 30 | ms | log | — |
| glue_release | GlueCompressor | 10–2000 | 0 (Auto) | ms | log | — |
| glue_knee | GlueCompressor | 0–12 | 4 | dB | linear | 5 ms |
| glue_makeup | GlueCompressor | -12 to +12 | 0 | dB | linear | 20 ms |
| glue_mix | GlueCompressor | 0–100 | 100 | % | linear | 10 ms |
| glue_sc_hpf | GlueCompressor | 20–300 | 100 | Hz | log | 10 ms |
| glue_lookahead | GlueCompressor | 0–10 | 0 | ms | linear | — |
| glue_sc_channel | GlueCompressor | 0=L+R,1=Mid | 0 | enum | — | — |
| mbd_xover[0..2]_freq | MultibandDynamics | 60–16000 | 120/800/5000 | Hz | log | 20 ms |
| mbd_band[0..3]_ratio | MultibandDynamics | 1.0–10 | 2/1.5/1.5/2 | :1 | log | 5 ms |
| mbd_band[0..3]_threshold | MultibandDynamics | -40 to 0 | -18 | dBFS | linear | 5 ms |
| mbd_band[0..3]_attack | MultibandDynamics | 0.1–200 | 30/20/10/5 | ms | log | — |
| mbd_band[0..3]_release | MultibandDynamics | 5–2000 | 150/120/80/50 | ms | log | — |
| mbd_band[0..3]_knee | MultibandDynamics | 0–12 | 4 | dB | linear | 5 ms |
| mbd_band[0..3]_makeup | MultibandDynamics | -12 to +12 | 0 | dB | linear | 20 ms |
| mbd_band[0..3]_upward_thresh | MultibandDynamics | -60 to -20 | -50 | dBFS | linear | 5 ms |
| mbd_band[0..3]_upward_ratio | MultibandDynamics | 1.0–4.0 | 1.0 | :1 | log | 5 ms |
| mbd_band[0..3]_enabled | MultibandDynamics | 0,1 | 1 | bool | — | — |
| teq_pultec_bass_freq | ToneEQ | 20/30/60/100 | 60 | Hz | enum | 10 ms |
| teq_pultec_bass_boost | ToneEQ | 0–10 | 1.5 | dB | linear | 10 ms |
| teq_pultec_bass_atten | ToneEQ | 0–10 | 0 | dB | linear | 10 ms |
| teq_pultec_atten_freq | ToneEQ | 100/200/300/400 | 200 | Hz | enum | 10 ms |
| teq_pultec_atten_bw | ToneEQ | 0.5–2.0 | 1.0 | oct | linear | 10 ms |
| teq_pultec_mid_boost | ToneEQ | 0–10 | 0.75 | dB | linear | 10 ms |
| teq_pultec_mid_freq | ToneEQ | 3k/4k/5k/8k/10k/12k/16k | 3000 | Hz | enum | 10 ms |
| teq_pultec_mid_bw | ToneEQ | 0.5–2.0 | 1.4 | oct | linear | 10 ms |
| teq_pultec_treble_boost | ToneEQ | 0–10 | 1.5 | dB | linear | 10 ms |
| teq_pultec_treble_freq | ToneEQ | 8k/10k/12k/16k | 14000 | Hz | enum | 10 ms |
| teq_pultec_treble_bw | ToneEQ | 0.5–2.0 | 1.0 | oct | linear | 10 ms |
| teq_baxandall_bass | ToneEQ | -6 to +6 | 0 | dB | linear | 10 ms |
| teq_baxandall_bass_freq | ToneEQ | 50–500 | 100 | Hz | log | 10 ms |
| teq_baxandall_treble | ToneEQ | -6 to +6 | 0 | dB | linear | 10 ms |
| teq_baxandall_treble_freq | ToneEQ | 2k–15k | 10000 | Hz | log | 10 ms |
| teq_passive_mid_gain | ToneEQ | -6 to +6 | 0 | dB | linear | 10 ms |
| teq_passive_mid_freq | ToneEQ | 200–8000 | 1000 | Hz | log | 10 ms |
| teq_passive_mid_q | ToneEQ | 0.3–1.5 | 0.7 | — | linear | 10 ms |
| teq_side_air_gain | ToneEQ | -6 to +6 | 1.0 | dB | linear | 10 ms |
| teq_side_air_freq | ToneEQ | 8k–20k | 10000 | Hz | log | 10 ms |
| sat_model | Saturation | 0=Tape,1=Tube,2=Transformer | 0 | enum | — | — |
| sat_drive | Saturation | 0–24 | 2 | dB | linear | 20 ms |
| sat_bias | Saturation | -1 to +1 | 0 | — | linear | 20 ms |
| sat_mix | Saturation | 0–100 | 40 | % | linear | 10 ms |
| sat_oversampling | Saturation | 1/2/4/8/16 | 4 | x | — | — |
| sat_multiband_enabled | Saturation | 0,1 | 0 | bool | — | — |
| sat_band[0..2]_freq | Saturation | 80–8000 | 200/2000 | Hz | log | 10 ms |
| sat_band[0..2]_drive | Saturation | 0–24 | 2 | dB | linear | 20 ms |
| sat_autogain | Saturation | 0,1 | 1 | bool | — | — |
| img_band[0..3]_width | StereoImager | 0–200 | 0/100/110/120 | % | linear | 20 ms |
| img_band[0..3]_enabled | StereoImager | 0,1 | 1 | bool | — | — |
| img_corr_guard | StereoImager | 0,1 | 1 | bool | — | — |
| clipper_mode | Clipper | 0=Soft,1=Hard,2=Knee | 0 | enum | — | — |
| clipper_ceiling | Clipper | -3.0 to 0.0 | -1.0 | dBTP | linear | 5 ms |
| clipper_drive | Clipper | 0–3 | 1.0 | dB | linear | 5 ms |
| clipper_enabled | Clipper | 0,1 | 1 | bool | — | — |
| lim_ceiling | TruePeakLimiter | -3.0 to 0.0 | -1.0 | dBTP | linear | 5 ms |
| lim_lookahead | TruePeakLimiter | 0–5 | 1.5 | ms | linear | — |
| lim_release | TruePeakLimiter | 10–500 | 0 (Auto) | ms | log | — |
| lim_style | TruePeakLimiter | 0=Transparent,1=Punchy,2=Warm,3=Aggressive | 0 | enum | — | — |
| lim_link | TruePeakLimiter | 0–100 | 100 | % | linear | — |
| lim_oversampling | TruePeakLimiter | 4/8/16 | 4 | x | — | — |
| lim_stage1_enabled | TruePeakLimiter | 0,1 | 1 | bool | — | — |
| dither_enabled | Dither | 0,1 | 1 | bool | — | — |
| dither_bits | Dither | 16/24 | 24 | bit | — | — |
| dither_shape | Dither | 0=TPDF,1=HighShibata,2=E-weighted | 1 | enum | — | — |

### Global Parameters
| ID | Range | Default | Description |
|----|-------|---------|-------------|
| module_order[0..11] | 0–11 | 0–11 | Slot → module mapping (drag-reorder) |
| module_bypass[0..11] | 0,1 | 0 | Per-module bypass |
| module_channel[0..11] | 0=Stereo,1=Mid,2=Side,3=L,4=R | 0 | Per-module routing |
| module_wetdry[0..11] | 0–100 | 100 | % wet |
| module_delta[0..11] | 0,1 | 0 | Delta (difference) solo |
| ab_slot | 0–3 | 0 | Active A/B/C/D snapshot |
| output_gain | -24 to +24 | 0 | dB, post-chain trim |
| low_latency_mode | 0,1 | 0 | Switches all linear-phase → min-phase |
| lufs_target | -20 to -6 | -9 | Target for Intelligent Assistant |
| target_profile | string | "R&B Major Label" | Active profile name |

---

## 5. Latency Map

| Configuration | Latency (samples @ 48 kHz) | Latency (ms) |
|---|---|---|
| All minimum-phase, no oversampling, no lookahead | 0 | 0 |
| Low-Latency mode (forced min-phase everywhere) | 0 | 0 |
| CorrectiveEQ linear-phase (8192-tap FIR) | 4096 | 85.3 ms |
| CorrectiveEQ natural-phase (4096-tap FIR) | 2048 | 42.7 ms |
| MultibandDynamics LR FIR crossovers (64-tap) | 32 | 0.67 ms |
| StereoImager LR FIR crossovers (64-tap) | 32 | 0.67 ms |
| Saturation oversampling (16×, poly FIR group delay) | 64 | 1.33 ms |
| Clipper oversampling (8×, poly FIR group delay) | 32 | 0.67 ms |
| TruePeakLimiter stage-1 lookahead (fixed 0.5 ms) | 24 | 0.5 ms |
| TruePeakLimiter stage-2 lookahead (default 1.5 ms) | 72 | 1.5 ms |
| TruePeakLimiter stage-2 detector kernel (2 × 16, any OS factor) | 32 | 0.67 ms |
| TruePeakLimiter backstop (16×, 2 × 64-tap kernel) | 128 | 2.67 ms |
| **Full chain, all linear-phase, default oversampling** | **4288** | **~89 ms** |
| **Full chain, minimum-phase, default oversampling** | **192** | **~4 ms** |

Total latency is computed in `PluginProcessor::updateLatency()`, called whenever phase mode or oversampling parameters change, and reported via `setLatencySamples()`.

---

## 6. Thread Architecture & Real-Time Safety

```
Audio Thread (RT):
  processBlock() → per-module process() → write meters to LockFreeQueue<MeterData,512>
                                         → write spectrum to LockFreeQueue<FFTData,8>

UI/Timer Thread (60 fps Timer callback):
  drain MeterData queue → update meter display
  drain FFTData queue → update spectrum display
  read APVTS state → update parameter displays

Codec Preview Thread (background std::thread):
  encode/decode WAV → read result → post to MainThread via MessageManager::callAsync

Analysis Thread (background std::thread):
  offline analysis pass → post proposals via MessageManager::callAsync
```

All cross-thread communication uses LockFreeQueue<T,N> (SPSC ring buffer, power-of-2 size, std::atomic<> head/tail, no mutex). APVTS parameter reads on the audio thread use RawParameterValue atomic pointers (JUCE standard pattern). No std::mutex, no new/delete, no file I/O on the audio thread.

---

## 7. CPU Budget Estimate (48 kHz / 512 samples / single core)

| Module | Est. CPU (%) |
|---|---|
| CorrectiveEQ (8 biquads, min-phase) | 0.3 |
| LowEndControl | 0.2 |
| DynamicEQ (6 bands) | 0.5 |
| GlueCompressor | 0.3 |
| MultibandDynamics (4 bands, FIR xover) | 0.8 |
| ToneEQ | 0.4 |
| Saturation (4× OS, tape model) | 2.5 |
| StereoImager (4 bands, FIR xover) | 0.8 |
| Clipper (8× OS) | 0.6 |
| TruePeakLimiter (4× OS, 1.5 ms LA) — measured 7.4 % at 44.1 kHz (null_test), dominated by the 16× ±64-tap backstop | 7.4 |
| Dither | 0.05 |
| Metering + FFT | 0.5 |
| **Total (default chain)** | **~8%** |

Target: < 15% at 48 kHz/512. Default configuration estimated at ~8%, leaving headroom for linear-phase mode (+2–3%) and 16× saturation (+3%).

---

## 8. Implementation Notes and Deviations (as built)

| Area | As built | Reason |
|---|---|---|
| TruePeakLimiter backstop | Independent 16× TruePeakDetector (±64-tap Kaiser β10) + GuaranteedGainSmoother (ramp 1, plateau = kernel) at ceiling × 0.99, then a per-sample clamp to ±ceiling. Replaces the "hard clip on the upsampled signal" / 4× "rescale block" backstop. | A clip on the upsampled signal is not preserved by downsampling; 4× can under-read by up to 0.69 dB near Nyquist; a ±24-tap kernel was measured 3.3 dB low at 0.96 × Nyquist and let white noise overshoot by 0.8 dB. The ±64 kernel is within 0.044 dB to 0.96 × Nyquist. |
| TruePeakLimiter latency | 256 samples at 48 kHz (stage 1 24 + stage 2 16 + 72 + 16 + backstop 128); 248 at 44.1 kHz. | Low-latency mode removes both lookaheads; the 160 samples of true-peak kernels remain, because removing them would give up the ceiling guarantee. |
| TruePeakLimiter channel link | Below 150 Hz the LF share of each channel's reduction (LR4 low-pass envelope) is linked in the dB domain with max(). | Filtering the gain signal itself (LP/HP) can raise gain above the requirement and break the ceiling. |
| TruePeakLimiter Warm style | tanh softening applies to stage 1 only. | Softening stage-2 reduction would exceed the ceiling by definition. |
| MeteringEngine channel sum | L = −0.691 + 10·log10(z_L + z_R) (BS.1770-4 sum, G = 1 for L/R). | The 0.5·(z_L + z_R) form reads 3.01 dB low; with the standard sum a −23 dBFS stereo 1 kHz sine reads −23.0 LUFS (EBU Tech 3341 case 1). |
| MeteringEngine K-weighting | 48 kHz BS.1770-4 coefficients; other rates derived by prewarped bilinear transform of the same shelf + high-pass. | BS.1770 only publishes 48 kHz coefficients. |
| SpectrumFrame | 2048-point layout (1025 bins, magnitude / mid / side / peak-hold, −120 dB floor). | Matches the 2048-pt FFT spec. |
| Dither | TPDF (xorshift64) + error-feedback shaping, NTF = 1 − Σ cₖ z⁻ᵏ, error clamped to ±1.5 LSB; bit-depth choice includes 32-bit float (dither off). | Error clamp prevents shaping-loop instability on clipped full-scale material. |
| Saturation multiband split | Minimum-phase LR4 IIR (LinkwitzRileyBank, zero latency). | Keeps the saturation stage latency-free. |
| LinearPhaseCrossover | Top band = delayed input − Σ lower bands, so the band sum equals the delayed input to within double rounding (measured error 0). Partitioned FFT convolution runs in float. | Perfect reconstruction by construction; float FFT only affects the band shapes, not the sum. |
| CodecPreview Opus bitrate | Opus encoded at 160 kb/s (libopus via ffmpeg); AAC 256 kb/s, MP3 320 kb/s. Runs on its own background thread over the last 10 s of the processor capture ring. | 160 kb/s matches what streaming platforms actually deliver over Opus, so the post-encode true-peak reading reflects real playback. |
| IntelligentAssistant tilt | Spectral tilt measured relative to a −24 dB reference master tilt; proposals adjust the Pultec treble boost when the deviation is beyond ±2 dB. | A flat-zero reference would flag every commercially balanced master as dull. |
| Target profiles | The 5 `profiles/*.json` are embedded as BinaryData (`MeridianProfileData`) factory profiles; JSON files in `<UserDocuments>/Meridian Mastering Suite/Profiles/` override a factory profile with the same name or add new ones. | The plugin works with no install step; users can still edit the profiles. |
| GR budget API | `IntelligentAssistant::updateGRBudget (float limiterGrDb, const float* perModuleGrDb)`, called from the processor's meter push. | Per-module GR is already gathered there for the meters, so it is passed as a pointer to the existing array with no copy. |
| UI LookAndFeel | The LookAndFeel and theme constants are in `ui/UITheme.h/.cpp` (`meridian::ui::MeridianLookAndFeel`, `Colours`, `FontSize`, `Radius`). | One file holds the whole visual theme. |
| Processor capture ring | 20 s stereo float ring (`captureL/R`) allocated in `prepareToPlay`. It is written lock-free in `processChunk` before the delta-solo stage, and read by `copyRecentAudio (dest, seconds)` on UI/background threads. | One shared source of post-chain audio for Assistant analysis, codec preview and the goniometer. |
| Reference A/B | `ReferencePanel::renderReference` runs after metering in `processChunk`, so meters always read the processed master. A/B swaps only the monitored output. | Loudness and true-peak readouts stay valid while auditioning the reference. |
| Codec preview window | The last 10 s of captured audio. | Keeps the ffmpeg round trip to a few seconds. |
| Editor gear button | Logs via `DBG` only; the settings live in the module rows and the top bar. | No separate global settings page was specified. |
| releaseResources before prepare | `DSPModuleBase::reset()` and `MeridianAudioProcessor::releaseResources()` are no-ops until `prepareToPlay` has run. | The VST3 wrapper calls `releaseResources` from `terminate()` even if the plugin was never prepared; pluginval's "Open plugin (cold)" test crashed on the unallocated limiter state. |
| ToneEQ | Pultec low boost at 60 Hz (nearest real switch position to ~70 Hz); "+0.5 dB @ 200 Hz wide" on the passive mid bell (Q 0.5); mid Q 0.7 expressed as 1.92-octave bandwidth; `teq_colour` added (default 0, skipped at 0). | Fidelity to the hardware switch positions. |

### Verification programs (`tests/`, CTest)
- `null_test`: pink noise at −6 dBFS through the limiter at a 0 dBTP ceiling, compared with the latency-aligned input (pass < −140 dBFS).
- `limiter_tp_test`: 7 signals × 4/8/16× at −1 dBTP, measured with an independent ±64-tap Blackman-sinc 16× estimator; each signal is followed by silence so the signal's own end is limited too (pass: TP ≤ ceiling, 0 dB tolerance).
- `lufs_test`: 1 kHz sine at −23 dBFS for 10 s at 48 / 44.1 / 96 kHz (pass −23.0 ± 0.2 LUFS).
- `crossover_sum_test`: 1 s white noise, 120 / 1000 / 5000 Hz, band sum vs delayed input (pass < 0.1 dB), plus a per-band energy-share check to prove the split is real.

*End of ARCH.md*
