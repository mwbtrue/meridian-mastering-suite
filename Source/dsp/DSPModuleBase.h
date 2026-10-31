#pragma once

#include "ModuleBase.h"
#include "common/DelayLine.h"
#include "common/MidSideMatrix.h"
#include "common/SmoothParam.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace meridian
{

/**
    Base class for every DSP module implementation.

    It sits between ModuleBase (the processor-facing interface: APVTS access,
    latency / gain-reduction reporting) and the concrete modules:

        ModuleBase  <-  DSPModuleBase  <-  InputModule, CorrectiveEQ, ...

    Two ways to run a module:
      1. Inside the plugin: PluginProcessor calls process(), which forwards to
         processBlock(). The processor itself performs bypass, channel routing,
         wet/dry and delta solo with its own latency-compensated dry paths.
      2. Standalone (offline renders, unit/null tests, the assistant's analysis
         pass): processWithRouting() applies the module's own bypass,
         channelMode, wetDry and deltaMode settings, with the dry path delayed
         by the module latency so blends are always phase-aligned.

    Derived classes implement prepareModule(), resetModule(), processBlock()
    and updateParameters(). prepare()/reset() are final so the routing state is
    always prepared and cleared together with the module.
*/
class DSPModuleBase : public ModuleBase
{
public:
    enum ChannelMode : int { Stereo = 0, Mid = 1, Side = 2, Left = 3, Right = 4 };

    /** Upper bound on module latency supported by processWithRouting()'s dry path. */
    static constexpr int kMaxRoutingLatency = 16384;

    using ModuleBase::ModuleBase;

    // ---- Lifecycle (final: routing state follows the module) ----------------
    void prepare (double sampleRate, int maxBlockSize) final
    {
        currentSampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
        maxBlock = std::max (1, maxBlockSize);

        for (auto* v : { &dryL, &dryR, &workA, &workB })
            v->assign ((size_t) maxBlock, 0.0);

        for (auto* d : { &dryDelayL, &dryDelayR, &passDelay })
            d->prepare (kMaxRoutingLatency);

        wetSmoother.prepare (currentSampleRate, 20.0);
        wetSmoother.reset (wetDry);

        prepareModule (currentSampleRate, maxBlock);
    }

    void reset() noexcept final
    {
        dryDelayL.reset();
        dryDelayR.reset();
        passDelay.reset();
        wetSmoother.reset (wetDry);
        resetModule();
    }

    /** Processor entry point: module processing only (routing done by the processor). */
    void process (double* left, double* right, int numSamples) noexcept final
    {
        processBlock (left, right, numSamples);
    }

    /** In-place stereo processing of the module's own effect. */
    virtual void processBlock (double* L, double* R, int numSamples) noexcept = 0;

    // ---- Standalone routing state ---------------------------------------------
    bool   bypassed    = false;
    double wetDry      = 1.0;      // 0 = dry, 1 = wet
    int    channelMode = Stereo;   // ChannelMode
    bool   deltaMode   = false;    // output = processed - dry

    void setBypassed (bool shouldBypass) noexcept   { bypassed = shouldBypass; }
    void setWetDry (double wet) noexcept            { wetDry = std::clamp (wet, 0.0, 1.0); }
    void setChannel (int mode) noexcept             { channelMode = std::clamp (mode, 0, 4); }
    void setDeltaMode (bool shouldDelta) noexcept   { deltaMode = shouldDelta; }

    double getSampleRate() const noexcept   { return currentSampleRate; }
    int getMaxBlockSize() const noexcept    { return maxBlock; }

    /**
        Applies bypass, channel routing, wet/dry and delta around processBlock().
        Real-time safe. numSamples larger than the prepared block size is
        processed in prepared-size chunks.
    */
    void processWithRouting (double* L, double* R, int numSamples) noexcept
    {
        int offset = 0;

        while (offset < numSamples)
        {
            const int n = std::min (maxBlock, numSamples - offset);
            processChunkWithRouting (L + offset, R + offset, n);
            offset += n;
        }
    }

protected:
    virtual void prepareModule (double sampleRate, int maxBlockSize) = 0;
    virtual void resetModule() noexcept = 0;

    static double dbToGain (double db) noexcept { return std::pow (10.0, db * 0.05); }
    static double gainToDb (double g, double floorDb = -150.0) noexcept
    {
        return g > 0.0 ? std::max (floorDb, 20.0 * std::log10 (g)) : floorDb;
    }

    double currentSampleRate = 48000.0;
    int maxBlock = 512;

private:
    void processChunkWithRouting (double* L, double* R, int n) noexcept
    {
        const int latency = std::min (getLatencySamples(), kMaxRoutingLatency);

        // Latency-aligned dry copy (also used by bypass so latency never changes).
        dryDelayL.process (L, dryL.data(), n, latency);
        dryDelayR.process (R, dryR.data(), n, latency);

        if (bypassed)
        {
            if (deltaMode)
            {
                std::fill (L, L + n, 0.0);
                std::fill (R, R + n, 0.0);
            }
            else
            {
                std::copy (dryL.begin(), dryL.begin() + n, L);
                std::copy (dryR.begin(), dryR.begin() + n, R);
            }

            wetSmoother.reset (wetDry);
            return;
        }

        switch (channelMode)
        {
            case Mid:
            case Side:
            {
                MidSide::encodeStereoBlock (L, R, n);          // L = M, R = S
                double* target = channelMode == Mid ? L : R;
                double* other  = channelMode == Mid ? R : L;

                std::copy (target, target + n, workA.data());
                std::copy (target, target + n, workB.data());
                processBlock (workA.data(), workB.data(), n);
                std::copy (workA.begin(), workA.begin() + n, target);

                passDelay.process (other, other, n, latency);
                MidSide::decodeStereoBlock (L, R, n);
                break;
            }

            case Left:
            case Right:
            {
                double* target = channelMode == Left ? L : R;
                double* other  = channelMode == Left ? R : L;

                std::copy (target, target + n, workA.data());
                std::copy (target, target + n, workB.data());
                processBlock (workA.data(), workB.data(), n);
                std::copy (workA.begin(), workA.begin() + n, target);

                passDelay.process (other, other, n, latency);
                break;
            }

            case Stereo:
            default:
                processBlock (L, R, n);
                break;
        }

        // Wet/dry blend (smoothed) and optional delta.
        wetSmoother.setTargetValue (wetDry);

        for (int i = 0; i < n; ++i)
        {
            const double wet = wetSmoother.processSample();
            const double dl = dryL[(size_t) i];
            const double dr = dryR[(size_t) i];
            const double ml = dl + wet * (L[i] - dl);
            const double mr = dr + wet * (R[i] - dr);
            L[i] = deltaMode ? ml - dl : ml;
            R[i] = deltaMode ? mr - dr : mr;
        }
    }

    std::vector<double> dryL, dryR, workA, workB;
    DelayLine dryDelayL, dryDelayR, passDelay;
    SmoothParam wetSmoother;
};

} // namespace meridian
