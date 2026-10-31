#include "MultibandDynamics.h"

#include <algorithm>
#include <cmath>

namespace meridian
{

MultibandDynamics::MultibandDynamics (juce::AudioProcessorValueTreeState& state)
    : DSPModuleBase (state)
{
    namespace f = pid::field;

    for (int i = 0; i < kNumBands; ++i)
    {
        auto& b = bands[(size_t) i];
        b.ratioParam     = rawParam (pid::bandParam (pid::mbdBandPrefix, i, f::ratio));
        b.thresholdParam = rawParam (pid::bandParam (pid::mbdBandPrefix, i, f::threshold));
        b.attackParam    = rawParam (pid::bandParam (pid::mbdBandPrefix, i, f::attack));
        b.releaseParam   = rawParam (pid::bandParam (pid::mbdBandPrefix, i, f::release));
        b.kneeParam      = rawParam (pid::bandParam (pid::mbdBandPrefix, i, f::knee));
        b.makeupParam    = rawParam (pid::bandParam (pid::mbdBandPrefix, i, f::makeup));
        b.upThreshParam  = rawParam (pid::bandParam (pid::mbdBandPrefix, i, f::upwardThresh));
        b.upRatioParam   = rawParam (pid::bandParam (pid::mbdBandPrefix, i, f::upwardRatio));
        b.enabledParam   = rawParam (pid::bandParam (pid::mbdBandPrefix, i, f::enabled));
    }

    for (int i = 0; i < kNumCrossovers; ++i)
        xoverParams[(size_t) i] = rawParam (pid::xoverParam (pid::mbdXoverPrefix, i));
}

void MultibandDynamics::readCrossoverFrequencies (double* out) const noexcept
{
    for (int i = 0; i < kNumCrossovers; ++i)
        out[i] = (double) load (xoverParams[(size_t) i]);
}

void MultibandDynamics::prepareModule (double sampleRate, int maxBlockSize)
{
    const int blockSize = std::max (1, maxBlockSize);

    double freqs[kNumCrossovers];
    readCrossoverFrequencies (freqs);
    crossover.prepare (sampleRate, blockSize);
    crossover.setCrossoverFrequencies (std::vector<double> (freqs, freqs + kNumCrossovers), sampleRate);

    bandStorage.assign ((size_t) (2 * kNumBands * blockSize), 0.0);
    for (int b = 0; b < kNumBands; ++b)
    {
        bandL[(size_t) b] = bandStorage.data() + (size_t) (2 * b) * (size_t) blockSize;
        bandR[(size_t) b] = bandStorage.data() + (size_t) (2 * b + 1) * (size_t) blockSize;
    }

    for (auto& b : bands)
    {
        b.core[0].prepare (sampleRate);
        b.core[1].prepare (sampleRate);
        b.makeup.prepare (sampleRate, 20.0);
        b.enable.prepare (sampleRate, 20.0);
    }

    midSideActive = midSideRequested.load();
    updateParameters();

    for (auto& b : bands)
    {
        b.makeup.reset (b.makeup.getTargetValue());
        b.enable.reset (b.enable.getTargetValue());
    }

    setLatencySamples (crossover.getLatencySamples());
}

void MultibandDynamics::resetModule() noexcept
{
    crossover.reset();

    for (size_t i = 0; i < bands.size(); ++i)
    {
        auto& b = bands[i];
        b.core[0].reset();
        b.core[1].reset();
        b.makeup.reset (b.makeup.getTargetValue());
        b.enable.reset (b.enable.getTargetValue());
        bandGr[i].store (0.0, std::memory_order_relaxed);
    }
}

void MultibandDynamics::updateParameters() noexcept
{
    double freqs[kNumCrossovers];
    readCrossoverFrequencies (freqs);
    crossover.setCrossoverFrequencies (freqs, kNumCrossovers);

    double maxGr = 0.0;

    for (size_t i = 0; i < bands.size(); ++i)
    {
        auto& b = bands[i];
        const double ratio = (double) load (b.ratioParam);
        const double threshold = (double) load (b.thresholdParam);
        const double knee = (double) load (b.kneeParam);
        const double attack = (double) load (b.attackParam);
        const double release = (double) load (b.releaseParam);
        const double upThresh = (double) load (b.upThreshParam);
        const double upRatio = (double) load (b.upRatioParam);

        for (auto& core : b.core)
        {
            core.setParameters (ratio, threshold, knee, attack, release, false);
            core.setUpwardExpansion (upThresh, upRatio);
        }

        b.makeup.setTargetValue (dbToGain (load (b.makeupParam)));
        b.enable.setTargetValue (loadBool (b.enabledParam) ? 1.0 : 0.0);

        maxGr = std::max (maxGr, bandGr[i].load (std::memory_order_relaxed));
    }

    const bool ms = midSideRequested.load (std::memory_order_relaxed);
    if (ms != midSideActive)
    {
        midSideActive = ms;
        for (auto& b : bands)
        {
            b.core[0].reset();
            b.core[1].reset();
        }
    }

    setLatencySamples (crossover.getLatencySamples());
    setGainReductionDb ((float) maxGr);
}

void MultibandDynamics::processBlock (double* L, double* R, int numSamples) noexcept
{
    const int blockSize = maxBlock > 0 ? maxBlock : numSamples;
    int offset = 0;
    std::array<double, kNumBands> blockGr {};

    while (offset < numSamples)
    {
        const int n = std::min (blockSize, numSamples - offset);
        double* l = L + offset;
        double* r = R + offset;

        if (midSideActive)
        {
            for (int i = 0; i < n; ++i)
            {
                const double m = 0.5 * (l[i] + r[i]);
                const double s = 0.5 * (l[i] - r[i]);
                l[i] = m;
                r[i] = s;
            }
        }

        crossover.processBlock (l, r, bandL.data(), bandR.data(), n, kNumBands);

        for (int bi = 0; bi < kNumBands; ++bi)
        {
            auto& b = bands[(size_t) bi];
            double* bl = bandL[(size_t) bi];
            double* br = bandR[(size_t) bi];

            for (int i = 0; i < n; ++i)
            {
                const double en = b.enable.processSample();
                const double mk = b.makeup.processSample();

                if (midSideActive)
                {
                    const double gM = std::pow (10.0, b.core[0].processPower (bl[i] * bl[i]) * 0.05) * mk;
                    const double gS = std::pow (10.0, b.core[1].processPower (br[i] * br[i]) * 0.05) * mk;
                    blockGr[(size_t) bi] = std::max (blockGr[(size_t) bi],
                                                     en * std::max (b.core[0].getGainReductionDb(), b.core[1].getGainReductionDb()));
                    bl[i] *= 1.0 + en * (gM - 1.0);
                    br[i] *= 1.0 + en * (gS - 1.0);
                }
                else
                {
                    const double power = 0.5 * (bl[i] * bl[i] + br[i] * br[i]);
                    const double g = std::pow (10.0, b.core[0].processPower (power) * 0.05) * mk;
                    blockGr[(size_t) bi] = std::max (blockGr[(size_t) bi], en * b.core[0].getGainReductionDb());
                    const double applied = 1.0 + en * (g - 1.0);
                    bl[i] *= applied;
                    br[i] *= applied;
                }
            }
        }

        LinearPhaseCrossover::sumBands (bandL.data(), bandR.data(), l, r, n, kNumBands);

        if (midSideActive)
        {
            for (int i = 0; i < n; ++i)
            {
                const double m = l[i];
                const double s = r[i];
                l[i] = m + s;
                r[i] = m - s;
            }
        }

        offset += n;
    }

    double maxGr = 0.0;
    for (size_t i = 0; i < bands.size(); ++i)
    {
        bandGr[i].store (blockGr[i], std::memory_order_relaxed);
        maxGr = std::max (maxGr, blockGr[i]);
    }

    setGainReductionDb ((float) maxGr);
}

} // namespace meridian
