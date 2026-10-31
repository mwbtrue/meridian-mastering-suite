// Limiter transparency: pink noise well below the ceiling must pass unchanged
// (after latency compensation) to better than -140 dBFS.
#include "TestHarness.h"
#include "dsp/TruePeakLimiter.h"

#include <chrono>

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    test::LimiterHost host;
    host.set (meridian::pid::limCeiling, 0.0f);

    const double fs = 44100.0;
    const int n = 44100;

    // Paul Kellett's refined pink-noise filter on white noise.
    std::vector<double> in ((size_t) n);
    juce::Random rng (1234);
    double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0, peak = 0;
    for (auto& v : in)
    {
        const double w = rng.nextDouble() * 2.0 - 1.0;
        b0 = 0.99886 * b0 + w * 0.0555179; b1 = 0.99332 * b1 + w * 0.0750759;
        b2 = 0.96900 * b2 + w * 0.1538520; b3 = 0.86650 * b3 + w * 0.3104856;
        b4 = 0.55000 * b4 + w * 0.5329522; b5 = -0.7616 * b5 - w * 0.0168980;
        v = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362;
        b6 = w * 0.115926;
        peak = std::max (peak, std::abs (v));
    }
    for (auto& v : in) v *= 0.5 / peak;   // -6 dBFS sample peak

    meridian::TruePeakLimiter lim (host.apvts);
    lim.prepare (fs, 512);
    lim.updateParameters();
    lim.reset();

    std::vector<double> L = in, R = in;
    const auto t0 = std::chrono::steady_clock::now();
    test::runModule (lim, L, R);
    const double secs = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();

    const int lat = lim.getLatencySamples();
    double maxDev = 0.0;
    for (int i = 0; i + lat < n; ++i)
        maxDev = std::max ({ maxDev, std::abs (L[(size_t) (i + lat)] - in[(size_t) i]), std::abs (R[(size_t) (i + lat)] - in[(size_t) i]) });

    const double devDb = test::toDb (maxDev);
    const bool pass = devDb < -140.0;
    std::printf ("null_test: latency %d samples, GR %.3g dB, max deviation %.1f dBFS -> %s\n",
                 lat, (double) lim.getGainReductionDb(), devDb, pass ? "PASS" : "FAIL");
    std::printf ("null_test: limiter cost %.2f %% of one core in real time (%d samples @ %.0f Hz in %.4f s)\n",
                 100.0 * secs / (n / fs), n, fs, secs);
    return pass ? 0 : 1;
}
