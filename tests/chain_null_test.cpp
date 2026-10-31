// Full-chain null test on the complete MeridianAudioProcessor.
//
//   (a) every module bypassed
//   (b) every module active at neutral settings
//   (c) diagnostics: each module alone at neutral (others bypassed)
//
// Stimulus: decorrelated stereo pink noise, -20 dBFS RMS, 6 s at 48 kHz,
// processed in double precision with 512-sample blocks. The output is
// compared with the input delayed by the latency the processor reports to
// the host; the worst absolute sample difference must be below -140 dBFS.

#include "PluginHarness.h"

using namespace meridian;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 512;
constexpr double kLimitDb = -140.0;

void applyNeutral (MeridianAudioProcessor& p)
{
    namespace f = pid::field;

    test::setParam (p, pid::inputTrim, 0.0f);
    test::setParam (p, pid::inputPolarity, 0.0f);
    test::setParam (p, pid::inputDcBlock, 0.0f);

    for (int i = 0; i < kNumCeqBands; ++i)
    {
        test::setParam (p, pid::bandParam (pid::ceqBandPrefix, i, f::type), 0.0f);   // Bell
        test::setParam (p, pid::bandParam (pid::ceqBandPrefix, i, f::gain), 0.0f);
    }

    test::setParam (p, pid::lecMonoEnabled, 0.0f);
    test::setParam (p, pid::lecDynEnabled, 0.0f);

    for (int i = 0; i < kNumDeqBands; ++i)
        test::setParam (p, pid::bandParam (pid::deqBandPrefix, i, f::enabled), 0.0f);
    test::setParam (p, pid::deqResonEnabled, 0.0f);

    test::setParam (p, pid::glueThreshold, 0.0f);
    test::setParam (p, pid::glueMakeup, 0.0f);

    for (int i = 0; i < kNumMbdBands; ++i)
        test::setParam (p, pid::bandParam (pid::mbdBandPrefix, i, f::enabled), 0.0f);

    for (auto id : { pid::teqPultecBassBoost, pid::teqPultecBassAtten, pid::teqPultecMidBoost,
                     pid::teqPultecTrebleBoost, pid::teqBaxandallBass, pid::teqBaxandallTreble,
                     pid::teqPassiveMidGain, pid::teqSideAirGain, pid::teqColour })
        test::setParam (p, id, 0.0f);

    test::setParam (p, pid::satMix, 0.0f);

    for (int i = 0; i < kNumImgBands; ++i)
        test::setParam (p, pid::bandParam (pid::imgBandPrefix, i, f::width), 100.0f);
    test::setParam (p, pid::imgCorrGuard, 0.0f);

    test::setParam (p, pid::clipperEnabled, 0.0f);
    test::setParam (p, pid::limCeiling, 0.0f);
    test::setParam (p, pid::ditherBits, 2.0f);   // 32-bit float: dither off
    test::setParam (p, pid::outputGain, 0.0f);
}

/** Returns the worst |out - delayed in| in dBFS. */
double runCase (const char* name, bool neutral, int onlyModule)
{
    auto p = std::make_unique<MeridianAudioProcessor>();
    p->setPlayConfigDetails (2, 2, kSampleRate, kBlock);

    if (neutral)
        applyNeutral (*p);

    for (int m = 0; m < kNumModules; ++m)
    {
        const bool active = neutral && (onlyModule < 0 || onlyModule == m);
        test::setParam (*p, pid::slotParam (pid::moduleBypassPrefix, m), active ? 0.0f : 1.0f);
    }

    p->prepareToPlay (kSampleRate, kBlock);

    const int latency = p->getLatencySamples();
    const int n = (int) (6.0 * kSampleRate) + latency;
    const auto inL = test::pinkNoise (n, -20.0, 0x9E3779B97F4A7C15ull);
    const auto inR = test::pinkNoise (n, -20.0, 0xD1B54A32D192ED03ull);
    auto L = inL, R = inR;

    test::runProcessor (*p, L, R, kBlock);

    double worst = 0.0;
    for (int i = latency; i < n; ++i)
    {
        worst = std::max (worst, std::abs (L[(size_t) i] - inL[(size_t) (i - latency)]));
        worst = std::max (worst, std::abs (R[(size_t) i] - inR[(size_t) (i - latency)]));
    }

    const double db = test::toDb (worst);
    std::printf ("  %-28s latency %6d smp   residual %8.1f dBFS   %s\n",
                 name, latency, db, db < kLimitDb ? "PASS" : "FAIL");
    p->releaseResources();
    return db;
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::printf ("Meridian full-chain null test (limit %.0f dBFS)\n", kLimitDb);

    bool ok = true;
    ok &= runCase ("(a) all modules bypassed", false, -1) < kLimitDb;
    ok &= runCase ("(b) all modules neutral", true, -1) < kLimitDb;

    std::printf ("Per-module neutral diagnostics:\n");
    for (int m = 0; m < kNumModules; ++m)
    {
        const auto name = std::string ("(c) ") + std::string (kModuleNames[(size_t) m]);
        ok &= runCase (name.c_str(), true, m) < kLimitDb;
    }

    std::printf (ok ? "RESULT: PASS\n" : "RESULT: FAIL\n");
    return ok ? 0 : 1;
}
