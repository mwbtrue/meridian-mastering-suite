#pragma once

#include <array>
#include <cmath>
#include <complex>

namespace meridian
{

/**
    Normalised biquad coefficients (a0 == 1):

        H(z) = (b0 + b1 z^-1 + b2 z^-2) / (1 + a1 z^-1 + a2 z^-2)

    First-order sections are represented with b2 = a2 = 0.
*/
struct BiquadCoeffs
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;

    static constexpr BiquadCoeffs identity() noexcept { return {}; }

    /** Complex frequency response at freq (Hz) for sample rate fs. */
    std::complex<double> response (double freq, double fs) const noexcept
    {
        const double w = 2.0 * 3.14159265358979323846 * freq / fs;
        const std::complex<double> z1 = std::polar (1.0, -w);   // z^-1
        const std::complex<double> z2 = z1 * z1;                 // z^-2
        return (b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2);
    }

    /** Linear magnitude at freq (Hz). */
    double magnitude (double freq, double fs) const noexcept { return std::abs (response (freq, fs)); }

    /** Linear interpolation between two coefficient sets (t in [0, 1]).
        The (a1, a2) stability triangle is convex, so interpolating between two
        stable sets always yields a stable filter. */
    static BiquadCoeffs lerp (const BiquadCoeffs& a, const BiquadCoeffs& b, double t) noexcept
    {
        return { a.b0 + (b.b0 - a.b0) * t, a.b1 + (b.b1 - a.b1) * t, a.b2 + (b.b2 - a.b2) * t,
                 a.a1 + (b.a1 - a.a1) * t, a.a2 + (b.a2 - a.a2) * t };
    }

    bool operator== (const BiquadCoeffs& o) const noexcept
    {
        return b0 == o.b0 && b1 == o.b1 && b2 == o.b2 && a1 == o.a1 && a2 == o.a2;
    }

    bool operator!= (const BiquadCoeffs& o) const noexcept { return ! (*this == o); }
};

/**
    Biquad filter, Direct Form II Transposed, double precision.

    Coefficient design follows the RBJ Audio EQ Cookbook (bilinear transform,
    exact omega = 2*pi*f/Fs, frequency pre-warped at the design frequency).
    All design functions are static, allocation-free and real-time safe.
*/
class Biquad
{
public:
    Biquad() noexcept = default;
    explicit Biquad (const BiquadCoeffs& c) noexcept : coeffs (c) {}

    void setCoefficients (const BiquadCoeffs& c) noexcept { coeffs = c; }

    void setCoefficients (double b0, double b1, double b2, double a1, double a2) noexcept
    {
        coeffs = { b0, b1, b2, a1, a2 };
    }

    const BiquadCoeffs& getCoefficients() const noexcept { return coeffs; }

    /** Zeroes the filter state. */
    void reset() noexcept { z1 = z2 = 0.0; }

    inline double process (double x) noexcept
    {
        const double y = coeffs.b0 * x + z1;
        z1 = coeffs.b1 * x - coeffs.a1 * y + z2;
        z2 = coeffs.b2 * x - coeffs.a2 * y;
        return y;
    }

    /** In-place block processing. */
    void processBlock (double* data, int numSamples) noexcept
    {
        const double b0 = coeffs.b0, b1 = coeffs.b1, b2 = coeffs.b2, a1 = coeffs.a1, a2 = coeffs.a2;
        double s1 = z1, s2 = z2;

        for (int i = 0; i < numSamples; ++i)
        {
            const double x = data[i];
            const double y = b0 * x + s1;
            s1 = b1 * x - a1 * y + s2;
            s2 = b2 * x - a2 * y;
            data[i] = y;
        }

        z1 = flushDenormal (s1);
        z2 = flushDenormal (s2);
    }

    /** Flushes accumulated denormals; call once per block for per-sample users. */
    void snapToZero() noexcept
    {
        z1 = flushDenormal (z1);
        z2 = flushDenormal (z2);
    }

    // ---- Coefficient design (RBJ cookbook) ----------------------------------
    static BiquadCoeffs makeBell      (double freq, double Q, double gainDB, double sampleRate) noexcept;
    static BiquadCoeffs makeHighShelf (double freq, double Q, double gainDB, double sampleRate) noexcept;
    static BiquadCoeffs makeLowShelf  (double freq, double Q, double gainDB, double sampleRate) noexcept;
    static BiquadCoeffs makeNotch     (double freq, double Q, double sampleRate) noexcept;
    static BiquadCoeffs makeHPF       (double freq, double Q, double sampleRate) noexcept;
    static BiquadCoeffs makeLPF       (double freq, double Q, double sampleRate) noexcept;
    /** Constant 0 dB peak-gain band-pass (unity gain and zero phase at freq). */
    static BiquadCoeffs makeBPF       (double freq, double Q, double sampleRate) noexcept;
    static BiquadCoeffs makeAllPass   (double freq, double Q, double sampleRate) noexcept;
    /** First-order tilt: -gainDB/2 at DC, +gainDB/2 at Nyquist, 0 dB at freq. */
    static BiquadCoeffs makeTilt      (double freq, double gainDB, double sampleRate) noexcept;

    /** First-order (6 dB/oct) sections used for odd Butterworth orders. */
    static BiquadCoeffs makeFirstOrderHPF (double freq, double sampleRate) noexcept;
    static BiquadCoeffs makeFirstOrderLPF (double freq, double sampleRate) noexcept;

    /** Bilinear transform of a cutoff-normalised analog biquad
        (B0 s^2 + B1 s + B2) / (A0 s^2 + A1 s + A2), pre-warped at freq. */
    static BiquadCoeffs bilinear (double B0, double B1, double B2,
                                  double A0, double A1, double A2,
                                  double freq, double sampleRate) noexcept;

    /** Butterworth cascade for a given order (1..16): fills out[] with
        ceil(order/2) sections and returns the section count. resonanceQ
        scales the highest-Q section (0.7071 = pure Butterworth). */
    static constexpr int kMaxCascadeSections = 8;
    static int makeButterworthCascade (bool highPass, int order, double freq, double resonanceQ,
                                       double sampleRate, BiquadCoeffs* out) noexcept;

    /** 4th-order elliptic high-pass (0.1 dB ripple, 80 dB stopband), passband
        edge at freq. Fills two sections and returns 2. */
    static int makeEllipticHPF (double freq, double sampleRate, BiquadCoeffs* out) noexcept;

    static double clampFrequency (double freq, double sampleRate) noexcept
    {
        const double nyquistLimit = 0.4999 * sampleRate;
        return freq < 1.0 ? 1.0 : (freq > nyquistLimit ? nyquistLimit : freq);
    }

private:
    static inline double flushDenormal (double v) noexcept { return std::abs (v) < 1.0e-30 ? 0.0 : v; }

    BiquadCoeffs coeffs;
    double z1 = 0.0, z2 = 0.0;
};

} // namespace meridian
