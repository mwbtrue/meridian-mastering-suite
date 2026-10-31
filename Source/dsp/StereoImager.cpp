#include "StereoImager.h"

#include <algorithm>
#include <cmath>

namespace meridian
{

StereoImager::StereoImager (juce::AudioProcessorValueTreeState& state) : DSPModuleBase (state)
{
    for (int i = 0; i < kNumImgXovers; ++i)
        xoverParams[(size_t) i] = rawParam (pid::xoverParam (pid::imgXoverPrefix, i));

    for (int i = 0; i < kBands; ++i)
    {
        widthParams[(size_t) i]   = rawParam (pid::bandParam (pid::imgBandPrefix, i, pid::field::width));
        enabledParams[(size_t) i] = rawParam (pid::bandParam (pid::imgBandPrefix, i, pid::field::enabled));
        bandCorrelation[(size_t) i].store (1.0f);
        bandGuard[(size_t) i].store (1.0f);
    }

    guardParam = rawParam (pid::imgCorrGuard);
    lowLatencyParam = rawParam (pid::lowLatencyMode);
}

void StereoImager::prepareModule (double sampleRate, int maxBlockSize)
{
    linearSplit.prepare (sampleRate, maxBlockSize);
    minSplit.prepare (sampleRate);

    for (int b = 0; b < kBands; ++b)
    {
        bufL[(size_t) b].assign ((size_t) maxBlockSize, 0.0);
        bufR[(size_t) b].assign ((size_t) maxBlockSize, 0.0);
    }

    inL.assign ((size_t) maxBlockSize, 0.0);
    inR.assign ((size_t) maxBlockSize, 0.0);

    ringBlocks = std::max (1, (int) std::lround (kWindowMs * 0.001 * sampleRate / kGuardInterval));

    for (auto& band : bands)
    {
        band.ringM.assign ((size_t) ringBlocks, 0.0);
        band.ringS.assign ((size_t) ringBlocks, 0.0);
    }

    guardCoeff = 1.0 - std::exp (-1.0 / (kGuardSmoothingMs * 0.001 * sampleRate));

    const double defaults[kNumImgXovers] { 120.0, 1000.0, 8000.0 };
    linearSplit.setCrossoverFrequencies (defaults, kNumImgXovers);
    minSplit.setCrossovers (defaults, kBands);
    linearSplit.reset();
}

void StereoImager::resetModule() noexcept
{
    linearSplit.reset();
    minSplit.reset();
    guardCounter = 0;

    for (int b = 0; b < kBands; ++b)
    {
        auto& band = bands[(size_t) b];
        band.guard = band.guardTarget = 1.0;
        band.accM = band.accS = band.sumM = band.sumS = 0.0;
        band.ringPos = 0;
        std::fill (band.ringM.begin(), band.ringM.end(), 0.0);
        std::fill (band.ringS.begin(), band.ringS.end(), 0.0);
        bandCorrelation[(size_t) b].store (1.0f, std::memory_order_relaxed);
        bandGuard[(size_t) b].store (1.0f, std::memory_order_relaxed);
    }
}

void StereoImager::updateParameters() noexcept
{
    double f[kNumImgXovers];
    double lo = 20.0;
    for (int i = 0; i < kNumImgXovers; ++i)
    {
        f[i] = std::clamp ((double) load (xoverParams[(size_t) i]), lo, 0.45 * currentSampleRate);
        lo = f[i] * 1.12246204830937;   // >= 1/6 octave apart
    }

    linearSplit.setCrossoverFrequencies (f, kNumImgXovers);
    minSplit.setCrossovers (f, kBands);

    for (int b = 0; b < kBands; ++b)
    {
        bands[(size_t) b].width = std::clamp ((double) load (widthParams[(size_t) b]) * 0.01, 0.0, 2.0);
        bands[(size_t) b].enabled = loadBool (enabledParams[(size_t) b]);
    }

    corrGuard = loadBool (guardParam);

    const bool ll = loadBool (lowLatencyParam);
    if (ll != lowLatency)
    {
        lowLatency = ll;
        linearSplit.reset();
        minSplit.reset();
    }

    setLatencySamples (lowLatency ? 0 : linearSplit.getLatencySamples());
}

void StereoImager::updateGuard (Band& band, int index) noexcept
{
    // Close the current 64-sample block into the 50 ms sliding sums.
    band.sumM += band.accM - band.ringM[(size_t) band.ringPos];
    band.sumS += band.accS - band.ringS[(size_t) band.ringPos];
    band.ringM[(size_t) band.ringPos] = band.accM;
    band.ringS[(size_t) band.ringPos] = band.accS;
    band.ringPos = (band.ringPos + 1) % ringBlocks;
    band.accM = band.accS = 0.0;

    // Guard against drift of the running sums.
    band.sumM = std::max (0.0, band.sumM);
    band.sumS = std::max (0.0, band.sumS);

    const double w = band.enabled ? band.width : 1.0;
    const double eSw = w * w * band.sumS;

    if (corrGuard && band.enabled && eSw > band.sumM && eSw > 1.0e-20)
        band.guardTarget = std::sqrt (band.sumM / eSw);
    else
        band.guardTarget = 1.0;

    const double eff = w * band.guard;
    const double eS = eff * eff * band.sumS;
    const double denom = band.sumM + eS;
    const double corr = denom > 1.0e-20 ? (band.sumM - eS) / denom : 1.0;

    bandCorrelation[(size_t) index].store ((float) corr, std::memory_order_relaxed);
    bandGuard[(size_t) index].store ((float) band.guard, std::memory_order_relaxed);
}

void StereoImager::processBand (int b, double* bl, double* br, int n) noexcept
{
    auto& band = bands[(size_t) b];
    const double w = band.enabled ? band.width : 1.0;
    const bool guardActive = corrGuard && band.enabled;

    if (! guardActive)
        band.guardTarget = 1.0;

    for (int i = 0; i < n; ++i)
    {
        const double m = 0.5 * (bl[i] + br[i]);
        const double s = 0.5 * (bl[i] - br[i]);
        band.accM += m * m;
        band.accS += s * s;

        band.guard += guardCoeff * (band.guardTarget - band.guard);
        if (std::abs (band.guard - band.guardTarget) < 1.0e-12)
            band.guard = band.guardTarget;

        const double sw = s * w * band.guard;
        bl[i] = m + sw;
        br[i] = m - sw;
    }
}

void StereoImager::processBlock (double* L, double* R, int n) noexcept
{
    double* bl[kBands];
    double* br[kBands];
    for (int b = 0; b < kBands; ++b)
    {
        bl[b] = bufL[(size_t) b].data();
        br[b] = bufR[(size_t) b].data();
    }

    if (lowLatency)
    {
        double split[LinkwitzRileyBank::kMaxBands];
        for (int i = 0; i < n; ++i)
        {
            minSplit.processSample (0, L[i], split);
            for (int b = 0; b < kBands; ++b) bl[b][i] = split[b];
            minSplit.processSample (1, R[i], split);
            for (int b = 0; b < kBands; ++b) br[b][i] = split[b];
        }
        minSplit.snapToZero();
    }
    else
    {
        std::copy (L, L + n, inL.data());
        std::copy (R, R + n, inR.data());
        linearSplit.processBlock (inL.data(), inR.data(), bl, br, n, kBands);
    }

    // Process in guard-interval chunks so the guard updates every 64 samples.
    int offset = 0;
    while (offset < n)
    {
        const int len = std::min (kGuardInterval - guardCounter, n - offset);

        for (int b = 0; b < kBands; ++b)
            processBand (b, bl[b] + offset, br[b] + offset, len);

        guardCounter += len;
        offset += len;

        if (guardCounter >= kGuardInterval)
        {
            guardCounter = 0;
            for (int b = 0; b < kBands; ++b)
                updateGuard (bands[(size_t) b], b);
        }
    }

    LinearPhaseCrossover::sumBands (bl, br, L, R, n, kBands);

    double minGuard = 1.0;
    for (const auto& band : bands)
        minGuard = std::min (minGuard, band.guard);
    setGainReductionDb ((float) -gainToDb (minGuard));
}

} // namespace meridian
