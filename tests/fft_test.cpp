// RealFFT accuracy: forward transform vs. a direct double-precision DFT, and
// forward + inverse round trip, for sizes 16 ... 4096.
// Pass: spectrum error < 1e-5 relative to the spectrum peak, round-trip
// error < 1e-5 relative to the input peak (single-precision transform).

#include "common/RealFFT.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

int main()
{
    int failures = 0;
    std::uint64_t s = 0x9E3779B97F4A7C15ull;
    auto rnd = [&s]
    {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return (double) (s >> 11) * (1.0 / 9007199254740992.0) * 2.0 - 1.0;
    };

    for (int N = 16; N <= 4096; N *= 2)
    {
        const int bins = N / 2 + 1;
        std::vector<float> x ((size_t) N), spec ((size_t) (2 * bins)), back ((size_t) N);
        for (auto& v : x) v = (float) rnd();

        meridian::RealFFT fft;
        fft.prepare (N);
        fft.forward (x.data(), spec.data());
        fft.inverse (spec.data(), back.data());

        double specErr = 0.0, specPeak = 0.0;
        for (int k = 0; k < bins; ++k)
        {
            double re = 0.0, im = 0.0;
            for (int n = 0; n < N; ++n)
            {
                const double a = -2.0 * 3.14159265358979323846 * (double) k * n / N;
                re += x[(size_t) n] * std::cos (a);
                im += x[(size_t) n] * std::sin (a);
            }
            specPeak = std::max (specPeak, std::hypot (re, im));
            specErr = std::max (specErr, std::hypot (spec[(size_t) k] - re, spec[(size_t) (bins + k)] - im));
        }

        double rtErr = 0.0, inPeak = 0.0;
        for (int n = 0; n < N; ++n)
        {
            inPeak = std::max (inPeak, (double) std::abs (x[(size_t) n]));
            rtErr = std::max (rtErr, (double) std::abs (back[(size_t) n] - x[(size_t) n]));
        }

        const double relSpec = specErr / specPeak, relRt = rtErr / inPeak;
        const bool ok = relSpec < 1.0e-5 && relRt < 1.0e-5;
        failures += ok ? 0 : 1;
        std::printf ("fft_test N=%5d  spectrum error %.2e  round-trip error %.2e  %s\n", N, relSpec, relRt, ok ? "PASS" : "FAIL");
    }

    std::printf ("fft_test: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
