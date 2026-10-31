#pragma once

#include "DSPModuleBase.h"
#include "common/Biquad.h"
#include "common/DelayLine.h"
#include "common/TruePeakDetector.h"

#include <array>
#include <atomic>

namespace meridian
{

/**
    Two-stage true-peak limiter with a hard ceiling guarantee.

    Signal flow (all per sample, base rate, double precision):

      input -> [Stage 1: transient catcher] -> [Stage 2: true-peak limiter]
            -> [Backstop: independent 16x true-peak limiter] -> [sample clamp] -> out

    Stage 1 (lim_stage1_enabled)
      Fixed 0.5 ms lookahead, stereo-linked detection d = max(|L|, |R|),
      threshold = ceiling + 1.5 dB, ratio 100:1:
          GR_dB = (1 - 1/100) * max(0, 20 log10(d) - thr_dB)
      Warm style soft-saturates the GR value: GR = tanh(1.1 GR) / 1.1.
      The required gain feeds a GuaranteedGainSmoother whose ramp length is
      the style attack (0.005 - 0.02 ms) and whose plateau pads the delay to
      exactly 0.5 ms, so the gain is fully applied when the transient reaches
      the output; release 2 - 8 ms (style). Stage 1 only shapes transients so
      stage 2 has less to do; it carries no guarantee. When disabled the
      audio still runs through the 0.5 ms delay (latency never changes).

    Stage 2 (true-peak, lim_lookahead 0 - 5 ms)
      TruePeakDetector per channel at lim_oversampling (4/8/16x): for every
      base sample c it evaluates the band-limited reconstruction at the M
      oversampled positions in [c, c + 1) and returns their max magnitude.
      G_req is therefore computed per upsampled sample
          G_req = min(1, c2 / max(1e-9, |x_up|))
      and reduced to the base rate by taking the MIN over the M phases
      (equivalently, from the peak). This "min-decimation" is stricter than
      the nearest-neighbour or linear decimation suggested in the brief: it
      can never miss an oversampled peak that falls between base samples.
      c2 = ceiling * 0.99 (-0.087 dB) leaves room for the backstop margin.

      Channel link: r_blend = link * min(rL, rR) + (1 - link) * r_ch.
      Below 150 Hz the reduction is always linked: an LR4 low-pass at 150 Hz
      measures each channel's LF peak; the LF share of a channel's reduction,
          GR_lf = GR_ch * clamp(|lf| / tp, 0, 1)   (in dB)
      is taken as max over both channels and applied to both:
          GR_final = max(GR_blend, GR_lf_linked)
      (The brief proposed LP/HP filtering of the GR signal itself; filtering a
      gain trajectory can raise it above the required gain and break the
      ceiling guarantee. Combining in the dB domain with max() keeps
      GR_final >= GR_ch, so the guarantee is preserved.)

      GuaranteedGainSmoother: ramp K = style attack (clamped to the
      lookahead), plateau S = 2 * halfSpan + 2 * (L - K + 1) so the smoother
      delay is exactly L + halfSpan. Guarantee: applied gain <= G_req for
      every sample, and an isolated peak sees constant gain over the whole
      reconstruction kernel, so its true peak is scaled exactly.

    Backstop (final mathematical guarantee)
      A second, independent TruePeakDetector at 16x with a +/-64-tap Kaiser
      (beta 10) kernel measures the stage-2 output; a GuaranteedGainSmoother
      with ramp 1 and a plateau covering that kernel enforces
      cb = ceiling * 0.99 (-0.087 dB). The long kernel is required: a
      +/-24-tap kernel reads 3.3 dB low at 0.96 x Nyquist (measured), so
      broadband material (white noise, the end of a full-scale sweep)
      overshot the ceiling by up to 0.8 dB against an independent +/-64-tap
      16x meter. The +/-64 kernel is within 0.044 dB up to 0.96 x Nyquist;
      the margin covers that plus the 0.042 dB worst-case 16x inter-phase
      under-read. The brief asked for a 4x detector with a block rescale:
        - 4x can under-read a true peak by up to 0.69 dB near Nyquist, so
          it cannot back a "never exceed" claim measured with a 16x meter;
        - a rescale after the fact cannot correct a peak whose reconstruction
          depends on samples not yet received, and a block-rate gain step
          is itself audible. A lookahead limiter with a plateau gives the
          same guarantee without either problem.
      Finally every output sample is clamped to +/- ceiling.

    Styles (lim_style)       stage-2 attack  release (auto range)   stage-1 att / rel
      Transparent            0.5 ms          200 ms (100 - 400)     0.01 ms / 5 ms
      Punchy                 0.3 ms           80 ms ( 50 - 160)     0.01 ms / 3 ms
      Warm                   1.0 ms          150 ms (100 - 300)     0.02 ms / 8 ms, tanh GR
      Aggressive             0.05 ms          20 ms ( 10 -  60)     0.005 ms / 2 ms

    Auto release (lim_auto_release): 200 ms exponential RMS of the stage-2
    input (max of both channels' power); a separate 1-pole follower (1 s)
    tracks it in dB. trend = rms_dB - follower_dB; release = base *
    2^(-clamp(trend / 6 dB, -1, 1)), clamped to the style range: rising
    programme shortens the release, falling programme lengthens it.
    Manual: release = lim_release.

    Warm's tanh is applied to stage 1 only: softening stage-2 reduction
    would by definition let the signal exceed the ceiling.

    Latency (exact, reported):
        stage1 (0.5 ms) + halfSpan (16) + lookahead + halfSpan (16) + 2 * 64 (backstop)
      = 256 samples at 48 kHz with the 1.5 ms default.
    low_latency_mode: stage-1 and stage-2 lookahead are forced to 0; the
    remaining 160 samples (3.3 ms at 48 kHz) are the true-peak kernels and
    cannot be removed without giving up the ceiling guarantee.

    GR metering: getStage2GainReductionDb() (<= 0 dB, the brief's
    convention); ModuleBase::getGainReductionDb() reports the total
    reduction as a positive value (the processor's meter convention).
*/
class TruePeakLimiter final : public DSPModuleBase
{
public:
    explicit TruePeakLimiter (juce::AudioProcessorValueTreeState& state);

    ModuleId getId() const noexcept override { return ModuleId::TruePeakLimiter; }
    void updateParameters() noexcept override;
    void processBlock (double* L, double* R, int numSamples) noexcept override;

    /** Stage-2 gain reduction in dB (<= 0), thread-safe. */
    float getStage2GainReductionDb() const noexcept { return stage2GrDb.load (std::memory_order_relaxed); }

    static constexpr int kDetectorHalfSpan = 16;
    static constexpr double kDetectorBeta = 8.0;
    static constexpr int kBackstopHalfSpan = 64;
    static constexpr double kBackstopBeta = 10.0;
    static constexpr int kBackstopFactor = 16;
    static constexpr double kStage2Margin = 0.99;
    static constexpr double kBackstopMargin = 0.99;
    static constexpr double kStage1ThresholdOffsetDb = 1.5;
    static constexpr double kStage1LookaheadMs = 0.5;
    static constexpr double kMaxLookaheadMs = 5.0;
    static constexpr double kLinkCrossoverHz = 150.0;

    struct StyleSettings
    {
        double stage2AttackMs, releaseMs, releaseMinMs, releaseMaxMs;
        double stage1AttackMs, stage1ReleaseMs;
        bool warmGr;
    };

    static const StyleSettings& getStyle (int style) noexcept;

protected:
    void prepareModule (double sampleRate, int maxBlockSize) override;
    void resetModule() noexcept override;

private:
    void configure (bool resetState) noexcept;
    double releaseCoeff (double ms) const noexcept;
    int computeLatency() const noexcept;

    std::atomic<float>* ceilingParam = nullptr;
    std::atomic<float>* lookaheadParam = nullptr;
    std::atomic<float>* releaseParam = nullptr;
    std::atomic<float>* autoReleaseParam = nullptr;
    std::atomic<float>* styleParam = nullptr;
    std::atomic<float>* linkParam = nullptr;
    std::atomic<float>* oversamplingParam = nullptr;
    std::atomic<float>* stage1Param = nullptr;
    std::atomic<float>* lowLatencyParam = nullptr;

    // Parameter state
    double ceilingLin = 0.891250938;
    double link = 1.0;
    double manualReleaseMs = 100.0;
    bool autoRelease = true;
    bool stage1Enabled = true;
    int style = 0;
    int lookaheadSamples = 0;
    int stage1Lookahead = 0;
    bool lowLatency = false;

    // Configured structure (changes only when lookahead/style/low-latency change)
    int cfgStyle = -1, cfgLookahead = -1, cfgStage1 = -1;
    int stage1Delay = 0, stage2Delay = 0, backstopDelay = 0;

    // Stage 1
    GuaranteedGainSmoother stage1Smoother;
    DelayLine stage1Line[2];
    double stage1Rel = 0.0;

    // Stage 2
    TruePeakDetector detector;
    GuaranteedGainSmoother stage2Smoother[2];
    DelayLine stage2Line[2];
    std::array<std::array<Biquad, 2>, 2> lfLowPass;   // [channel][section], LR4 at 150 Hz
    double lfEnv[2] {};
    double lfEnvRelease = 0.0;

    // Auto release
    double rmsPower = 0.0, rmsCoeff = 0.0;
    double followerDb = -120.0, followerCoeff = 0.0;
    double currentReleaseMs = 200.0;

    // Backstop
    TruePeakDetector backstopDetector;
    GuaranteedGainSmoother backstopSmoother[2];
    DelayLine backstopLine[2];
    double backstopRel = 0.0;

    double lastGain = 1.0;
    std::atomic<float> stage2GrDb { 0.0f };
};

} // namespace meridian
