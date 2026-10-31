#include "Biquad.h"

#include <algorithm>

namespace meridian
{

namespace
{
    constexpr double kPi = 3.14159265358979323846;

    struct Trig
    {
        double cosw, sinw;
    };

    inline Trig trigFor (double freq, double sampleRate) noexcept
    {
        const double w0 = 2.0 * kPi * Biquad::clampFrequency (freq, sampleRate) / sampleRate;
        return { std::cos (w0), std::sin (w0) };
    }

    inline double safeQ (double Q) noexcept { return std::max (Q, 1.0e-3); }

    inline BiquadCoeffs normalise (double b0, double b1, double b2, double a0, double a1, double a2) noexcept
    {
        const double inv = 1.0 / a0;
        return { b0 * inv, b1 * inv, b2 * inv, a1 * inv, a2 * inv };
    }

    /** 4th-order elliptic low-pass prototype (passband edge 1 rad/s, 0.1 dB
        ripple, 80 dB stopband), split into two sections with each pole pair
        matched to its nearest transmission zero. Values from the exact
        Jacobi-elliptic design (equivalent to scipy.signal.ellip(4, 0.1, 80, 1, analog=True)). */
    struct EllipticSection
    {
        double zeroW;    // transmission-zero frequency (rad/s)
        double poleRe;   // pole real part
        double poleIm;   // pole imaginary part (positive)
    };

    constexpr EllipticSection kElliptic4[2]
    {
        { 6.16347372,  -0.25860416, 1.12214088 },
        { 14.79811842, -0.64148588, 0.47232332 }
    };

    constexpr double kEllipticRippleDb = 0.1;
}

//==============================================================================
BiquadCoeffs Biquad::makeBell (double freq, double Q, double gainDB, double sampleRate) noexcept
{
    const auto t = trigFor (freq, sampleRate);
    const double A = std::pow (10.0, gainDB / 40.0);
    const double alpha = t.sinw / (2.0 * safeQ (Q));

    return normalise (1.0 + alpha * A, -2.0 * t.cosw, 1.0 - alpha * A,
                      1.0 + alpha / A, -2.0 * t.cosw, 1.0 - alpha / A);
}

BiquadCoeffs Biquad::makeLowShelf (double freq, double Q, double gainDB, double sampleRate) noexcept
{
    const auto t = trigFor (freq, sampleRate);
    const double A = std::pow (10.0, gainDB / 40.0);
    const double alpha = t.sinw / (2.0 * safeQ (Q));
    const double k = 2.0 * std::sqrt (A) * alpha;

    return normalise (A * ((A + 1.0) - (A - 1.0) * t.cosw + k),
                      2.0 * A * ((A - 1.0) - (A + 1.0) * t.cosw),
                      A * ((A + 1.0) - (A - 1.0) * t.cosw - k),
                      (A + 1.0) + (A - 1.0) * t.cosw + k,
                      -2.0 * ((A - 1.0) + (A + 1.0) * t.cosw),
                      (A + 1.0) + (A - 1.0) * t.cosw - k);
}

BiquadCoeffs Biquad::makeHighShelf (double freq, double Q, double gainDB, double sampleRate) noexcept
{
    const auto t = trigFor (freq, sampleRate);
    const double A = std::pow (10.0, gainDB / 40.0);
    const double alpha = t.sinw / (2.0 * safeQ (Q));
    const double k = 2.0 * std::sqrt (A) * alpha;

    return normalise (A * ((A + 1.0) + (A - 1.0) * t.cosw + k),
                      -2.0 * A * ((A - 1.0) + (A + 1.0) * t.cosw),
                      A * ((A + 1.0) + (A - 1.0) * t.cosw - k),
                      (A + 1.0) - (A - 1.0) * t.cosw + k,
                      2.0 * ((A - 1.0) - (A + 1.0) * t.cosw),
                      (A + 1.0) - (A - 1.0) * t.cosw - k);
}

BiquadCoeffs Biquad::makeNotch (double freq, double Q, double sampleRate) noexcept
{
    const auto t = trigFor (freq, sampleRate);
    const double alpha = t.sinw / (2.0 * safeQ (Q));
    return normalise (1.0, -2.0 * t.cosw, 1.0, 1.0 + alpha, -2.0 * t.cosw, 1.0 - alpha);
}

BiquadCoeffs Biquad::makeHPF (double freq, double Q, double sampleRate) noexcept
{
    const auto t = trigFor (freq, sampleRate);
    const double alpha = t.sinw / (2.0 * safeQ (Q));
    const double b = 1.0 + t.cosw;
    return normalise (0.5 * b, -b, 0.5 * b, 1.0 + alpha, -2.0 * t.cosw, 1.0 - alpha);
}

BiquadCoeffs Biquad::makeLPF (double freq, double Q, double sampleRate) noexcept
{
    const auto t = trigFor (freq, sampleRate);
    const double alpha = t.sinw / (2.0 * safeQ (Q));
    const double b = 1.0 - t.cosw;
    return normalise (0.5 * b, b, 0.5 * b, 1.0 + alpha, -2.0 * t.cosw, 1.0 - alpha);
}

BiquadCoeffs Biquad::makeBPF (double freq, double Q, double sampleRate) noexcept
{
    const auto t = trigFor (freq, sampleRate);
    const double alpha = t.sinw / (2.0 * safeQ (Q));
    return normalise (alpha, 0.0, -alpha, 1.0 + alpha, -2.0 * t.cosw, 1.0 - alpha);
}

BiquadCoeffs Biquad::makeAllPass (double freq, double Q, double sampleRate) noexcept
{
    const auto t = trigFor (freq, sampleRate);
    const double alpha = t.sinw / (2.0 * safeQ (Q));
    return normalise (1.0 - alpha, -2.0 * t.cosw, 1.0 + alpha, 1.0 + alpha, -2.0 * t.cosw, 1.0 - alpha);
}

BiquadCoeffs Biquad::makeTilt (double freq, double gainDB, double sampleRate) noexcept
{
    // Analog prototype normalised to w0 = 1:
    //   H(s) = sqrt(A) * (s + 1/sqrt(A)) / (s + sqrt(A)),  A = 10^(gainDB/20)
    // DC gain 1/sqrt(A), HF gain sqrt(A), unity gain at w0 (geometric centre).
    const double f = clampFrequency (freq, sampleRate);
    const double A = std::pow (10.0, gainDB / 20.0);
    const double rootA = std::sqrt (A);
    const double c = 1.0 / std::tan (kPi * f / sampleRate);
    const double z0 = 1.0 / rootA;
    const double p0 = rootA;

    const double a0 = c + p0;
    return { rootA * (c + z0) / a0, rootA * (z0 - c) / a0, 0.0, (p0 - c) / a0, 0.0 };
}

BiquadCoeffs Biquad::makeFirstOrderHPF (double freq, double sampleRate) noexcept
{
    const double K = std::tan (kPi * clampFrequency (freq, sampleRate) / sampleRate);
    const double inv = 1.0 / (K + 1.0);
    return { inv, -inv, 0.0, (K - 1.0) * inv, 0.0 };
}

BiquadCoeffs Biquad::makeFirstOrderLPF (double freq, double sampleRate) noexcept
{
    const double K = std::tan (kPi * clampFrequency (freq, sampleRate) / sampleRate);
    const double inv = 1.0 / (K + 1.0);
    return { K * inv, K * inv, 0.0, (K - 1.0) * inv, 0.0 };
}

BiquadCoeffs Biquad::bilinear (double B0, double B1, double B2,
                               double A0, double A1, double A2,
                               double freq, double sampleRate) noexcept
{
    // s = c * (1 - z^-1) / (1 + z^-1), c = 1 / tan(pi f / fs)  (maps 1 rad/s -> freq)
    const double c = 1.0 / std::tan (kPi * clampFrequency (freq, sampleRate) / sampleRate);
    const double c2 = c * c;

    return normalise (B0 * c2 + B1 * c + B2,
                      2.0 * (B2 - B0 * c2),
                      B0 * c2 - B1 * c + B2,
                      A0 * c2 + A1 * c + A2,
                      2.0 * (A2 - A0 * c2),
                      A0 * c2 - A1 * c + A2);
}

int Biquad::makeButterworthCascade (bool highPass, int order, double freq, double resonanceQ,
                                    double sampleRate, BiquadCoeffs* out) noexcept
{
    order = std::clamp (order, 1, 2 * kMaxCascadeSections);
    const int pairs = order / 2;
    const double qScale = std::max (resonanceQ, 0.1) / 0.70710678118654752;
    int n = 0;

    for (int k = 0; k < pairs; ++k)
    {
        // Pole angle from the negative real axis: even orders (2k+1)pi/2N, odd orders (k+1)pi/N.
        const double theta = (order & 1) == 0 ? kPi * (2.0 * k + 1.0) / (2.0 * order)
                                              : kPi * (k + 1.0) / order;
        double q = 1.0 / (2.0 * std::cos (theta));

        if (k == pairs - 1)
            q *= qScale;   // resonance applies to the highest-Q pole pair

        out[n++] = highPass ? makeHPF (freq, q, sampleRate) : makeLPF (freq, q, sampleRate);
    }

    if ((order & 1) != 0)
        out[n++] = highPass ? makeFirstOrderHPF (freq, sampleRate) : makeFirstOrderLPF (freq, sampleRate);

    return n;
}

int Biquad::makeEllipticHPF (double freq, double sampleRate, BiquadCoeffs* out) noexcept
{
    // Low-pass section (DC-normalised):  (wp^2/wz^2) (s^2 + wz^2) / (s^2 + (wp/Q) s + wp^2)
    // LP -> HP (s -> 1/s):                (s^2 + 1/wz^2) / (s^2 + s/(Q wp) + 1/wp^2)
    // HF gain of each HP section is 1; the first section is scaled by the ripple
    // so the passband maximum sits at 0 dB.
    const double rippleGain = std::pow (10.0, -kEllipticRippleDb / 20.0);

    for (int i = 0; i < 2; ++i)
    {
        const auto& s = kElliptic4[i];
        const double wp = std::sqrt (s.poleRe * s.poleRe + s.poleIm * s.poleIm);
        const double q = wp / (-2.0 * s.poleRe);
        const double g = (i == 0) ? rippleGain : 1.0;

        out[i] = bilinear (g, 0.0, g / (s.zeroW * s.zeroW),
                           1.0, 1.0 / (q * wp), 1.0 / (wp * wp),
                           freq, sampleRate);
    }

    return 2;
}

} // namespace meridian
