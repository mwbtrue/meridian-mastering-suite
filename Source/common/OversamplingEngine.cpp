#include "OversamplingEngine.h"
#include "DotProduct.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace meridian
{

namespace
{
    double besselI0 (double x) noexcept
    {
        const double halfX = 0.5 * x;
        double sum = 1.0;
        double term = 1.0;

        for (int k = 1; k < 64; ++k)
        {
            const double f = halfX / static_cast<double> (k);
            term *= f * f;
            sum += term;

            if (term < sum * 1.0e-17)
                break;
        }

        return sum;
    }

    int sanitiseFactor (int requested, int maxAllowed) noexcept
    {
        int f = 1;
        while (f * 2 <= requested && f * 2 <= maxAllowed && f < 16)
            f *= 2;
        return f;
    }
}

//==============================================================================
int OversamplingEngine::tableIndexForFactor (int f) noexcept
{
    switch (f)
    {
        case 2:  return 1;
        case 4:  return 2;
        case 8:  return 3;
        case 16: return 4;
        default: return 0;
    }
}

OversamplingEngine::FactorTables OversamplingEngine::buildTables (int f)
{
    FactorTables t;
    t.factor = f;

    if (f <= 1)
    {
        t.length = 1;
        t.phaseLength = 1;
        t.prototype = { 1.0 };
        t.polyphase = { 1.0 };
        return t;
    }

    const int L = getFilterLength (f);
    t.length = L;
    t.phaseLength = tapsPerPhase + 1;
    t.prototype.resize ((size_t) L);

    // Windowed-sinc low-pass at the original Nyquist, expressed at the high rate.
    const double beta = 0.1102 * (stopbandAttenuationDb - 8.7);
    const double cutoff = 0.5 * (passbandEdge + stopbandEdge) / static_cast<double> (f); // cycles/sample
    const double centre = 0.5 * static_cast<double> (L - 1);
    const double i0Beta = besselI0 (beta);

    double sum = 0.0;

    for (int n = 0; n < L; ++n)
    {
        const double x = static_cast<double> (n) - centre;
        const double arg = 2.0 * cutoff * x;
        const double sinc = (x == 0.0) ? 1.0 : std::sin (std::numbers::pi * arg) / (std::numbers::pi * arg);
        const double r = x / centre;
        const double w = besselI0 (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0Beta;
        const double h = 2.0 * cutoff * sinc * w;
        t.prototype[(size_t) n] = h;
        sum += h;
    }

    for (auto& h : t.prototype)
        h /= sum;

    // Polyphase decomposition for interpolation: phase p, tap k -> h[k*M + p] * M.
    t.polyphase.assign ((size_t) (f * t.phaseLength), 0.0);

    for (int p = 0; p < f; ++p)
        for (int k = 0; k < t.phaseLength; ++k)
        {
            const int idx = k * f + p;
            if (idx < L)
                t.polyphase[(size_t) (p * t.phaseLength + k)] = t.prototype[(size_t) idx] * static_cast<double> (f);
        }

    return t;
}

const std::array<OversamplingEngine::FactorTables, 5>& OversamplingEngine::getTables()
{
    static const std::array<FactorTables, 5> tables = []
    {
        std::array<FactorTables, 5> t;
        t[0] = buildTables (1);
        t[1] = buildTables (2);
        t[2] = buildTables (4);
        t[3] = buildTables (8);
        t[4] = buildTables (16);
        return t;
    }();

    return tables;
}

const std::vector<double>& OversamplingEngine::getPrototype (int f)
{
    return getTables()[(size_t) tableIndexForFactor (f)].prototype;
}

//==============================================================================
OversamplingEngine::OversamplingEngine (int maxChans, int maxFact)
    : numChannels (std::max (1, maxChans)),
      maxFactor (sanitiseFactor (std::max (1, maxFact), 16))
{
    const int maxLength = getFilterLength (maxFactor);

    channels.resize ((size_t) numChannels);

    for (auto& c : channels)
    {
        c.upHistory.assign ((size_t) (2 * (tapsPerPhase + 1)), 0.0);
        c.downHistory.assign ((size_t) (2 * maxLength), 0.0);
    }

    active = &getTables()[0];
    factor = 1;
}

void OversamplingEngine::setOversamplingFactor (int requested) noexcept
{
    const int f = sanitiseFactor (requested, maxFactor);

    if (f == factor && active != nullptr)
        return;

    factor = f;
    active = &getTables()[(size_t) tableIndexForFactor (f)];
    reset();
}

void OversamplingEngine::reset() noexcept
{
    for (auto& c : channels)
    {
        std::fill (c.upHistory.begin(), c.upHistory.end(), 0.0);
        std::fill (c.downHistory.begin(), c.downHistory.end(), 0.0);
        c.upPos = 0;
        c.downPos = 0;
        c.downPhase = 0;
    }
}

//==============================================================================
int OversamplingEngine::upsample (const double* in, double* out, int numSamplesIn, int channel) noexcept
{
    if (numSamplesIn <= 0 || channel < 0 || channel >= numChannels)
        return 0;

    if (factor == 1)
    {
        std::copy (in, in + numSamplesIn, out);
        return numSamplesIn;
    }

    auto& c = channels[(size_t) channel];
    const int H = active->phaseLength;              // history length (mirrored to 2H)
    const double* poly = active->polyphase.data();
    double* hist = c.upHistory.data();
    int pos = c.upPos;

    for (int n = 0; n < numSamplesIn; ++n)
    {
        // Newest sample at hist[pos]; hist[pos + k] == x[n - k] for k < H.
        pos = (pos == 0) ? H - 1 : pos - 1;
        hist[pos] = in[n];
        hist[pos + H] = in[n];

        const double* x = hist + pos;
        double* y = out + n * factor;

        for (int p = 0; p < factor; ++p)
        {
            y[p] = dotProduct (poly + p * H, x, H);
        }
    }

    c.upPos = pos;
    return numSamplesIn * factor;
}

int OversamplingEngine::downsample (const double* in, double* out, int numSamplesIn, int channel) noexcept
{
    if (numSamplesIn <= 0 || channel < 0 || channel >= numChannels)
        return 0;

    if (factor == 1)
    {
        std::copy (in, in + numSamplesIn, out);
        return numSamplesIn;
    }

    auto& c = channels[(size_t) channel];
    const int L = active->length;
    const double* h = active->prototype.data();
    double* hist = c.downHistory.data();
    int pos = c.downPos;
    int phase = c.downPhase;
    int written = 0;

    for (int n = 0; n < numSamplesIn; ++n)
    {
        pos = (pos == 0) ? L - 1 : pos - 1;
        hist[pos] = in[n];
        hist[pos + L] = in[n];

        if (phase == 0)
        {
            out[written++] = symmetricDotProduct (h, hist + pos, L);   // linear-phase prototype
        }

        if (++phase == factor)
            phase = 0;
    }

    c.downPos = pos;
    c.downPhase = phase;
    return written;
}

} // namespace meridian
