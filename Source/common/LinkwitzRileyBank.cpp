#include "LinkwitzRileyBank.h"

#include <algorithm>
#include <cmath>

namespace meridian
{

namespace
{
    constexpr double kButterworthQ = 0.70710678118654752;
}

void LinkwitzRileyBank::prepare (double sampleRate) noexcept
{
    fs = sampleRate > 0.0 ? sampleRate : 48000.0;
    freqs.fill (0.0);   // force a full coefficient refresh on the next setCrossovers()
    reset();
}

void LinkwitzRileyBank::reset() noexcept
{
    for (auto& c : channels)
    {
        for (auto& x : c.xo)
            for (int s = 0; s < 2; ++s)
            {
                x.lp[s].reset();
                x.hp[s].reset();
            }

        for (auto& a : c.ap)
            a.reset();
    }
}

void LinkwitzRileyBank::applyCoefficients (int index) noexcept
{
    const double f = freqs[(size_t) index];
    const auto lp = Biquad::makeLPF (f, kButterworthQ, fs);
    const auto hp = Biquad::makeHPF (f, kButterworthQ, fs);

    for (auto& c : channels)
        for (int s = 0; s < 2; ++s)
        {
            c.xo[(size_t) index].lp[s].setCoefficients (lp);
            c.xo[(size_t) index].hp[s].setCoefficients (hp);
        }
}

void LinkwitzRileyBank::setCrossovers (const double* newFreqs, int bands) noexcept
{
    bands = std::clamp (bands, 2, kMaxBands);
    const bool topologyChanged = bands != numBands;
    numBands = bands;

    double prev = 10.0;
    for (int i = 0; i < numBands - 1; ++i)
    {
        // Ascending, at least 1/6 octave apart, inside (10 Hz, 0.45 fs).
        double f = std::clamp (newFreqs[i], 10.0, 0.45 * fs);
        f = std::max (f, prev * 1.122462048309373);
        f = std::min (f, 0.45 * fs);
        prev = f;

        if (f != freqs[(size_t) i] || topologyChanged)
        {
            freqs[(size_t) i] = f;
            applyCoefficients (i);
        }
    }

    // Compensation all-passes depend on the topology.
    BiquadCoeffs ap0 = BiquadCoeffs::identity(), ap1 = BiquadCoeffs::identity();

    if (numBands == 3)
        ap0 = Biquad::makeAllPass (freqs[1], kButterworthQ, fs);              // on band 0
    else if (numBands == 4)
    {
        ap0 = Biquad::makeAllPass (freqs[2], kButterworthQ, fs);              // on low branch
        ap1 = Biquad::makeAllPass (freqs[0], kButterworthQ, fs);              // on high branch
    }

    for (auto& c : channels)
    {
        if (c.ap[0].getCoefficients() != ap0) c.ap[0].setCoefficients (ap0);
        if (c.ap[1].getCoefficients() != ap1) c.ap[1].setCoefficients (ap1);
    }

    if (topologyChanged)
        reset();
}

void LinkwitzRileyBank::processSample (int ch, double x, double* bands) noexcept
{
    auto& c = channels[(size_t) ch];

    switch (numBands)
    {
        case 2:
            bands[0] = c.xo[0].lowPass (x);
            bands[1] = c.xo[0].highPass (x);
            break;

        case 3:
        {
            bands[0] = c.ap[0].process (c.xo[0].lowPass (x));
            const double rest = c.xo[0].highPass (x);
            bands[1] = c.xo[1].lowPass (rest);
            bands[2] = c.xo[1].highPass (rest);
            break;
        }

        case 4:
        default:
        {
            const double lo = c.ap[0].process (c.xo[1].lowPass (x));
            const double hi = c.ap[1].process (c.xo[1].highPass (x));
            bands[0] = c.xo[0].lowPass (lo);
            bands[1] = c.xo[0].highPass (lo);
            bands[2] = c.xo[2].lowPass (hi);
            bands[3] = c.xo[2].highPass (hi);
            break;
        }
    }
}

void LinkwitzRileyBank::snapToZero() noexcept
{
    for (auto& c : channels)
    {
        for (auto& x : c.xo)
            for (int s = 0; s < 2; ++s)
            {
                x.lp[s].snapToZero();
                x.hp[s].snapToZero();
            }

        for (auto& a : c.ap)
            a.snapToZero();
    }
}

} // namespace meridian
