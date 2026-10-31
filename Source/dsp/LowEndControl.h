#pragma once

#include "DSPModuleBase.h"
#include "common/Biquad.h"

#include <atomic>

namespace meridian
{

/**
    Low-end control: mono maker + dynamic sub control.

    Signal flow (per sample, double precision):

        L/R -> M = (L+R)/2, S = (L-R)/2
        M, S -> Linkwitz-Riley 4th order split at lec_mono_freq
                (2 x Butterworth LPF + 2 x Butterworth HPF, Q = 0.7071)
        M_out = g_sub * M_lo + M_hi
        S_out = monoGain * S_lo + S_hi
        L = M_out + S_out, R = M_out - S_out

    Both M and S always pass through the same LR4 network, so the LP + HP sum
    is an all-pass with flat magnitude and M/S stay phase-coherent. Enabling
    the mono maker only ramps monoGain from 1 to 0 (15 ms), so toggling it
    never clicks and the disabled state is magnitude-flat.

    Dynamic sub: the Mid signal feeds a band-pass sidechain at 55 Hz
    (Q ~ 1.83, ~30 Hz bandwidth covering 40-70 Hz) and an RMS detector
    (10 ms attack, 120 ms release). Above lec_dyn_threshold a 4:1 downward
    curve is applied, limited to |lec_dyn_maxgr| dB, smoothed by a 1 ms
    one-pole and applied to the M sub band (M_lo) only.

    The crossover frequency glides with a ~100 ms smoother (log domain);
    while gliding, the coefficients are recomputed every 16 samples.
*/
class LowEndControl final : public DSPModuleBase
{
public:
    explicit LowEndControl (juce::AudioProcessorValueTreeState& state);

    ModuleId getId() const noexcept override { return ModuleId::LowEndControl; }
    void updateParameters() noexcept override;
    void processBlock (double* L, double* R, int numSamples) noexcept override;

    /** Current dynamic-sub gain reduction in positive dB (thread-safe). */
    double getSubGainReductionDb() const noexcept { return subGrDb.load (std::memory_order_relaxed); }

    static constexpr double kSidechainFreqHz = 55.0;
    static constexpr double kSidechainQ = 1.83;
    static constexpr double kAttackMs = 10.0;
    static constexpr double kReleaseMs = 120.0;
    static constexpr double kRatio = 4.0;
    static constexpr double kGainSmoothMs = 1.0;
    static constexpr double kFreqGlideMs = 100.0;
    static constexpr int kControlInterval = 16;

protected:
    void prepareModule (double sampleRate, int maxBlockSize) override;
    void resetModule() noexcept override;

private:
    void updateCrossover (double freqHz) noexcept;

    std::atomic<float>* monoFreqParam = nullptr;
    std::atomic<float>* monoEnabledParam = nullptr;
    std::atomic<float>* dynEnabledParam = nullptr;
    std::atomic<float>* dynThresholdParam = nullptr;
    std::atomic<float>* dynMaxGrParam = nullptr;

    SmoothParam logFreq;   // log(Hz)
    SmoothParam monoGain;  // gain of S below the crossover (1 = off, 0 = mono)
    SmoothParam dynMix;    // 0 = dynamic sub off, 1 = on
    SmoothParam engage;    // 0 = exact identity (both sections off), 1 = split path

    // [0] = Mid, [1] = Side; two cascaded sections each
    Biquad lp[2][2], hp[2][2];
    Biquad sidechain;

    double appliedFreq = -1.0;
    int controlCounter = 0;

    double thresholdDb = -12.0;
    double maxGrDb = 3.0;          // positive
    double attackCoeff = 0.0, releaseCoeff = 0.0, gainCoeff = 1.0;
    double meanSquare = 0.0;
    double subGainDb = 0.0;        // smoothed gain (<= 0 dB)

    std::atomic<double> subGrDb { 0.0 };
};

} // namespace meridian
