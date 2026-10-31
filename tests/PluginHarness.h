#pragma once

// Helpers for tests that instantiate the complete MeridianAudioProcessor
// (linked from the plugin's SharedCode library).

#include "PluginProcessor.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace test
{
inline void setParam (MeridianAudioProcessor& p, const juce::String& id, float value)
{
    auto* param = p.getAPVTS().getParameter (id);
    if (param == nullptr)
    {
        std::printf ("FATAL: unknown parameter '%s'\n", id.toRawUTF8());
        std::exit (2);
    }
    param->setValueNotifyingHost (param->convertTo0to1 (value));
}

inline void setParam (MeridianAudioProcessor& p, std::string_view id, float value)
{
    setParam (p, meridian::pid::str (id), value);
}

/** Deterministic pink noise (Paul Kellet's refined filter on xorshift white),
    scaled to the requested RMS in dBFS. */
inline std::vector<double> pinkNoise (int n, double rmsDb, std::uint64_t seed)
{
    std::vector<double> out ((size_t) n);
    std::uint64_t s = seed;
    double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;

    for (int i = 0; i < n; ++i)
    {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        const double w = (double) (s >> 11) * (1.0 / 9007199254740992.0) * 2.0 - 1.0;
        b0 = 0.99886 * b0 + w * 0.0555179;
        b1 = 0.99332 * b1 + w * 0.0750759;
        b2 = 0.96900 * b2 + w * 0.1538520;
        b3 = 0.86650 * b3 + w * 0.3104856;
        b4 = 0.55000 * b4 + w * 0.5329522;
        b5 = -0.7616 * b5 - w * 0.0168980;
        out[(size_t) i] = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362;
        b6 = w * 0.115926;
    }

    double sum = 0.0;
    for (double v : out) sum += v * v;
    const double g = std::pow (10.0, rmsDb / 20.0) / std::sqrt (sum / n);
    for (double& v : out) v *= g;
    return out;
}

/** Runs L/R through the processor in double precision, block by block. */
inline void runProcessor (MeridianAudioProcessor& p, std::vector<double>& L, std::vector<double>& R, int block)
{
    juce::AudioBuffer<double> buf (2, block);
    juce::MidiBuffer midi;
    const int n = (int) L.size();

    for (int o = 0; o < n; o += block)
    {
        const int len = std::min (block, n - o);
        buf.setSize (2, len, false, false, true);
        std::copy (L.begin() + o, L.begin() + o + len, buf.getWritePointer (0));
        std::copy (R.begin() + o, R.begin() + o + len, buf.getWritePointer (1));
        p.processBlock (buf, midi);
        std::copy (buf.getReadPointer (0), buf.getReadPointer (0) + len, L.begin() + o);
        std::copy (buf.getReadPointer (1), buf.getReadPointer (1) + len, R.begin() + o);
    }
}

inline double toDb (double v) { return 20.0 * std::log10 (std::max (v, 1.0e-300)); }
} // namespace test
