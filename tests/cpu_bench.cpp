// CPU benchmark: complete processor, factory-default parameters, 48 kHz,
// 512-sample float blocks (the host format FL Studio uses), 60 s of stereo
// pink noise at -14 dBFS RMS. Reports wall-clock processing time as a
// percentage of real time on one core. Target: < 15 %.
//
// Pass --fail-over=<percent> to make the exit code reflect the target (CTest
// uses a generous limit because shared CI machines are noisy).

#include "PluginHarness.h"

#include <chrono>
#include <cstring>

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    double failOver = 1.0e9;
    for (int a = 1; a < argc; ++a)
        if (std::strncmp (argv[a], "--fail-over=", 12) == 0)
            failOver = std::atof (argv[a] + 12);

    constexpr double sr = 48000.0;
    constexpr int block = 512;
    constexpr double seconds = 60.0;
    const int n = (int) (seconds * sr);

    MeridianAudioProcessor p;
    p.setPlayConfigDetails (2, 2, sr, block);
    p.prepareToPlay (sr, block);

    const auto srcL = test::pinkNoise (n, -14.0, 1234567ull);
    const auto srcR = test::pinkNoise (n, -14.0, 7654321ull);

    juce::AudioBuffer<float> buf (2, block);
    juce::MidiBuffer midi;

    // Warm-up (2 s) so background FIR designs and caches settle.
    for (int o = 0; o + block <= (int) (2.0 * sr); o += block)
    {
        for (int i = 0; i < block; ++i)
        {
            buf.setSample (0, i, (float) srcL[(size_t) (o + i)]);
            buf.setSample (1, i, (float) srcR[(size_t) (o + i)]);
        }
        p.processBlock (buf, midi);
    }

    double busy = 0.0, worstBlock = 0.0;
    int blocks = 0;

    for (int o = 0; o + block <= n; o += block)
    {
        for (int i = 0; i < block; ++i)
        {
            buf.setSample (0, i, (float) srcL[(size_t) (o + i)]);
            buf.setSample (1, i, (float) srcR[(size_t) (o + i)]);
        }

        const auto t0 = std::chrono::steady_clock::now();
        p.processBlock (buf, midi);
        const double dt = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();

        busy += dt;
        worstBlock = std::max (worstBlock, dt);
        ++blocks;
    }

    const double audio = (double) blocks * block / sr;
    const double pct = 100.0 * busy / audio;
    const double worstPct = 100.0 * worstBlock / (block / sr);

    std::printf ("Meridian CPU benchmark: default chain, 48 kHz / %d, %.0f s\n", block, audio);
    std::printf ("  average load  %6.2f %% of one core (target < 15 %%)\n", pct);
    std::printf ("  worst block   %6.2f %% of its real-time budget\n", worstPct);
    std::printf ("  latency       %d samples (%.2f ms)\n", p.getLatencySamples(), 1000.0 * p.getLatencySamples() / sr);
    std::printf ("RESULT: %s\n", pct < 15.0 ? "PASS" : "OVER TARGET");

    p.releaseResources();
    return pct < failOver ? 0 : 1;
}
