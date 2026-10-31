// Loudness-matched bypass and undo/redo on the complete processor.
//
//   1. Input module active with +6 dB trim, every other module bypassed:
//      output level is +6 dB over the input.
//   2. Input module bypassed with lm_bypass on: the bypassed output keeps the
//      level of the processed signal (+6 dB +/- 0.2 dB) and the processor
//      reports the applied compensation.
//   3. lm_bypass off: the bypassed output returns to the input level (0 dB).
//   4. Undo restores the previous parameter value, redo re-applies it, and two
//      edits separated by a transaction boundary are two undo steps.

#include "PluginHarness.h"

using namespace meridian;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 512;
int failures = 0;

void check (bool ok, const char* what, double value, const char* unit)
{
    std::printf ("  %-58s %9.3f %-4s %s\n", what, value, unit, ok ? "PASS" : "FAIL");
    if (! ok)
        ++failures;
}

/** Processes `seconds` of pink noise and returns output/input level in dB,
    measured over the last second (after the 50 ms gain ramp has settled). */
double runSection (MeridianAudioProcessor& p, double seconds, std::uint64_t seed)
{
    const int n = (int) (seconds * kSampleRate);
    auto L = test::pinkNoise (n, -20.0, seed);
    auto R = test::pinkNoise (n, -20.0, seed * 7 + 3);
    const auto inL = L, inR = R;

    test::runProcessor (p, L, R, kBlock);

    // Output sample i corresponds to input sample i - latency.
    const int lat = p.getLatencySamples();
    const int tail = (int) kSampleRate;
    double inSum = 0.0, outSum = 0.0;
    for (int i = n - tail; i < n; ++i)
    {
        const auto j = (size_t) (i - lat);
        inSum  += inL[j] * inL[j] + inR[j] * inR[j];
        outSum += L[(size_t) i] * L[(size_t) i] + R[(size_t) i] * R[(size_t) i];
    }
    return 10.0 * std::log10 (outSum / inSum);
}

float paramValue (MeridianAudioProcessor& p, std::string_view id)
{
    auto* param = p.getAPVTS().getParameter (pid::str (id));
    return param->convertFrom0to1 (param->getValue());
}

void testLoudnessMatch()
{
    std::printf ("Loudness-matched bypass\n");
    auto p = std::make_unique<MeridianAudioProcessor>();
    p->setPlayConfigDetails (2, 2, kSampleRate, kBlock);

    for (int m = 0; m < kNumModules; ++m)
        test::setParam (*p, pid::slotParam (pid::moduleBypassPrefix, m), m == (int) ModuleId::Input ? 0.0f : 1.0f);
    test::setParam (*p, pid::inputTrim, 6.0f);
    test::setParam (*p, pid::inputDcBlock, 0.0f);   // pure gain: the DC blocker would trim pink-noise LF energy
    test::setParam (*p, pid::lmBypass, 1.0f);
    p->prepareToPlay (kSampleRate, kBlock);

    const double active = runSection (*p, 8.0, 11);
    check (std::abs (active - 6.0) < 0.05, "Input active, +6 dB trim: output level", active, "dB");

    test::setParam (*p, pid::slotParam (pid::moduleBypassPrefix, (int) ModuleId::Input), 1.0f);
    const double matched = runSection (*p, 3.0, 23);
    check (std::abs (matched - 6.0) < 0.2, "Input bypassed, LM on: output level", matched, "dB");
    check (std::abs (matched - active) < 0.2, "LM on: bypassed minus active level", matched - active, "dB");

    const double reported = p->getLoudnessMatchDb (ModuleId::Input);
    check (std::abs (reported - 6.0) < 0.2, "Input bypassed, LM on: reported compensation", reported, "dB");

    test::setParam (*p, pid::lmBypass, 0.0f);
    const double plain = runSection (*p, 3.0, 37);
    check (std::abs (plain) < 0.01, "Input bypassed, LM off: output level", plain, "dB");

    p->releaseResources();
}

void testUndoRedo()
{
    std::printf ("Undo / redo\n");
    auto p = std::make_unique<MeridianAudioProcessor>();
    auto& um = p->getUndoManager();

    const float initial = paramValue (*p, pid::inputTrim);
    um.clearUndoHistory();

    // copyState() flushes pending parameter changes into the ValueTree, which
    // is where the APVTS records the undoable action.
    test::setParam (*p, pid::inputTrim, 3.0f);
    (void) p->getAPVTS().copyState();
    um.beginNewTransaction();
    test::setParam (*p, pid::inputTrim, -4.0f);
    (void) p->getAPVTS().copyState();

    check (p->canUndo(), "canUndo after two edits", p->canUndo() ? 1.0 : 0.0, "");

    p->undo();
    const float afterUndo1 = paramValue (*p, pid::inputTrim);
    check (std::abs (afterUndo1 - 3.0f) < 1.0e-3f, "first undo restores trim to +3 dB", afterUndo1, "dB");

    p->undo();
    const float afterUndo2 = paramValue (*p, pid::inputTrim);
    check (std::abs (afterUndo2 - initial) < 1.0e-3f, "second undo restores the initial trim", afterUndo2, "dB");

    check (p->canRedo(), "canRedo after undo", p->canRedo() ? 1.0 : 0.0, "");
    p->redo();
    p->redo();
    const float afterRedo = paramValue (*p, pid::inputTrim);
    check (std::abs (afterRedo + 4.0f) < 1.0e-3f, "two redos re-apply -4 dB", afterRedo, "dB");
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    testLoudnessMatch();
    testUndoRedo();

    std::printf ("RESULT: %s (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
