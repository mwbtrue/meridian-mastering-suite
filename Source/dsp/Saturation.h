#pragma once

#include "DSPModuleBase.h"
#include "common/Biquad.h"
#include "common/DelayLine.h"
#include "common/LinkwitzRileyBank.h"
#include "common/OversamplingEngine.h"

#include <array>
#include <atomic>
#include <vector>

namespace meridian
{

/**
    Oversampled, optionally 3-band, analog-modelled saturation.

    Signal flow (per channel):
        x -> [LR4 split into 3 bands]? -> per band: upsample (1..16x) -> model
          -> downsample -> sum = wet -> auto-gain -> mix with latency-aligned dry

    Every model is normalised to unity small-signal gain (y = f(g x) / g, with
    g = drive), so drive changes the amount of harmonic content, not the
    level, and the dry/wet blend stays phase-coherent.

    Models
      Tape         Rate-independent Dahl / Jiles-Atherton-lite hysteresis, in
                   the field domain H = g x + 0.5 bias:
                       M_irr' = dM_irr/dH = (1 - c) / (2k) * (1 - sign(dH) M_irr / (1 - c))
                       M_rev  = c tanh(H + alpha M)
                       M      = M_irr + M_rev
                   k = 0.5, alpha = 0.001, c = 0.1, so the small-signal slope
                   is exactly (1 - c)/(2k) + c = 1 and M saturates at +/-1.
                   H is linear between oversampled samples; the ODE is
                   integrated with classical RK4, sub-stepped so that every
                   step satisfies |dH| <= 0.25 (stiffness 1/(2k) = 1 per unit
                   H, so RK4 is far inside its stability region) and capped
                   at 64 sub-steps; this keeps the solver stable over the full
                   0-24 dB drive range at any oversampling factor.
                   (The spec's time-domain ODE "dM/dt = (B_eff - M mu0) /
                   (k delta (1 - c))" has no rate term and is not
                   dimensionally a hysteresis law; the field-domain Dahl form
                   above is the standard well-posed version with the same
                   parameters.) Bias offsets H: asymmetric loop, even harmonics.
      Tube         Asymmetric triode: y = tanh(u + beta tanh^2(u)), u = g x,
                   beta = 0.15 + 0.35 bias. The inner term is monotonic for
                   |beta| <= 0.5, so the curve never folds over. The spec's
                   "tanh(u + b u^2) - tanh(b u^2)" cancels the 2nd harmonic
                   to 2nd order (the even term it is meant to create), so the
                   asymmetric part is kept and its DC is removed by the DC
                   blocker instead.
      Transformer  Odd-only cubic core: s(u) = u - u^3/3 for |u| < 1, +/-2/3
                   beyond (C1 continuous), clamped to +/-1.2. Core saturation
                   is LF-dependent: the low-passed input (2nd-order, 40 Hz)
                   is added to the core drive with weight eta = drive/24 dB
                   and subtracted linearly afterwards, so the linear response
                   is untouched while bass drives the core harder. Bias is
                   ignored (a transformer is symmetric: odd harmonics only).

    Multiband (sat_multiband_enabled): minimum-phase LR4 IIR splitter
    (LinkwitzRileyBank, zero latency) instead of the linear-phase crossover:
    saturation runs at up to 16x per band and a 1280-sample FIR crossover
    would add latency the module does not need. The bands sum to an all-pass
    (flat magnitude); the dry path is taken from the same band sum so the mix
    stays phase-aligned. Band drives replace the global drive.

    Auto-gain: 100 ms exponential RMS of dry and wet; the wet signal is scaled
    by RMS_dry / RMS_wet (clamped +/-12 dB, frozen below -80 dBFS, smoothed
    100 ms) before the mix, so the blend itself is level-matched. Disabled
    (unity) when mix = 0 or drive = 0.

    Latency: 63 samples for any factor > 1, 0 at 1x (OversamplingEngine).
*/
class Saturation final : public DSPModuleBase
{
public:
    explicit Saturation (juce::AudioProcessorValueTreeState& state);

    ModuleId getId() const noexcept override { return ModuleId::Saturation; }
    void updateParameters() noexcept override;
    void processBlock (double* L, double* R, int numSamples) noexcept override;

    static constexpr int kMaxFactor = 16;
    static constexpr int kBands = kNumSatBands;

    // Tape model constants
    static constexpr double kTapeK = 0.5;
    static constexpr double kTapeAlpha = 0.001;
    static constexpr double kTapeC = 0.1;

    /** Current auto-gain correction in dB (thread-safe, metering). */
    float getAutoGainDb() const noexcept { return autoGainDb.load (std::memory_order_relaxed); }

protected:
    void prepareModule (double sampleRate, int maxBlockSize) override;
    void resetModule() noexcept override;

private:
    struct ModelState
    {
        double mIrr = 0.0;       // tape irreversible magnetisation
        double mTotal = 0.0;     // tape total magnetisation (for alpha feedback)
        double prevH = 0.0;      // tape previous field
        bool   hInit = false;
        Biquad lfFilter;         // transformer LF detector (oversampled rate)
        double dcX = 0.0, dcY = 0.0;
        void reset() noexcept;
    };

    double processTape (ModelState& s, double x, double g, double bias) const noexcept;
    double processTube (double x, double g, double bias) const noexcept;
    double processTransformer (ModelState& s, double x, double g, double driveDb) const noexcept;
    double dcBlock (ModelState& s, double x) const noexcept;

    void runBand (int band, int ch, double* data, int n, double driveDb) noexcept;
    void configureOversampling (int factor) noexcept;

    OversamplingEngine engine { 2 * kNumSatBands, kMaxFactor };
    LinkwitzRileyBank splitter;

    std::array<std::array<ModelState, 2>, kNumSatBands> states;
    std::array<std::array<std::vector<double>, 2>, kNumSatBands> bandBuf;
    std::vector<double> osBuf, dryL, dryR;
    DelayLine dryDelay[2];

    std::atomic<float>* modelParam = nullptr;
    std::atomic<float>* driveParam = nullptr;
    std::atomic<float>* biasParam = nullptr;
    std::atomic<float>* mixParam = nullptr;
    std::atomic<float>* osParam = nullptr;
    std::atomic<float>* mbParam = nullptr;
    std::atomic<float>* autoGainParam = nullptr;
    std::array<std::atomic<float>*, kNumSatXovers> xoverParams {};
    std::array<std::atomic<float>*, kNumSatBands> bandDriveParams {};

    int model = 0;
    int factor = 1;
    bool multiband = false;
    bool autoGainOn = true;
    double bias = 0.0;
    double driveDb = 0.0;
    std::array<double, kNumSatBands> bandDriveDb {};
    std::array<double, kNumSatXovers> xovers { 200.0, 2000.0 };

    SmoothParam mix, makeup;
    double rmsCoeff = 0.0;
    double msDry = 0.0, msWet = 0.0;
    double dcCoeffOs = 0.0;
    double osRate = 48000.0;
    std::atomic<float> autoGainDb { 0.0f };
};

} // namespace meridian
