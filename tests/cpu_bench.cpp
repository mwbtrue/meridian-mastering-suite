// CPU benchmark: complete processor, factory-default parameters, 48 kHz,
// 512-sample float blocks (the host format FL Studio uses), 60 s of stereo
// pink noise at -14 dBFS RMS. Reports wall-clock processing time as a
// percentage of real time on one core. Target: < 15 %.
//
// Pass --fail-over=<percent> to make the exit code reflect the target (CTest
// uses a generous limit because shared CI machines are noisy). --per-module
// adds a per-module cost breakdown. --seconds=<n> shortens the main run.

#include "PluginHarness.h"

#include <chrono>
#include <cstring>

namespace
{
/** Processes `seconds` of the stimulus in float blocks; returns % of real time. */
double measure (MeridianAudioProcessor& p, const std::vector<double>& L, const std::vector<double>& R,
                double sr, int block, double seconds, double* worstPct)
{
    juce::AudioBuffer<float> buf (2, block);
    juce::MidiBuffer midi;
    const int n = std::min ((int) L.size(), (int) (seconds * sr));
    double busy = 0.0, worst = 0.0;
    int blocks = 0;

    for (int o = 0; o + block <= n; o += block)
    {
        for (int i = 0; i < block; ++i)
        {
            buf.setSample (0, i, (float) L[(size_t) (o + i)]);
            buf.setSample (1, i, (float) R[(size_t) (o + i)]);
        }

        const auto t0 = std::chrono::steady_clock::now();
        p.processBlock (buf, midi);
        const double dt = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        busy += dt;
        worst = std::max (worst, dt);
        ++blocks;
    }

    if (worstPct != nullptr)
        *worstPct = 100.0 * worst / (block / sr);
    return 100.0 * busy / ((double) blocks * block / sr);
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    double failOver = 1.0e9;
    bool perModule = false;
    double seconds = 60.0;
    for (int a = 1; a < argc; ++a)
    {
        if (std::strncmp (argv[a], "--fail-over=", 12) == 0)
            failOver = std::atof (argv[a] + 12);
        else if (std::strncmp (argv[a], "--seconds=", 10) == 0)
            seconds = std::max (1.0, std::atof (argv[a] + 10));
        else if (std::strcmp (argv[a], "--per-module") == 0)
            perModule = true;
    }

    constexpr double sr = 48000.0;
    constexpr int block = 512;
    const int n = (int) (seconds * sr);

    MeridianAudioProcessor p;
    p.setPlayConfigDetails (2, 2, sr, block);
    p.prepareToPlay (sr, block);

    const auto srcL = test::pinkNoise (n, -14.0, 1234567ull);
    const auto srcR = test::pinkNoise (n, -14.0, 7654321ull);

    measure (p, srcL, srcR, sr, block, 2.0, nullptr);   // warm-up: FIR designs, caches

    double worstPct = 0.0;
    const double pct = measure (p, srcL, srcR, sr, block, seconds, &worstPct);

    std::printf ("Meridian CPU benchmark: default chain, 48 kHz / %d, %.0f s\n", block, seconds);
    std::printf ("  average load  %6.2f %% of one core (target < 15 %%)\n", pct);
    std::printf ("  worst block   %6.2f %% of its real-time budget\n", worstPct);
    std::printf ("  latency       %d samples (%.2f ms)\n", p.getLatencySamples(), 1000.0 * p.getLatencySamples() / sr);
    std::printf ("  per-module latency:");
    for (int m = 0; m < meridian::kNumModules; ++m)
        if (const auto* mod = p.getModule (static_cast<meridian::ModuleId> (m)); mod != nullptr && mod->getLatencySamples() != 0)
            std::printf (" %s %d,", std::string (meridian::kModuleNames[(size_t) m]).c_str(), mod->getLatencySamples());
    std::printf ("\n");
    std::printf ("RESULT: %s\n", pct < 15.0 ? "PASS" : "OVER TARGET");

    if (perModule)
    {
        std::printf ("Per-module cost (full chain minus chain with that module bypassed, best of 5 x 4 s):\n");
        // Minimum of five 4 s runs: robust against scheduler noise on shared machines.
        auto best = [&]
        {
            double m = 1.0e9;
            for (int r = 0; r < 5; ++r)
                m = std::min (m, measure (p, srcL, srcR, sr, block, 4.0, nullptr));
            return m;
        };
        const double ref = best();

        for (int m = 0; m < meridian::kNumModules; ++m)
        {
            const auto bypassId = meridian::pid::slotParam (meridian::pid::moduleBypassPrefix, m);
            test::setParam (p, bypassId, 1.0f);
            measure (p, srcL, srcR, sr, block, 0.5, nullptr);   // let the bypass ramp settle
            const double without = best();
            test::setParam (p, bypassId, 0.0f);
            measure (p, srcL, srcR, sr, block, 0.5, nullptr);
            std::printf ("  %-14s %6.2f %%\n", std::string (meridian::kModuleNames[(size_t) m]).c_str(), ref - without);
        }

        // Everything outside the modules: metering, spectrum, capture ring,
        // dry-path delay compensation, loudness-match tracking.
        for (int m = 0; m < meridian::kNumModules; ++m)
            test::setParam (p, meridian::pid::slotParam (meridian::pid::moduleBypassPrefix, m), 1.0f);
        measure (p, srcL, srcR, sr, block, 0.5, nullptr);
        std::printf ("  %-14s %6.2f %%   (all modules bypassed: framework overhead)\n", "Framework", best());
        std::printf ("  %-14s %6.2f %%   (best-of-5 full chain used as reference)\n", "Full chain", ref);
    }

    p.releaseResources();
    return pct < failOver ? 0 : 1;
}
