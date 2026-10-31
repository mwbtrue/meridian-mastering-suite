#pragma once

#include "DSPModuleBase.h"
#include "common/DelayLine.h"
#include "common/OversamplingEngine.h"

#include <atomic>
#include <vector>

namespace meridian
{

/**
    8x oversampled peak clipper (soft / hard / knee).

    Per oversampled sample, with c = ceiling (linear) and d = drive (linear):
        u = d x
      Soft  y = c tanh(u / c)
      Hard  y = clamp(u, -c, c)
      Knee  knee width w = min(0.5 d c, 1.5 c), knee start s = c - 2w/3
            |u| <= s           y = u
            s < |u| < s + w    y = sign(u) (|u| - (|u| - s)^3 / (3 w^2))
            |u| >= s + w       y = sign(u) c
            The cubic has slope 1 at s and slope 0 at s + w where it reaches
            exactly c, so the curve is C1 and lands on the ceiling. (With the
            brief's s = c - w the cubic would top out at c - w/3 and need a
            discontinuous jump to c; s = c - 2w/3 keeps the same polynomial
            and fixes the join.)
        y = clamp(y, -c, c)       absolute guarantee in the oversampled domain

    Ceiling: lim_ceiling when clipper_link_ceiling is on, else clipper_ceiling
    (dBTP -> linear). Drive and ceiling are smoothed (20 ms).

    The guarantee is in the oversampled domain, before decimation. The
    decimation filter (linear-phase, 0.45 fs passband) can create small
    overshoots at the base rate; the TruePeakLimiter that follows enforces
    the true-peak ceiling.

    clipper_enabled = off: latency-matched pass-through (52-sample delay), so
    toggling never changes the reported latency.

    Latency: 52 samples (OversamplingEngine round trip at 8x).
*/
class Clipper final : public DSPModuleBase
{
public:
    explicit Clipper (juce::AudioProcessorValueTreeState& state);

    ModuleId getId() const noexcept override { return ModuleId::Clipper; }
    void updateParameters() noexcept override;
    void processBlock (double* L, double* R, int numSamples) noexcept override;

    static constexpr int kFactor = 8;

    /** Static curve (exposed for tests / UI): u already includes drive. */
    static double shape (int mode, double u, double ceiling, double driveLin) noexcept;

protected:
    void prepareModule (double sampleRate, int maxBlockSize) override;
    void resetModule() noexcept override;

private:
    OversamplingEngine engine { 2, kFactor };
    std::vector<double> osBuf, ceilBuf, driveBuf;
    DelayLine bypassDelay[2];

    std::atomic<float>* modeParam = nullptr;
    std::atomic<float>* ceilingParam = nullptr;
    std::atomic<float>* driveParam = nullptr;
    std::atomic<float>* enabledParam = nullptr;
    std::atomic<float>* linkParam = nullptr;
    std::atomic<float>* limCeilingParam = nullptr;

    int mode = 0;
    bool enabled = true;
    SmoothParam ceiling, drive;
};

} // namespace meridian
