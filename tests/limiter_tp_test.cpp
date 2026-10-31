// True-peak ceiling guarantee: output true peak (independent 16x estimator)
// must never exceed the -1 dBTP ceiling, for every signal and oversampling setting.
#include "TestHarness.h"
#include "dsp/TruePeakLimiter.h"

#include <functional>
#include <string>

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    const double fs = 48000.0;
    const int n = 96000;        // signal length
    const int pad = 8192;       // trailing silence: flushes the limiter so the signal's own
                                // end (a real full-scale -> 0 step) is part of the output and
                                // the estimator never sees an artificial truncation edge
    const double pi = 3.14159265358979323846;
    const double ceilLin = std::pow (10.0, -1.0 / 20.0);

    struct Signal { std::string name; std::function<double (int, juce::Random&)> gen; };
    const double p12 = std::pow (10.0, 12.0 / 20.0);
    const double m3 = std::pow (10.0, -3.0 / 20.0);

    std::vector<Signal> signals
    {
        { "997 Hz sine 0 dBFS",         [&] (int i, juce::Random&) { return std::sin (2 * pi * 997.0 * i / fs); } },
        { "square 0 dBFS (100 Hz)",     [&] (int i, juce::Random&) { return (i / 240) % 2 ? -1.0 : 1.0; } },
        { "997+1003 Hz -3 dBFS each",   [&] (int i, juce::Random&) { return m3 * (std::sin (2 * pi * 997.0 * i / fs) + std::sin (2 * pi * 1003.0 * i / fs)); } },
        { "square +12 dBFS (100 Hz)",   [&] (int i, juce::Random&) { return p12 * ((i / 240) % 2 ? -1.0 : 1.0); } },
        { "white noise 0 dBFS",         [&] (int, juce::Random& r) { return r.nextDouble() * 2.0 - 1.0; } },
        { "fs/4 @45deg +12 dB (ISP)",   [&] (int i, juce::Random&) { return p12 * std::sin (pi * 0.5 * i + pi * 0.25); } },
        { "log sweep 20-20k +12 dB",    [&] (int i, juce::Random&) {
              const double T = n / fs, k = std::log (1000.0);
              return p12 * std::sin (2 * pi * 20.0 * T / k * (std::exp (k * (i / fs) / T) - 1.0)); } },
    };

    bool allPass = true;
    std::printf ("limiter_tp_test: ceiling -1.000 dBTP (%.6f), independent 16x estimator\n", ceilLin);

    for (const auto& s : signals)
    {
        for (int osIndex = 0; osIndex < 3; ++osIndex)
        {
            test::LimiterHost host;
            host.set (meridian::pid::limCeiling, -1.0f);
            host.set (meridian::pid::limOversampling, (float) osIndex);

            meridian::TruePeakLimiter lim (host.apvts);
            lim.prepare (fs, 512);
            lim.updateParameters();
            lim.reset();

            juce::Random rng (42);
            std::vector<double> L ((size_t) (n + pad), 0.0), R ((size_t) (n + pad), 0.0);
            for (int i = 0; i < n; ++i)
            {
                L[(size_t) i] = s.gen (i, rng);
                R[(size_t) i] = s.name.rfind ("white", 0) == 0 ? rng.nextDouble() * 2.0 - 1.0 : L[(size_t) i];
            }

            test::runModule (lim, L, R);

            const double tp = std::max (test::truePeak16x (L), test::truePeak16x (R));
            const bool pass = tp <= ceilLin;
            allPass = allPass && pass;
            std::printf ("  %-28s %2dx  TP %+8.4f dBTP  %s\n", s.name.c_str(), 4 << osIndex, test::toDb (tp), pass ? "PASS" : "FAIL");
        }
    }

    std::printf ("limiter_tp_test: %s\n", allPass ? "PASS" : "FAIL");
    return allPass ? 0 : 1;
}
