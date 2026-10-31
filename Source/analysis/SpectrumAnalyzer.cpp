#include "SpectrumAnalyzer.h"

#include <algorithm>
#include <cmath>

namespace meridian
{

SpectrumAnalyzer::SpectrumAnalyzer()
{
    for (int i = 0; i < kFftSize; ++i)
        window[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * 3.14159265358979323846 * i / kFftSize));
}

void SpectrumAnalyzer::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    ringL.assign ((size_t) kRingSize, 0.0f);
    ringR.assign ((size_t) kRingSize, 0.0f);
    work.assign ((size_t) (2 * kFftSize), 0.0f);
    for (auto* v : { &frameL, &frameR, &frameM, &frameS })
        v->assign ((size_t) kFftSize, 0.0f);

    // Exponential amplitude decay exp(-t / tau), expressed in dB per hop.
    peakDecayDbPerFrame = (float) (20.0 * std::log10 (std::exp (1.0)) * (kHop / sampleRate) / kPeakHoldTau);
    reset();
}

void SpectrumAnalyzer::reset() noexcept
{
    std::fill (ringL.begin(), ringL.end(), 0.0f);
    std::fill (ringR.begin(), ringR.end(), 0.0f);
    writeCount.store (0, std::memory_order_release);
    nextFrameEnd = kFftSize;
    peak.fill (SpectrumFrame::kFloorDb);
}

void SpectrumAnalyzer::pushSamples (const double* left, const double* right, int numSamples) noexcept
{
    if (ringL.empty())
        return;

    std::uint64_t w = writeCount.load (std::memory_order_relaxed);
    constexpr std::uint64_t mask = kRingSize - 1;

    for (int i = 0; i < numSamples; ++i, ++w)
    {
        ringL[(size_t) (w & mask)] = (float) left[i];
        ringR[(size_t) (w & mask)] = (float) right[i];
    }

    writeCount.store (w, std::memory_order_release);
}

void SpectrumAnalyzer::transform (const float* input, float* outDb) noexcept
{
    for (int i = 0; i < kFftSize; ++i)
        work[(size_t) i] = input[i] * window[(size_t) i];
    std::fill (work.begin() + kFftSize, work.end(), 0.0f);

    fft.performRealOnlyForwardTransform (work.data(), true);

    const float scale = 4.0f / (float) kFftSize;
    for (int k = 0; k < kNumBins; ++k)
    {
        const float re = work[(size_t) (2 * k)], im = work[(size_t) (2 * k + 1)];
        float mag = std::sqrt (re * re + im * im) * scale;
        if (k == 0 || k == kNumBins - 1)
            mag *= 0.5f;   // DC and Nyquist are not mirrored
        outDb[k] = std::max (SpectrumFrame::kFloorDb, 20.0f * std::log10 (mag + 1.0e-30f));
    }
}

bool SpectrumAnalyzer::getNextFrame (SpectrumFrame& frame) noexcept
{
    if (ringL.empty())
        return false;

    const std::uint64_t written = writeCount.load (std::memory_order_acquire);
    if (written < nextFrameEnd)
        return false;

    // Fallen too far behind (UI or caller stalled): jump to the newest hop boundary.
    if (written - nextFrameEnd > (std::uint64_t) (kRingSize - kFftSize - 4096))
        nextFrameEnd = written - (written - nextFrameEnd) % (std::uint64_t) kHop;

    constexpr std::uint64_t mask = kRingSize - 1;
    const std::uint64_t start = nextFrameEnd - kFftSize;

    for (int i = 0; i < kFftSize; ++i)
    {
        const auto idx = (size_t) ((start + (std::uint64_t) i) & mask);
        const float l = ringL[idx], r = ringR[idx];
        frameL[(size_t) i] = l;
        frameR[(size_t) i] = r;
        frameM[(size_t) i] = 0.5f * (l + r);
        frameS[(size_t) i] = 0.5f * (l - r);
    }

    nextFrameEnd += kHop;

    transform (frameL.data(), dbL.data());
    transform (frameR.data(), dbR.data());
    transform (frameM.data(), frame.magnitudeMid);
    transform (frameS.data(), frame.magnitudeSide);

    for (int k = 0; k < kNumBins; ++k)
    {
        const double pl = std::pow (10.0, dbL[(size_t) k] * 0.1);
        const double pr = std::pow (10.0, dbR[(size_t) k] * 0.1);
        const float avg = std::max (SpectrumFrame::kFloorDb, (float) (10.0 * std::log10 (0.5 * (pl + pr))));
        frame.magnitude[k] = avg;
        peak[(size_t) k] = std::max (avg, std::max (SpectrumFrame::kFloorDb, peak[(size_t) k] - peakDecayDbPerFrame));
        frame.peakHold[k] = peak[(size_t) k];
    }

    frame.sampleRate = sampleRate;
    return true;
}

} // namespace meridian
