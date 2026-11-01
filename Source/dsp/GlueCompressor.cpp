#include "GlueCompressor.h"
#include "../common/FastMath.h"

#include <algorithm>
#include <cmath>

namespace meridian
{

//==============================================================================
// VcaCompressorCore
//==============================================================================
double VcaCompressorCore::msToCoeff (double ms, double sampleRate) noexcept
{
    const double samples = std::max (ms, 0.01) * 0.001 * sampleRate;
    return 1.0 - std::exp (-1.0 / samples);
}

void VcaCompressorCore::prepare (double sampleRate) noexcept
{
    fs = sampleRate;
    rmsCoeff = msToCoeff (kRmsWindowMs, fs);
    windowCoeff = 1.0 - std::exp (-(double) kAutoReleaseInterval / (kAutoReleaseWindowMs * 0.001 * fs));
    setParameters (ratio, thresholdDb, kneeDb, 10.0, releaseMs, autoRelease);
    reset();
}

void VcaCompressorCore::reset() noexcept
{
    meanSquare = 0.0;
    rmsPower = 0.0;
    levelMean = -120.0;
    levelVar = 0.0;
    autoCounter = 0;
    lastGrDb = 0.0;
    activeReleaseCoeff = releaseCoeff;
}

void VcaCompressorCore::setParameters (double newRatio, double newThresholdDb, double newKneeDb,
                                       double attackMs, double newReleaseMs, bool newAutoRelease) noexcept
{
    ratio = std::max (1.0, newRatio);
    thresholdDb = newThresholdDb;
    kneeDb = std::max (0.0, newKneeDb);
    attackCoeff = msToCoeff (attackMs, fs);
    releaseMs = std::max (1.0, newReleaseMs);
    releaseCoeff = msToCoeff (releaseMs, fs);

    if (! newAutoRelease)
        activeReleaseCoeff = releaseCoeff;

    autoRelease = newAutoRelease;
}

void VcaCompressorCore::setUpwardExpansion (double newThresholdDb, double newRatio) noexcept
{
    upThresholdDb = newThresholdDb;
    upRatio = std::max (1.0, newRatio);
}

double VcaCompressorCore::downwardGainDb (double x, double t, double r, double w) noexcept
{
    const double over = x - t;

    if (2.0 * over < -w)
        return 0.0;

    if (w > 0.0 && 2.0 * std::abs (over) <= w)
    {
        const double k = over + 0.5 * w;
        return (1.0 / r - 1.0) * k * k / (2.0 * w);
    }

    return (t + over / r) - x;
}

double VcaCompressorCore::upwardGainDb (double x, double t, double r) noexcept
{
    if (r <= 1.0 || x >= t)
        return 0.0;

    const double gain = std::min ((t - x) * (1.0 - 1.0 / r), kMaxUpwardGainDb);
    const double floorFade = std::clamp ((x + 90.0) / 10.0, 0.0, 1.0);
    return gain * floorFade;
}

void VcaCompressorCore::updateAutoRelease() noexcept
{
    const double levelDb = fastmath::powerToDb (meanSquare + 1.0e-30);
    const double diff = levelDb - levelMean;
    levelMean += windowCoeff * diff;
    levelVar += windowCoeff * (diff * diff - levelVar);

    const double stdDb = std::sqrt (std::max (0.0, levelVar));
    const double scale = std::clamp (stdDb / 6.0, 0.2, 1.0) / (1.0 + lastGrDb / 12.0);
    activeReleaseCoeff = msToCoeff (std::max (5.0, releaseMs * scale), fs);
}

double VcaCompressorCore::processPower (double power) noexcept
{
    rmsPower += rmsCoeff * (power - rmsPower);
    meanSquare += (rmsPower > meanSquare ? attackCoeff : activeReleaseCoeff) * (rmsPower - meanSquare);

    if (meanSquare < 1.0e-30)
        meanSquare = rmsPower = 0.0;

    if (autoRelease && --autoCounter <= 0)
    {
        autoCounter = kAutoReleaseInterval;
        updateAutoRelease();
    }

    const double levelDb = fastmath::powerToDb (meanSquare + 1.0e-30);
    const double down = downwardGainDb (levelDb, thresholdDb, ratio, kneeDb);
    lastGrDb = -down;
    return down + upwardGainDb (levelDb, upThresholdDb, upRatio);
}

//==============================================================================
// GlueCompressor
//==============================================================================
GlueCompressor::GlueCompressor (juce::AudioProcessorValueTreeState& state)
    : DSPModuleBase (state)
{
    ratioParam       = rawParam (pid::glueRatio);
    thresholdParam   = rawParam (pid::glueThreshold);
    attackParam      = rawParam (pid::glueAttack);
    releaseParam     = rawParam (pid::glueRelease);
    autoReleaseParam = rawParam (pid::glueAutoRelease);
    kneeParam        = rawParam (pid::glueKnee);
    makeupParam      = rawParam (pid::glueMakeup);
    mixParam         = rawParam (pid::glueMix);
    scHpfParam       = rawParam (pid::glueScHpf);
    lookaheadParam   = rawParam (pid::glueLookahead);
    scChannelParam   = rawParam (pid::glueScChannel);
}

void GlueCompressor::prepareModule (double sampleRate, int /*maxBlockSize*/)
{
    core.prepare (sampleRate);

    const int maxLookahead = (int) std::ceil (kMaxLookaheadMs * 0.001 * sampleRate);
    delay[0].prepare (maxLookahead);
    delay[1].prepare (maxLookahead);

    makeupGain.prepare (sampleRate, 20.0);
    mix.prepare (sampleRate, 20.0);
    makeupGain.reset (dbToGain (load (makeupParam)));
    mix.reset (std::clamp ((double) load (mixParam) * 0.01, 0.0, 1.0));

    scFilterFreq = -1.0;
    updateParameters();
    makeupGain.reset (makeupGain.getTargetValue());
    mix.reset (mix.getTargetValue());
}

void GlueCompressor::resetModule() noexcept
{
    core.reset();
    scFilter[0].reset();
    scFilter[1].reset();
    delay[0].reset();
    delay[1].reset();
    makeupGain.reset (makeupGain.getTargetValue());
    mix.reset (mix.getTargetValue());
    gainReductionDb.store (0.0, std::memory_order_relaxed);
}

void GlueCompressor::updateSidechainFilter (double freq) noexcept
{
    if (freq == scFilterFreq)
        return;

    scFilterFreq = freq;
    const auto c = Biquad::makeHPF (Biquad::clampFrequency (freq, currentSampleRate), 0.70710678118654752, currentSampleRate);
    scFilter[0].setCoefficients (c);
    scFilter[1].setCoefficients (c);
}

void GlueCompressor::updateParameters() noexcept
{
    core.setParameters ((double) load (ratioParam), (double) load (thresholdParam), (double) load (kneeParam),
                        (double) load (attackParam), (double) load (releaseParam), loadBool (autoReleaseParam));

    updateSidechainFilter ((double) load (scHpfParam));

    const bool useMid = loadInt (scChannelParam) == 1;
    if (useMid != scUseMid)
    {
        scUseMid = useMid;
        scFilter[0].reset();
        scFilter[1].reset();
    }

    makeupGain.setTargetValue (dbToGain (load (makeupParam)));
    mix.setTargetValue (std::clamp ((double) load (mixParam) * 0.01, 0.0, 1.0));

    const int maxLookahead = delay[0].getMaxDelay();
    lookaheadSamples = std::clamp ((int) std::lround ((double) load (lookaheadParam) * 0.001 * currentSampleRate), 0, maxLookahead);
    setLatencySamples (lookaheadSamples);

    setGainReductionDb ((float) gainReductionDb.load (std::memory_order_relaxed));
}

void GlueCompressor::processBlock (double* L, double* R, int numSamples) noexcept
{
    double maxGr = 0.0;

    for (int i = 0; i < numSamples; ++i)
    {
        const double inL = L[i];
        const double inR = R[i];

        // Sidechain from the undelayed input (lookahead).
        double power;
        if (scUseMid)
        {
            const double m = scFilter[0].process (0.5 * (inL + inR));
            power = m * m;
        }
        else
        {
            const double l = scFilter[0].process (inL);
            const double r = scFilter[1].process (inR);
            power = 0.5 * (l * l + r * r);
        }

        const double gainDb = core.processPower (power);
        maxGr = std::max (maxGr, core.getGainReductionDb());

        const double delayedL = delay[0].processSample (inL, lookaheadSamples);
        const double delayedR = delay[1].processSample (inR, lookaheadSamples);

        const double wetGain = fastmath::dbToAmp (gainDb) * makeupGain.processSample();
        const double m = mix.processSample();
        const double g = (1.0 - m) + m * wetGain;

        L[i] = delayedL * g;
        R[i] = delayedR * g;
    }

    scFilter[0].snapToZero();
    scFilter[1].snapToZero();

    gainReductionDb.store (maxGr, std::memory_order_relaxed);
    setGainReductionDb ((float) maxGr);
}

} // namespace meridian
