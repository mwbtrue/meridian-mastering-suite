#include "InputModule.h"

#include <cmath>

namespace meridian
{

InputModule::InputModule (juce::AudioProcessorValueTreeState& state)
    : DSPModuleBase (state)
{
    trimParam     = rawParam (pid::inputTrim);
    polarityParam = rawParam (pid::inputPolarity);
    dcBlockParam  = rawParam (pid::inputDcBlock);
}

void InputModule::prepareModule (double sampleRate, int /*maxBlockSize*/)
{
    trimGain.prepare (sampleRate, 20.0);
    polarityGain.prepare (sampleRate, 5.0);
    dcMix.prepare (sampleRate, 10.0);

    dcR = std::exp (-2.0 * juce::MathConstants<double>::pi * kDcCutoffHz / sampleRate);
    peakRelease = std::exp (-1.0 / (kPeakReleaseMs * 0.001 * sampleRate));
    rmsCoeff = 1.0 - std::exp (-1.0 / (kRmsWindowMs * 0.001 * sampleRate));

    trimGain.reset (dbToGain (load (trimParam)));
    polarityGain.reset (loadBool (polarityParam) ? -1.0 : 1.0);
    dcMix.reset (loadBool (dcBlockParam) ? 1.0 : 0.0);
    setLatencySamples (0);
}

void InputModule::resetModule() noexcept
{
    for (int c = 0; c < 2; ++c)
    {
        dcX1[c] = dcY1[c] = 0.0;
        peakState[c] = meanSquare[c] = 0.0;
        peak[c].store (0.0, std::memory_order_relaxed);
        rms[c].store (0.0, std::memory_order_relaxed);
    }

    trimGain.reset (trimGain.getTargetValue());
    polarityGain.reset (polarityGain.getTargetValue());
    dcMix.reset (dcMix.getTargetValue());
}

void InputModule::updateParameters() noexcept
{
    trimGain.setTargetValue (dbToGain (load (trimParam)));
    polarityGain.setTargetValue (loadBool (polarityParam) ? -1.0 : 1.0);
    dcMix.setTargetValue (loadBool (dcBlockParam) ? 1.0 : 0.0);
    setGainReductionDb (0.0f);
}

void InputModule::processBlock (double* L, double* R, int numSamples) noexcept
{
    double* io[2] = { L, R };

    for (int i = 0; i < numSamples; ++i)
    {
        const double g = trimGain.processSample() * polarityGain.processSample();
        const double mix = dcMix.processSample();

        for (int c = 0; c < 2; ++c)
        {
            const double x = io[c][i] * g;

            // y[n] = x[n] - x[n-1] + R * y[n-1]
            const double y = x - dcX1[c] + dcR * dcY1[c];
            dcX1[c] = x;
            dcY1[c] = y;

            const double out = x + mix * (y - x);
            io[c][i] = out;

            const double a = std::abs (out);
            peakState[c] = a > peakState[c] ? a : peakState[c] * peakRelease;
            meanSquare[c] += rmsCoeff * (out * out - meanSquare[c]);
        }
    }

    for (int c = 0; c < 2; ++c)
    {
        if (std::abs (dcY1[c]) < 1.0e-30) dcY1[c] = 0.0;
        if (meanSquare[c] < 1.0e-30)      meanSquare[c] = 0.0;
        if (peakState[c] < 1.0e-30)       peakState[c] = 0.0;

        peak[c].store (peakState[c], std::memory_order_relaxed);
        rms[c].store (std::sqrt (meanSquare[c]), std::memory_order_relaxed);
    }
}

} // namespace meridian
