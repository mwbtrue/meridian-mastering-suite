#pragma once

#include <cstdint>
#include <cstring>

namespace meridian::fastmath
{

/*  Branch-light exp / log / tanh for the per-sample audio paths.

    The C runtime versions (notably MSVC's under /fp:precise) are several times
    slower than these and are never inlined. Accuracy here is close to full double
    precision (relative error < 1e-15 for exp and log over the audio range, absolute
    error < 1e-15 for tanh), i.e. ~-300 dB, so results are sonically identical.

    exp (0) == 1 exactly and tanh (0) == 0 exactly, so neutral settings stay neutral. */

inline double bitsToDouble (std::uint64_t b) noexcept { double d; std::memcpy (&d, &b, sizeof d); return d; }
inline std::uint64_t doubleToBits (double d) noexcept { std::uint64_t b; std::memcpy (&b, &d, sizeof b); return b; }

/** e^x. Inputs are clamped to [-700, 700] (results stay normal). */
inline double exp (double x) noexcept
{
    constexpr double kLog2e = 1.4426950408889634074;
    constexpr double kLn2Hi = 6.93147180369123816490e-01;   // Cody-Waite split of ln 2
    constexpr double kLn2Lo = 1.90821492927058770002e-10;

    x = x < -700.0 ? -700.0 : (x > 700.0 ? 700.0 : x);

    const double t = x * kLog2e;
    const double nd = t >= 0.0 ? static_cast<double> (static_cast<std::int64_t> (t + 0.5))
                               : -static_cast<double> (static_cast<std::int64_t> (0.5 - t));
    const double r = (x - nd * kLn2Hi) - nd * kLn2Lo;          // |r| <= 0.3466

    // Taylor series to r^13 / 13!  (truncation < 2e-17 relative)
    double p = 1.0 / 6227020800.0;
    p = p * r + 1.0 / 479001600.0;
    p = p * r + 1.0 / 39916800.0;
    p = p * r + 1.0 / 3628800.0;
    p = p * r + 1.0 / 362880.0;
    p = p * r + 1.0 / 40320.0;
    p = p * r + 1.0 / 5040.0;
    p = p * r + 1.0 / 720.0;
    p = p * r + 1.0 / 120.0;
    p = p * r + 1.0 / 24.0;
    p = p * r + 1.0 / 6.0;
    p = p * r + 0.5;
    p = p * r + 1.0;
    p = p * r + 1.0;

    const auto n = static_cast<std::int64_t> (nd);
    return p * bitsToDouble (static_cast<std::uint64_t> (n + 1023) << 52);
}

/** Natural log. x must be a positive normal number (callers add a 1e-30 floor). */
inline double log (double x) noexcept
{
    constexpr double kLn2 = 0.69314718055994530942;
    constexpr double kSqrt2 = 1.41421356237309504880;

    const std::uint64_t b = doubleToBits (x);
    auto e = static_cast<std::int64_t> ((b >> 52) & 0x7ff) - 1023;
    double m = bitsToDouble ((b & 0x000fffffffffffffULL) | 0x3ff0000000000000ULL);   // [1, 2)

    if (m > kSqrt2)
    {
        m *= 0.5;
        ++e;
    }

    // log(m) = 2 atanh(f), f = (m-1)/(m+1), |f| <= 0.1716
    const double f = (m - 1.0) / (m + 1.0);
    const double s = f * f;
    double p = 1.0 / 23.0;
    p = p * s + 1.0 / 21.0;
    p = p * s + 1.0 / 19.0;
    p = p * s + 1.0 / 17.0;
    p = p * s + 1.0 / 15.0;
    p = p * s + 1.0 / 13.0;
    p = p * s + 1.0 / 11.0;
    p = p * s + 1.0 / 9.0;
    p = p * s + 1.0 / 7.0;
    p = p * s + 1.0 / 5.0;
    p = p * s + 1.0 / 3.0;
    p = p * s + 1.0;

    return 2.0 * f * p + static_cast<double> (e) * kLn2;
}

/** 10 log10(x) for a power quantity. */
inline double powerToDb (double x) noexcept { return 4.3429448190325182765 * log (x); }

/** 20 log10(x) for an amplitude quantity. */
inline double ampToDb (double x) noexcept { return 8.6858896380650365530 * log (x); }

/** 10^(db/20). */
inline double dbToAmp (double db) noexcept { return exp (0.11512925464970228420 * db); }

/** Hyperbolic tangent. */
inline double tanh (double x) noexcept
{
    const double a = x < 0.0 ? -x : x;

    if (a < 0.125)
    {
        // Odd Taylor series to x^17 (truncation < 2e-21 at |x| = 0.125), no cancellation.
        const double s = x * x;
        double p = 6404582.0 / 10854718875.0;
        p = p * s - 929569.0 / 638512875.0;
        p = p * s + 21844.0 / 6081075.0;
        p = p * s - 1382.0 / 155925.0;
        p = p * s + 62.0 / 2835.0;
        p = p * s - 17.0 / 315.0;
        p = p * s + 2.0 / 15.0;
        p = p * s - 1.0 / 3.0;
        return x + x * s * p;
    }

    if (a > 19.0)
        return x < 0.0 ? -1.0 : 1.0;

    const double e = exp (-2.0 * a);          // (0, 0.78]
    const double t = (1.0 - e) / (1.0 + e);
    return x < 0.0 ? -t : t;
}

} // namespace meridian::fastmath
