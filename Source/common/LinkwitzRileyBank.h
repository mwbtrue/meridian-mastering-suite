#pragma once

#include "Biquad.h"

#include <array>

namespace meridian
{

/**
    Minimum-phase stereo Linkwitz-Riley band splitter (2, 3 or 4 bands).

    Each crossover is an LR4 pair (two cascaded 2nd-order Butterworth sections,
    Q = 1/sqrt(2)). LR4 low-pass + high-pass sums to the 2nd-order all-pass
        AP(s) = (s^2 - sqrt(2) s + 1) / (s^2 + sqrt(2) s + 1),
    so every branch that does not pass through a given crossover receives that
    crossover's all-pass instead. The band sum is therefore the cascade of all
    crossover all-passes: magnitude exactly flat (to rounding), phase rotated.

    Topologies (f0 < f1 < f2):
        2 bands: b0 = LP(f0) x, b1 = HP(f0) x
        3 bands: b0 = AP(f1) LP(f0) x, rest = HP(f0) x, b1 = LP(f1) rest, b2 = HP(f1) rest
        4 bands: lo = AP(f2) LP(f1) x, hi = AP(f0) HP(f1) x,
                 b0 = LP(f0) lo, b1 = HP(f0) lo, b2 = LP(f2) hi, b3 = HP(f2) hi

    Zero latency, allocation-free, real-time safe.
*/
class LinkwitzRileyBank
{
public:
    static constexpr int kMaxBands = 4;
    static constexpr int kMaxCrossovers = kMaxBands - 1;
    static constexpr int kChannels = 2;

    void prepare (double sampleRate) noexcept;
    void reset() noexcept;

    /** Sets numBands (2..4) and numBands - 1 ascending crossover frequencies.
        Coefficients are recomputed only for crossovers that changed. */
    void setCrossovers (const double* freqs, int numBands) noexcept;

    int getNumBands() const noexcept { return numBands; }

    /** Splits one sample of channel ch into getNumBands() band outputs. */
    void processSample (int ch, double x, double* bands) noexcept;

    /** Flushes denormals from all filter states (call once per block). */
    void snapToZero() noexcept;

private:
    struct LR4
    {
        Biquad lp[2], hp[2];
        double lowPass (double x) noexcept  { return lp[1].process (lp[0].process (x)); }
        double highPass (double x) noexcept { return hp[1].process (hp[0].process (x)); }
    };

    struct Channel
    {
        std::array<LR4, kMaxCrossovers> xo;
        std::array<Biquad, 2> ap;   // compensation all-passes
    };

    void applyCoefficients (int index) noexcept;

    double fs = 48000.0;
    int numBands = 2;
    std::array<double, kMaxCrossovers> freqs { 0.0, 0.0, 0.0 };
    std::array<Channel, kChannels> channels;
};

} // namespace meridian
