#pragma once

#include "DSPModuleBase.h"

#include <atomic>

namespace meridian
{

/**
    Input stage: trim, polarity inversion, DC blocking and input metering.

      - Trim: input_trim (dB) -> linear gain, smoothed (20 ms).
      - Polarity: input_polarity flips the sign through a 5 ms gain ramp
        (+1 -> -1), so toggling never clicks; settled value is exactly -1.
      - DC block: first-order high-pass  H(z) = (1 - z^-1) / (1 - R z^-1),
        R = exp(-2 pi * 2 Hz / fs)  (R ~ 0.99974 at 48 kHz). The filter always
        runs; enabling/disabling crossfades over 10 ms.
      - Metering (post-processing): peak (instant attack, 300 ms release) and
        RMS (125 ms exponential integration), written to atomics per channel.
*/
class InputModule final : public DSPModuleBase
{
public:
    explicit InputModule (juce::AudioProcessorValueTreeState& state);

    ModuleId getId() const noexcept override { return ModuleId::Input; }
    void updateParameters() noexcept override;
    void processBlock (double* L, double* R, int numSamples) noexcept override;

    /** Linear peak / RMS for channel 0 (L) or 1 (R). Thread-safe. */
    double getPeak (int channel) const noexcept { return peak[channel & 1].load (std::memory_order_relaxed); }
    double getRms (int channel) const noexcept  { return rms[channel & 1].load (std::memory_order_relaxed); }
    double getPeakDb (int channel) const noexcept { return gainToDb (getPeak (channel)); }
    double getRmsDb (int channel) const noexcept  { return gainToDb (getRms (channel)); }

    static constexpr double kDcCutoffHz = 2.0;
    static constexpr double kRmsWindowMs = 125.0;
    static constexpr double kPeakReleaseMs = 300.0;

protected:
    void prepareModule (double sampleRate, int maxBlockSize) override;
    void resetModule() noexcept override;

private:
    std::atomic<float>* trimParam = nullptr;
    std::atomic<float>* polarityParam = nullptr;
    std::atomic<float>* dcBlockParam = nullptr;

    SmoothParam trimGain;      // linear
    SmoothParam polarityGain;  // +1 / -1
    SmoothParam dcMix;         // 0 = off, 1 = DC-blocked

    double dcR = 0.9997;
    double dcX1[2] {}, dcY1[2] {};

    double peakRelease = 0.0;
    double rmsCoeff = 0.0;
    double peakState[2] {}, meanSquare[2] {};

    std::atomic<double> peak[2] {}, rms[2] {};
};

} // namespace meridian
