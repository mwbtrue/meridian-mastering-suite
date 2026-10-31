#include "LowEndControl.h"

#include <algorithm>
#include <cmath>

namespace meridian
{

LowEndControl::LowEndControl (juce::AudioProcessorValueTreeState& state)
    : DSPModuleBase (state)
{
    monoFreqParam     = rawParam (pid::lecMonoFreq);
    monoEnabledParam  = rawParam (pid::lecMonoEnabled);
    dynEnabledParam   = rawParam (pid::lecDynEnabled);
    dynThresholdParam = rawParam (pid::lecDynThreshold);
    dynMaxGrParam     = rawParam (pid::lecDynMaxGr);
}

void LowEndControl::prepareModule (double sampleRate, int /*maxBlockSize*/)
{
    logFreq.prepare (sampleRate, kFreqGlideMs / 3.0);   // ~100 ms to settle (3 tau)
    monoGain.prepare (sampleRate, 15.0);
    dynMix.prepare (sampleRate, 10.0);

    attackCoeff  = 1.0 - std::exp (-1.0 / (kAttackMs * 0.001 * sampleRate));
    releaseCoeff = 1.0 - std::exp (-1.0 / (kReleaseMs * 0.001 * sampleRate));
    gainCoeff    = 1.0 - std::exp (-1.0 / (kGainSmoothMs * 0.001 * sampleRate));

    sidechain.setCoefficients (Biquad::makeBPF (kSidechainFreqHz, kSidechainQ, sampleRate));

    logFreq.reset (std::log (std::clamp ((double) load (monoFreqParam), 20.0, 0.45 * sampleRate)));
    monoGain.reset (loadBool (monoEnabledParam) ? 0.0 : 1.0);
    dynMix.reset (loadBool (dynEnabledParam) ? 1.0 : 0.0);

    appliedFreq = -1.0;
    updateCrossover (std::exp (logFreq.getCurrentValue()));
    setLatencySamples (0);
}

void LowEndControl::resetModule() noexcept
{
    for (int c = 0; c < 2; ++c)
        for (int s = 0; s < 2; ++s)
        {
            lp[c][s].reset();
            hp[c][s].reset();
        }

    sidechain.reset();
    meanSquare = 0.0;
    subGainDb = 0.0;
    controlCounter = 0;
    subGrDb.store (0.0, std::memory_order_relaxed);

    logFreq.reset (logFreq.getTargetValue());
    monoGain.reset (monoGain.getTargetValue());
    dynMix.reset (dynMix.getTargetValue());
    updateCrossover (std::exp (logFreq.getCurrentValue()));
}

void LowEndControl::updateCrossover (double freqHz) noexcept
{
    if (freqHz == appliedFreq)
        return;

    appliedFreq = freqHz;
    const auto lpc = Biquad::makeLPF (freqHz, 0.70710678118654752, currentSampleRate);
    const auto hpc = Biquad::makeHPF (freqHz, 0.70710678118654752, currentSampleRate);

    for (int c = 0; c < 2; ++c)
        for (int s = 0; s < 2; ++s)
        {
            lp[c][s].setCoefficients (lpc);
            hp[c][s].setCoefficients (hpc);
        }
}

void LowEndControl::updateParameters() noexcept
{
    const double f = std::clamp ((double) load (monoFreqParam), 20.0, 0.45 * currentSampleRate);
    logFreq.setTargetValue (std::log (f));
    monoGain.setTargetValue (loadBool (monoEnabledParam) ? 0.0 : 1.0);
    dynMix.setTargetValue (loadBool (dynEnabledParam) ? 1.0 : 0.0);

    thresholdDb = (double) load (dynThresholdParam);
    maxGrDb = std::abs ((double) load (dynMaxGrParam));

    setGainReductionDb ((float) subGrDb.load (std::memory_order_relaxed));
}

void LowEndControl::processBlock (double* L, double* R, int numSamples) noexcept
{
    constexpr double slopeFactor = 1.0 - 1.0 / kRatio;
    double maxGrThisBlock = 0.0;

    for (int i = 0; i < numSamples; ++i)
    {
        if (logFreq.isSmoothing())
        {
            const double lf = logFreq.processSample();

            if (controlCounter <= 0 || ! logFreq.isSmoothing())
            {
                updateCrossover (std::exp (lf));
                controlCounter = kControlInterval;
            }

            --controlCounter;
        }

        const double mid  = 0.5 * (L[i] + R[i]);
        const double side = 0.5 * (L[i] - R[i]);

        // LR4 split: cascaded Butterworth pairs
        const double mLo = lp[0][1].process (lp[0][0].process (mid));
        const double mHi = hp[0][1].process (hp[0][0].process (mid));
        const double sLo = lp[1][1].process (lp[1][0].process (side));
        const double sHi = hp[1][1].process (hp[1][0].process (side));

        // Dynamic sub detector (always runs so enabling is seamless)
        const double sc = sidechain.process (mid);
        const double sq = sc * sc;
        meanSquare += (sq > meanSquare ? attackCoeff : releaseCoeff) * (sq - meanSquare);

        const double envDb = 10.0 * std::log10 (meanSquare + 1.0e-30);
        double targetGrDb = envDb > thresholdDb ? (envDb - thresholdDb) * slopeFactor : 0.0;
        targetGrDb = std::min (targetGrDb, maxGrDb) * dynMix.processSample();

        subGainDb += gainCoeff * (-targetGrDb - subGainDb);
        maxGrThisBlock = std::max (maxGrThisBlock, -subGainDb);

        const double gSub = subGainDb > -1.0e-9 ? 1.0 : std::pow (10.0, subGainDb * 0.05);

        const double mOut = gSub * mLo + mHi;
        const double sOut = monoGain.processSample() * sLo + sHi;

        L[i] = mOut + sOut;
        R[i] = mOut - sOut;
    }

    for (int c = 0; c < 2; ++c)
        for (int s = 0; s < 2; ++s)
        {
            lp[c][s].snapToZero();
            hp[c][s].snapToZero();
        }

    sidechain.snapToZero();
    if (meanSquare < 1.0e-30) meanSquare = 0.0;
    if (std::abs (subGainDb) < 1.0e-12) subGainDb = 0.0;

    subGrDb.store (maxGrThisBlock, std::memory_order_relaxed);
    setGainReductionDb ((float) maxGrThisBlock);
}

} // namespace meridian
