#include "Dither.h"

#include <algorithm>
#include <cmath>

namespace meridian
{

namespace
{
    // Round to nearest, ties to even: identical to std::nearbyint in the default
    // rounding mode for |x| < 2^51 (adding 1.5 * 2^52 leaves no fractional bits,
    // so the FPU's own round-to-nearest-even does the work). Inlines to two
    // additions; MSVC compiles std::nearbyint to a CRT call that checks the
    // rounding mode and was 5 % of a core here. Exact under /fp:precise and
    // -fno-fast-math, which forbid reassociating (x + k) - k.
    inline double roundToNearestEven (double x) noexcept
    {
        constexpr double k = 6755399441055744.0;   // 1.5 * 2^52
        if (std::abs (x) < 2251799813685248.0)       // 2^51
            return (x + k) - k;
        return std::nearbyint (x);
    }
}

Dither::Dither (juce::AudioProcessorValueTreeState& state) : DSPModuleBase (state)
{
    enabledParam = rawParam (pid::ditherEnabled);
    bitsParam    = rawParam (pid::ditherBits);
    shapeParam   = rawParam (pid::ditherShape);
}

void Dither::seed() noexcept
{
    // Four distinct non-zero seeds (xorshift state must never be 0).
    channels[0].a.state = 0x9E3779B97F4A7C15ull;
    channels[0].b.state = 0xD1B54A32D192ED03ull;
    channels[1].a.state = 0x8CB92BA72F3D8DD7ull;
    channels[1].b.state = 0xABC98388FB8FAC03ull;

    for (auto& ch : channels)
        ch.error.fill (0.0);
}

void Dither::prepareModule (double, int)
{
    seed();
    setLatencySamples (0);
}

void Dither::resetModule() noexcept
{
    seed();
}

void Dither::updateParameters() noexcept
{
    const int bitsIndex = std::clamp (loadInt (bitsParam), 0, (int) choices::ditherBits.size() - 1);
    bits = choices::ditherBits[(size_t) bitsIndex];
    active = loadBool (enabledParam) && bits < 32;

    const int newShape = std::clamp (loadInt (shapeParam), 0, 2);
    if (newShape != shape)
    {
        shape = newShape;
        for (auto& ch : channels)
            ch.error.fill (0.0);
    }

    switch ((choices::DitherShape) shape)
    {
        case choices::DitherShape::HighShibata: coeffs = &kHighShibata; numTaps = 9; break;
        case choices::DitherShape::EWeighted:   coeffs = &kEWeighted;   numTaps = 3; break;
        case choices::DitherShape::TPDF:
        default:                                coeffs = nullptr;       numTaps = 0; break;
    }

    setLatencySamples (0);
}

void Dither::processBlock (double* L, double* R, int numSamples) noexcept
{
    if (! active)
        return;

    const double lsb = 2.0 / std::pow (2.0, (double) bits);
    const double invLsb = 1.0 / lsb;
    const double maxOut = 1.0 - lsb;

    double* data[2] = { L, R };

    for (int c = 0; c < 2; ++c)
    {
        auto& ch = channels[(size_t) c];
        double* x = data[c];

        for (int i = 0; i < numSamples; ++i)
        {
            double fb = 0.0;
            for (int k = 0; k < numTaps; ++k)
                fb += (*coeffs)[(size_t) k] * ch.error[(size_t) k];

            const double v = x[i] - fb * lsb;
            const double d = ch.a.uniform() - ch.b.uniform();
            const double w = v + d * lsb;
            const double y = std::clamp (roundToNearestEven (w * invLsb) * lsb, -1.0, maxOut);

            if (numTaps > 0)
            {
                for (int k = numTaps - 1; k > 0; --k)
                    ch.error[(size_t) k] = ch.error[(size_t) (k - 1)];
                ch.error[0] = std::clamp ((y - v) * invLsb, -kErrorClampLsb, kErrorClampLsb);
            }

            x[i] = y;
        }
    }
}

} // namespace meridian
