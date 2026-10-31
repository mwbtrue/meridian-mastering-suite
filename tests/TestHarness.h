#pragma once

// Minimal processor hosting an APVTS with exactly the parameters the
// TruePeakLimiter reads, plus small signal / measurement helpers.

#include <juce_audio_processors/juce_audio_processors.h>

#include "ParameterIDs.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace test
{
using namespace meridian;

struct LimiterHost : juce::AudioProcessor
{
    juce::AudioProcessorValueTreeState apvts;

    static juce::AudioProcessorValueTreeState::ParameterLayout layout()
    {
        juce::AudioProcessorValueTreeState::ParameterLayout l;
        auto F = [&] (std::string_view id, float lo, float hi, float d)
        { l.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { pid::str (id), 1 }, pid::str (id), juce::NormalisableRange<float> (lo, hi), d)); };
        auto B = [&] (std::string_view id, bool d)
        { l.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { pid::str (id), 1 }, pid::str (id), d)); };
        auto C = [&] (std::string_view id, int n, int d)
        {
            juce::StringArray s;
            for (int i = 0; i < n; ++i) s.add (juce::String (i));
            l.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { pid::str (id), 1 }, pid::str (id), s, d));
        };

        F (pid::limCeiling, -3.0f, 0.0f, -1.0f);
        F (pid::limLookahead, 0.0f, 5.0f, 1.5f);
        F (pid::limRelease, 10.0f, 500.0f, 100.0f);
        B (pid::limAutoRelease, true);
        C (pid::limStyle, 4, 0);
        F (pid::limLink, 0.0f, 100.0f, 100.0f);
        C (pid::limOversampling, 3, 0);
        B (pid::limStage1Enabled, true);
        B (pid::lowLatencyMode, false);
        return l;
    }

    LimiterHost()
        : juce::AudioProcessor (BusesProperties().withInput ("in", juce::AudioChannelSet::stereo()).withOutput ("out", juce::AudioChannelSet::stereo())),
          apvts (*this, nullptr, "P", layout()) {}

    void set (std::string_view id, float v)
    {
        auto* p = apvts.getParameter (pid::str (id));
        p->setValueNotifyingHost (p->convertTo0to1 (v));
    }

    const juce::String getName() const override { return "test"; }
    void prepareToPlay (double, int) override {}
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}
};

/** Runs a module over L/R in blocks, calling updateParameters() per block. */
template <typename Module>
void runModule (Module& m, std::vector<double>& L, std::vector<double>& R, int block = 512)
{
    const int n = (int) L.size();
    for (int o = 0; o < n; o += block)
    {
        const int len = std::min (block, n - o);
        m.updateParameters();
        m.process (L.data() + o, R.data() + o, len);
    }
}

/**
    Independent 16x true-peak estimator (shares no code with TruePeakDetector):
    direct evaluation of a Blackman-windowed sinc reconstruction with a
    +/-64-sample kernel at 16 positions per sample. Returns linear peak.
*/
inline double truePeak16x (const std::vector<double>& x)
{
    constexpr int M = 16, H = 64;
    const double pi = 3.14159265358979323846;
    std::vector<double> k ((size_t) (M * 2 * H));

    for (int p = 0; p < M; ++p)
    {
        double sum = 0.0;
        for (int j = 0; j < 2 * H; ++j)
        {
            const double t = double (p) / M - double (j - H + 1);
            const double w = 0.42 + 0.5 * std::cos (pi * t / H) + 0.08 * std::cos (2.0 * pi * t / H);
            const double s = t == 0.0 ? 1.0 : std::sin (pi * t) / (pi * t);
            k[(size_t) (p * 2 * H + j)] = std::abs (t) < H ? s * w : 0.0;
            sum += k[(size_t) (p * 2 * H + j)];
        }
        for (int j = 0; j < 2 * H; ++j)
            k[(size_t) (p * 2 * H + j)] /= sum;
    }

    double peak = 0.0;
    const int n = (int) x.size();
    for (int c = 0; c < n; ++c)
    {
        peak = std::max (peak, std::abs (x[(size_t) c]));
        for (int p = 1; p < M; ++p)
        {
            double acc = 0.0;
            const double* kp = k.data() + p * 2 * H;
            for (int j = 0; j < 2 * H; ++j)
            {
                const int idx = c + j - H + 1;
                if (idx >= 0 && idx < n)
                    acc += kp[j] * x[(size_t) idx];
            }
            peak = std::max (peak, std::abs (acc));
        }
    }
    return peak;
}

inline double toDb (double v) { return 20.0 * std::log10 (std::max (v, 1.0e-300)); }

} // namespace test
