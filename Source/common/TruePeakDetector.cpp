#include "TruePeakDetector.h"
#include "DotProduct.h"

#include <algorithm>
#include <cmath>

namespace meridian
{

namespace
{
    constexpr double kPi = 3.14159265358979323846;

    double besselI0 (double x) noexcept
    {
        double sum = 1.0, term = 1.0;
        const double q = 0.25 * x * x;

        for (int k = 1; k < 64; ++k)
        {
            term *= q / (double (k) * double (k));
            sum += term;
            if (term < sum * 1.0e-17)
                break;
        }

        return sum;
    }
}

//==============================================================================
int TruePeakDetector::indexForFactor (int f) noexcept
{
    if (f >= 16) return 4;
    if (f >= 8)  return 3;
    if (f >= 4)  return 2;
    if (f >= 2)  return 1;
    return 0;
}

void TruePeakDetector::prepare (int maxFactorToUse, int halfSpanSamples, double kaiserBeta)
{
    maxFactor = 1 << indexForFactor (std::clamp (maxFactorToUse, 1, 16));
    halfSpan = std::max (1, halfSpanSamples);
    length = 2 * halfSpan;

    const double i0Beta = besselI0 (kaiserBeta);

    for (int ti = 0; ti <= indexForFactor (maxFactor); ++ti)
    {
        auto& t = tables[(size_t) ti];
        t.factor = 1 << ti;
        t.coeffs.assign ((size_t) (t.factor * length), 0.0);

        for (int p = 0; p < t.factor; ++p)
        {
            const double frac = double (p) / double (t.factor);
            double* c = t.coeffs.data() + (size_t) (p * length);
            double sum = 0.0;

            // Index i (oldest -> newest) holds x[centre + k], k = i - halfSpan + 1.
            for (int i = 0; i < length; ++i)
            {
                const double tau = frac - double (i - halfSpan + 1);
                const double r = tau / double (halfSpan);
                double h = 0.0;

                if (std::abs (r) < 1.0)
                {
                    const double sinc = tau == 0.0 ? 1.0 : std::sin (kPi * tau) / (kPi * tau);
                    h = sinc * besselI0 (kaiserBeta * std::sqrt (1.0 - r * r)) / i0Beta;
                }

                c[i] = h;
                sum += h;
            }

            for (int i = 0; i < length; ++i)
                c[i] /= sum;
        }

        const int pairs = t.factor / 2;
        t.even.assign ((size_t) ((pairs + 1) * halfSpan), 0.0);
        t.odd.assign ((size_t) ((pairs + 1) * halfSpan), 0.0);

        for (int p = 1; p <= pairs; ++p)
        {
            const double* c = t.coeffs.data() + (size_t) (p * length);
            for (int i = 0; i < halfSpan; ++i)
            {
                t.even[(size_t) (p * halfSpan + i)] = c[i] + c[length - 1 - i];
                t.odd[(size_t) (p * halfSpan + i)]  = c[i] - c[length - 1 - i];
            }
        }
    }

    foldSum.assign ((size_t) halfSpan, 0.0);
    foldDiff.assign ((size_t) halfSpan, 0.0);

    for (auto& ch : channels)
        ch.history.assign ((size_t) (2 * length), 0.0);

    setFactor (factor);
    reset();
}

void TruePeakDetector::setFactor (int newFactor) noexcept
{
    factor = 1 << indexForFactor (std::clamp (newFactor, 1, maxFactor));
    active = &tables[(size_t) indexForFactor (factor)];
}

void TruePeakDetector::reset() noexcept
{
    for (auto& ch : channels)
    {
        std::fill (ch.history.begin(), ch.history.end(), 0.0);
        ch.writePos = 0;
    }
}

double TruePeakDetector::process (int chIndex, double x) noexcept
{
    auto& ch = channels[(size_t) chIndex];

    if (ch.history.empty() || active == nullptr)
        return std::abs (x);

    ch.history[(size_t) ch.writePos] = x;
    ch.history[(size_t) (ch.writePos + length)] = x;
    ch.writePos = ch.writePos + 1 == length ? 0 : ch.writePos + 1;

    const double* window = ch.history.data() + ch.writePos;   // oldest -> newest
    double peak = std::abs (window[halfSpan - 1]);             // phase 0 == centre sample

    const int f = active->factor;
    if (f < 2)
        return peak;

    double* a = foldSum.data();
    double* d = foldDiff.data();
    for (int i = 0; i < halfSpan; ++i)
    {
        a[i] = window[i] + window[length - 1 - i];
        d[i] = window[i] - window[length - 1 - i];
    }

    const int pairs = f / 2;
    for (int p = 1; p <= pairs; ++p)
    {
        const double s = dotProduct (active->even.data() + (size_t) (p * halfSpan), a, halfSpan);

        if (2 * p == f)   // centre phase is self-symmetric: s == 2 * y
        {
            peak = std::max (peak, std::abs (0.5 * s));
            continue;
        }

        const double df = dotProduct (active->odd.data() + (size_t) (p * halfSpan), d, halfSpan);
        peak = std::max (peak, std::max (std::abs (0.5 * (s + df)), std::abs (0.5 * (s - df))));
    }

    return peak;
}

//==============================================================================
void GuaranteedGainSmoother::prepare (int maxHold, int maxBox)
{
    dqCapacity = std::max (2, maxHold + 1);
    dqValue.assign ((size_t) dqCapacity, 1.0);
    dqIndex.assign ((size_t) dqCapacity, 0);
    box.assign ((size_t) std::max (1, maxBox), 1.0);
    configure (1, 0, 1.0);
}

void GuaranteedGainSmoother::configure (int rampLength, int plateauLength, double initialGain) noexcept
{
    ramp = std::clamp (rampLength, 1, (int) box.size());
    plateau = std::max (0, plateauLength & ~1);
    hold = std::min (ramp + plateau, dqCapacity - 1);
    reset (initialGain);
}

void GuaranteedGainSmoother::reset (double initialGain) noexcept
{
    dqHead = 0;
    dqSize = 0;
    sampleIndex = 0;
    envelope = initialGain;
    std::fill (box.begin(), box.begin() + ramp, initialGain);
    boxPos = 0;
    boxSum = initialGain * ramp;
}

double GuaranteedGainSmoother::process (double required, double releaseCoeff) noexcept
{
    const long long n = sampleIndex++;

    // --- Sliding minimum over the last `hold` requirements (monotonic deque) ---
    while (dqSize > 0)
    {
        const int back = (dqHead + dqSize - 1) % dqCapacity;
        if (dqValue[(size_t) back] < required)
            break;
        --dqSize;
    }

    const int slot = (dqHead + dqSize) % dqCapacity;
    dqValue[(size_t) slot] = required;
    dqIndex[(size_t) slot] = n;
    ++dqSize;

    while (dqIndex[(size_t) dqHead] <= n - hold)
    {
        dqHead = (dqHead + 1) % dqCapacity;
        --dqSize;
    }

    const double m = dqValue[(size_t) dqHead];

    // --- Release (never above m) ---
    envelope = m < envelope ? m : envelope + releaseCoeff * (m - envelope);
    envelope = std::min (envelope, m);

    // --- Box filter (exact re-summation once per cycle bounds rounding drift) ---
    boxSum += envelope - box[(size_t) boxPos];
    box[(size_t) boxPos] = envelope;

    if (++boxPos == ramp)
    {
        boxPos = 0;
        double s = 0.0;
        for (int i = 0; i < ramp; ++i)
            s += box[(size_t) i];
        boxSum = s;
    }

    // Rounding can only push the mean up by ~ramp * eps; scale it away.
    return std::max (0.0, boxSum / double (ramp)) * (1.0 - 1.0e-13);
}

} // namespace meridian
