// Verification of the multi-stage half-band OversamplingEngine:
//   1. reported latency is exact (band-limited sine round trip vs delayed input),
//   2. passband gain within +/-0.01 dB and phase error < 1e-4 rad up to 20 kHz at 48 kHz,
//   3. upsampler image rejection and decimator alias rejection >= 80 dB,
//   4. arbitrary block sizes give bit-identical output to fixed blocks.
#include "common/OversamplingEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <vector>

using meridian::OversamplingEngine;

namespace
{
constexpr double kPi = std::numbers::pi;

// Windowed single-bin projection: returns amplitude of frequency f (cycles per
// sample) in x[start, start + n). 4-term Blackman-Harris, sidelobes < -92 dB.
double toneAmplitude (const std::vector<double>& x, int start, int n, double f)
{
    double re = 0.0, im = 0.0, wsum = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double t = 2.0 * kPi * i / (n - 1);
        const double w = 0.35875 - 0.48829 * std::cos (t) + 0.14128 * std::cos (2 * t) - 0.01168 * std::cos (3 * t);
        re += w * x[(size_t) (start + i)] * std::cos (2.0 * kPi * f * (start + i));
        im += w * x[(size_t) (start + i)] * std::sin (2.0 * kPi * f * (start + i));
        wsum += w;
    }
    return 2.0 * std::sqrt (re * re + im * im) / wsum;
}

// Least-squares fit of y[start, start+n) to a*sin + b*cos at frequency f.
void fitSine (const std::vector<double>& y, int start, int n, double f, double& gain, double& phase)
{
    double ss = 0, cc = 0, sc = 0, ys = 0, yc = 0;
    for (int i = start; i < start + n; ++i)
    {
        const double s = std::sin (2.0 * kPi * f * i), c = std::cos (2.0 * kPi * f * i);
        ss += s * s; cc += c * c; sc += s * c; ys += y[(size_t) i] * s; yc += y[(size_t) i] * c;
    }
    const double det = ss * cc - sc * sc;
    const double a = (ys * cc - yc * sc) / det;
    const double b = (yc * ss - ys * sc) / det;
    gain = std::sqrt (a * a + b * b);
    phase = std::atan2 (b, a);
}

std::vector<double> roundTrip (OversamplingEngine& os, const std::vector<double>& in, int block)
{
    std::vector<double> out (in.size(), 0.0), hi ((size_t) block * 16, 0.0);
    size_t pos = 0;
    while (pos < in.size())
    {
        const int n = (int) std::min<size_t> ((size_t) block, in.size() - pos);
        const int m = os.upsample (in.data() + pos, hi.data(), n);
        os.downsample (hi.data(), out.data() + pos, m);
        pos += (size_t) n;
    }
    return out;
}

double db (double x) { return 20.0 * std::log10 (std::max (x, 1.0e-300)); }
}

int main()
{
    bool pass = true;
    const double fs = 48000.0;
    const int len = 1 << 15;

    for (int factor : { 2, 4, 8, 16 })
    {
        OversamplingEngine os (1, 16);
        os.setOversamplingFactor (factor);
        const int lat = os.getLatencySamples();
        const bool latOk = lat == OversamplingEngine::kRoundTripLatency
                           && lat == OversamplingEngine::latencyForFactor (factor)
                           && lat <= OversamplingEngine::kMaxLatencySamples;

        // 1 + 2: sine round trip, compared with the input delayed by lat.
        double worstGainDb = 0.0, worstPhase = 0.0;
        for (double hz : { 50.0, 1000.0, 5000.0, 10000.0, 15000.0, 20000.0 })
        {
            const double f = hz / fs;
            std::vector<double> x ((size_t) len);
            for (int i = 0; i < len; ++i)
                x[(size_t) i] = 0.5 * std::sin (2.0 * kPi * f * (i + lat));   // pre-advanced by lat
            os.reset();
            const auto y = roundTrip (os, x, 512);

            double g, ph;
            fitSine (y, 4096, len - 4096, f, g, ph);
            worstGainDb = std::max (worstGainDb, std::abs (db (g / 0.5)));
            worstPhase = std::max (worstPhase, std::abs (std::remainder (ph, 2.0 * kPi)));
        }

        // 3a: upsampler images. A 15 kHz tone has images at k*48k +/- 15k up to factor*24k.
        double worstImageDb = -400.0;
        {
            const double f0 = 15000.0 / fs;
            std::vector<double> x ((size_t) len), hi ((size_t) len * factor);
            for (int i = 0; i < len; ++i) x[(size_t) i] = std::sin (2.0 * kPi * f0 * i);
            os.reset();
            for (int p = 0; p < len; p += 512)
                os.upsample (x.data() + p, hi.data() + (size_t) p * factor, 512);

            const int n = 1 << 16, start = (int) hi.size() - n;
            const double ref = toneAmplitude (hi, start, n, f0 / factor);
            for (int k = 1; k < factor; ++k)
                for (double img : { k * fs - 15000.0, k * fs + 15000.0 })
                    if (img < factor * fs * 0.5)
                        worstImageDb = std::max (worstImageDb, db (toneAmplitude (hi, start, n, img / (fs * factor)) / ref));
        }

        // 3b: decimator aliases. High-rate tones in the stopband must not fold into 0..0.45 fs.
        double worstAliasDb = -400.0;
        for (double hz : { 27000.0, 33000.0, 45000.0, 70000.0, 100000.0, 170000.0, 330000.0 })
        {
            if (hz >= factor * fs * 0.5)
                continue;
            const double fh = hz / (fs * factor);
            std::vector<double> hi ((size_t) len * factor), y ((size_t) len);
            for (size_t i = 0; i < hi.size(); ++i) hi[i] = std::sin (2.0 * kPi * fh * (double) i);
            os.reset();
            int w = 0;
            for (size_t p = 0; p < hi.size(); p += 4096)
                w += os.downsample (hi.data() + p, y.data() + w, 4096);

            double alias = std::fmod (hz, fs);
            if (alias > fs * 0.5) alias = fs - alias;
            worstAliasDb = std::max (worstAliasDb, db (toneAmplitude (y, len - 16384, 16384, alias / fs)));
        }

        // 4: random block sizes must match fixed 512-sample blocks exactly.
        double blockDiff = 0.0;
        {
            std::vector<double> x ((size_t) len);
            unsigned s = 12345u;
            for (auto& v : x) { s = s * 1664525u + 1013904223u; v = (double) (s >> 8) / 8388608.0 - 1.0; }
            os.reset();
            const auto a = roundTrip (os, x, 512);
            os.reset();
            std::vector<double> b (x.size()), hi ((size_t) 1024 * 16);
            size_t pos = 0;
            while (pos < x.size())
            {
                s = s * 1664525u + 1013904223u;
                const int n = (int) std::min<size_t> (1 + (s >> 8) % 1024, x.size() - pos);
                const int m = os.upsample (x.data() + pos, hi.data(), n);
                os.downsample (hi.data(), b.data() + pos, m);
                pos += (size_t) n;
            }
            for (size_t i = 0; i < x.size(); ++i) blockDiff = std::max (blockDiff, std::abs (a[i] - b[i]));
        }

        const bool ok = latOk && worstGainDb < 0.01 && worstPhase < 1.0e-4
                        && worstImageDb < -80.0 && worstAliasDb < -80.0 && blockDiff == 0.0;
        std::printf ("%2dx: latency %d (%s), passband gain err %.5f dB, phase err %.2e rad, "
                     "images %.1f dB, aliases %.1f dB, block-size diff %.1e  %s\n",
                     factor, lat, latOk ? "ok" : "WRONG", worstGainDb, worstPhase,
                     worstImageDb, worstAliasDb, blockDiff, ok ? "ok" : "FAIL");
        pass = pass && ok;
    }

    std::printf ("RESULT: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
