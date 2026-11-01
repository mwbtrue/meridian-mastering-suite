#include "Clipper.h"
#include "../common/FastMath.h"

#include <algorithm>
#include <cmath>

namespace meridian
{

Clipper::Clipper (juce::AudioProcessorValueTreeState& state) : DSPModuleBase (state)
{
    modeParam       = rawParam (pid::clipperMode);
    ceilingParam    = rawParam (pid::clipperCeiling);
    driveParam      = rawParam (pid::clipperDrive);
    enabledParam    = rawParam (pid::clipperEnabled);
    linkParam       = rawParam (pid::clipperLinkCeiling);
    limCeilingParam = rawParam (pid::limCeiling);
}

void Clipper::prepareModule (double sampleRate, int maxBlockSize)
{
    osBuf.assign ((size_t) maxBlockSize * kFactor, 0.0);
    ceilBuf.assign ((size_t) maxBlockSize * kFactor, 0.0);
    driveBuf.assign ((size_t) maxBlockSize * kFactor, 0.0);
    engine.setOversamplingFactor (kFactor);
    engine.reset();

    for (auto& d : bypassDelay)
        d.prepare (OversamplingEngine::tapsPerPhase + 8);

    ceiling.prepare (sampleRate * kFactor, 20.0);
    drive.prepare (sampleRate * kFactor, 20.0);
    setLatencySamples (engine.getLatencySamples());
}

void Clipper::resetModule() noexcept
{
    engine.reset();
    for (auto& d : bypassDelay)
        d.reset();
    ceiling.reset (ceiling.getTargetValue());
    drive.reset (drive.getTargetValue());
}

void Clipper::updateParameters() noexcept
{
    mode = std::clamp (loadInt (modeParam), 0, 2);
    enabled = loadBool (enabledParam);

    const double ceilDb = std::clamp ((double) load (loadBool (linkParam) ? limCeilingParam : ceilingParam), -3.0, 0.0);
    ceiling.setTargetValue (dbToGain (ceilDb));
    drive.setTargetValue (dbToGain (std::clamp ((double) load (driveParam), 0.0, 3.0)));

    setLatencySamples (engine.getLatencySamples());
}

double Clipper::shape (int clipMode, double u, double c, double driveLin) noexcept
{
    double y;

    switch (clipMode)
    {
        case (int) choices::ClipperMode::Hard:
            y = u;
            break;

        case (int) choices::ClipperMode::Knee:
        {
            const double w = std::min (0.5 * driveLin * c, 1.5 * c);
            const double s = c - 2.0 * w / 3.0;
            const double a = std::abs (u);

            if (a <= s)
                y = u;
            else if (a < s + w)
            {
                const double t = a - s;
                y = std::copysign (a - t * t * t / (3.0 * w * w), u);
            }
            else
                y = std::copysign (c, u);
            break;
        }

        case (int) choices::ClipperMode::Soft:
        default:
            y = c * fastmath::tanh (u / c);
            break;
    }

    return std::clamp (y, -c, c);
}

void Clipper::processBlock (double* L, double* R, int n) noexcept
{
    double* io[2] { L, R };

    if (! enabled)
    {
        for (int ch = 0; ch < 2; ++ch)
            bypassDelay[ch].process (io[ch], io[ch], n, engine.getLatencySamples());

        engine.reset();
        ceiling.reset (ceiling.getTargetValue());
        drive.reset (drive.getTargetValue());
        setGainReductionDb (0.0f);
        return;
    }

    double blockPeakIn = 0.0, blockPeakOut = 0.0;
    const int nOsTotal = n * kFactor;

    // One smoother trajectory shared by both channels.
    for (int i = 0; i < nOsTotal; ++i)
    {
        ceilBuf[(size_t) i] = ceiling.processSample();
        driveBuf[(size_t) i] = drive.processSample();
    }

    for (int ch = 0; ch < 2; ++ch)
    {
        const int nOs = engine.upsample (io[ch], osBuf.data(), n, ch);

        for (int i = 0; i < nOs; ++i)
        {
            const double c = ceilBuf[(size_t) i];
            const double d = driveBuf[(size_t) i];
            const double u = d * osBuf[(size_t) i];
            const double y = shape (mode, u, c, d);
            blockPeakIn = std::max (blockPeakIn, std::abs (u));
            blockPeakOut = std::max (blockPeakOut, std::abs (y));
            osBuf[(size_t) i] = y;
        }

        engine.downsample (osBuf.data(), io[ch], nOs, ch);
    }

    const double gr = blockPeakIn > blockPeakOut && blockPeakOut > 0.0 ? gainToDb (blockPeakIn / blockPeakOut) : 0.0;
    setGainReductionDb ((float) gr);
}

} // namespace meridian
