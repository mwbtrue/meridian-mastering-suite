// Linear-phase 4-band crossover (120 / 1000 / 5000 Hz): band sum vs delayed input.
#include <juce_core/juce_core.h>

#include "common/LinearPhaseCrossover.h"

#include <cmath>
#include <cstdio>
#include <vector>

int main()
{
    const double fs = 48000.0;
    const int n = 48000, block = 512, extra = 4096;

    juce::Random rng (7);
    std::vector<double> in ((size_t) (n + extra), 0.0);
    for (int i = 0; i < n; ++i)
        in[(size_t) i] = rng.nextDouble() * 2.0 - 1.0;

    meridian::LinearPhaseCrossover xo;
    xo.prepare (fs, block);
    xo.setCrossoverFrequencies (std::vector<double> { 120.0, 1000.0, 5000.0 }, fs);
    const int lat = xo.getLatencySamples();

    std::vector<std::vector<double>> bl (4, std::vector<double> ((size_t) block)), br = bl;
    std::vector<double> out ((size_t) (n + extra)), outR ((size_t) (n + extra));
    double bandEnergy[4] {};
    double* pl[4]; double* pr[4];
    for (int b = 0; b < 4; ++b) { pl[b] = bl[(size_t) b].data(); pr[b] = br[(size_t) b].data(); }

    for (int o = 0; o < n + extra; o += block)
    {
        const int len = std::min (block, n + extra - o);
        xo.processBlock (in.data() + o, in.data() + o, pl, pr, len, 4);
        meridian::LinearPhaseCrossover::sumBands (pl, pr, out.data() + o, outR.data() + o, len, 4);
        if (o >= 2048 + lat && o + len <= n + lat)
            for (int b = 0; b < 4; ++b)
                for (int i = 0; i < len; ++i)
                    bandEnergy[b] += pl[b][i] * pl[b][i];
    }

    // RMS of the error relative to RMS of the signal over the steady-state region.
    double errSq = 0.0, sigSq = 0.0, maxErr = 0.0;
    for (int i = 2048; i < n; ++i)
    {
        const double e = out[(size_t) (i + lat)] - in[(size_t) i];
        errSq += e * e;
        sigSq += in[(size_t) i] * in[(size_t) i];
        maxErr = std::max (maxErr, std::abs (e));
    }

    double sumSq = 0.0;
    for (int i = 2048; i < n; ++i)
        sumSq += out[(size_t) (i + lat)] * out[(size_t) (i + lat)];

    const double levelDevDb = 10.0 * std::log10 (sumSq / sigSq);
    const double errDb = 10.0 * std::log10 (errSq / sigSq + 1.0e-300);
    const double worstDb = 20.0 * std::log10 (1.0 + std::sqrt (errSq / sigSq));
    // The split must be real: with white noise each band's share of the energy
    // tracks its bandwidth (0-120 Hz ~0.5 %, 120-1k ~3.7 %, 1k-5k ~16.7 %, 5k-24k ~79 %).
    double totalBand = 0.0;
    for (double e : bandEnergy) totalBand += e;
    bool splitOk = totalBand > 0.0;
    const double expectedShare[4] { 120.0 / 24000.0, 880.0 / 24000.0, 4000.0 / 24000.0, 19000.0 / 24000.0 };
    std::printf ("crossover_sum_test: band energy share (measured / ideal):");
    for (int b = 0; b < 4; ++b)
    {
        const double share = bandEnergy[b] / totalBand;
        splitOk = splitOk && std::abs (10.0 * std::log10 (share / expectedShare[b])) < 1.0;
        std::printf ("  b%d %.4f / %.4f", b, share, expectedShare[b]);
    }
    std::printf ("  -> split %s\n", splitOk ? "OK" : "WRONG");

    const bool pass = std::abs (levelDevDb) < 0.05 && worstDb < 0.05 && splitOk;   // spec: flat within +/-0.05 dB
    std::printf ("crossover_sum_test: latency %d, RMS level deviation %.6f dB, error/signal %.1f dB "
                 "(=> max gain deviation bound %.6f dB), peak error %.3g -> %s\n",
                 lat, levelDevDb, errDb, worstDb, maxErr, pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
