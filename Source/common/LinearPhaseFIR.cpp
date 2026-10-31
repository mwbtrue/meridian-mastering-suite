#include "LinearPhaseFIR.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace meridian
{

namespace
{
    int validatedFFTSize (int size)
    {
        if (size < 64 || ! juce::isPowerOfTwo (size))
            throw std::invalid_argument ("LinearPhaseFIR: fftSize must be a power of two >= 64");
        return size;
    }
}

LinearPhaseFIR::LinearPhaseFIR (int size, double beta)
    : fftSize (validatedFFTSize (size)),
      fftOrder (juce::roundToInt (std::log2 (static_cast<double> (size)))),
      hopSize (size / 4),
      numTaps (size / 2 + 1),
      numBins (size / 2 + 1),
      kaiserBeta (beta)
{
    audioFFT  = std::make_unique<juce::dsp::FFT> (fftOrder);
    designFFT = std::make_unique<juce::dsp::FFT> (fftOrder);

    fftWork.assign (static_cast<size_t> (2 * fftSize), 0.0f);
    designWork.assign (static_cast<size_t> (2 * fftSize), 0.0f);

    for (int ch = 0; ch < 2; ++ch)
    {
        inputFifo[(size_t) ch].assign ((size_t) hopSize, 0.0);
        outputFifo[(size_t) ch].assign ((size_t) hopSize, 0.0);
        overlap[(size_t) ch].assign ((size_t) fftSize, 0.0);
    }

    for (auto& k : kernels)
        k.assign (static_cast<size_t> (2 * numBins), 0.0f);

    window.resize ((size_t) numTaps);
    makeKaiserWindow (window.data(), numTaps, kaiserBeta);

    lastBinMagnitudes.assign ((size_t) numBins, 1.0);

    // Initial unity kernel goes straight into the front buffer.
    designKernel (lastBinMagnitudes, kernels[(size_t) frontIndex]);
}

//==============================================================================
double LinearPhaseFIR::besselI0 (double x) noexcept
{
    // I0(x) = sum_k ((x/2)^k / k!)^2 ; converges quickly for the beta range used.
    const double halfX = 0.5 * x;
    double sum = 1.0;
    double term = 1.0;

    for (int k = 1; k < 64; ++k)
    {
        const double f = halfX / static_cast<double> (k);
        term *= f * f;
        sum += term;

        if (term < sum * 1.0e-17)
            break;
    }

    return sum;
}

void LinearPhaseFIR::makeKaiserWindow (double* w, int length, double beta) noexcept
{
    if (length <= 0)
        return;

    if (length == 1)
    {
        w[0] = 1.0;
        return;
    }

    const double denom = besselI0 (beta);
    const double m = static_cast<double> (length - 1);

    for (int n = 0; n < length; ++n)
    {
        const double r = (2.0 * n) / m - 1.0;
        const double arg = beta * std::sqrt (std::max (0.0, 1.0 - r * r));
        w[n] = besselI0 (arg) / denom;
    }
}

//==============================================================================
void LinearPhaseFIR::designKernel (const std::vector<double>& binMagnitudes, std::vector<float>& kernelOut)
{
    // 1) Zero-phase spectrum -> impulse response (real, even, centred at t = 0).
    std::fill (designWork.begin(), designWork.end(), 0.0f);

    for (int k = 0; k < numBins; ++k)
    {
        const double mag = std::max (0.0, binMagnitudes[(size_t) k]);
        designWork[(size_t) (2 * k)]     = static_cast<float> (mag);
        designWork[(size_t) (2 * k + 1)] = 0.0f;
    }

    designFFT->performRealOnlyInverseTransform (designWork.data());

    // 2) Truncate to L = N/2 + 1 taps around t = 0, window, place causally centred at N/4.
    std::vector<float> causal ((size_t) (2 * fftSize), 0.0f);
    const int halfSpan = (numTaps - 1) / 2;   // N / 4

    for (int t = -halfSpan; t <= halfSpan; ++t)
    {
        const int src = (t + fftSize) % fftSize;
        const int dst = t + halfSpan;
        causal[(size_t) dst] = static_cast<float> (designWork[(size_t) src] * window[(size_t) dst]);
    }

    // 3) Kernel spectrum used by the overlap-add convolution.
    designFFT->performRealOnlyForwardTransform (causal.data(), true);

    std::copy (causal.begin(), causal.begin() + 2 * numBins, kernelOut.begin());
}

void LinearPhaseFIR::publishKernel (const std::vector<float>& kernel)
{
    auto& back = kernels[(size_t) backIndex];
    std::copy (kernel.begin(), kernel.end(), back.begin());

    const int previous = middleIndex.exchange (backIndex | dirtyFlag, std::memory_order_acq_rel);
    backIndex = previous & ~dirtyFlag;
}

void LinearPhaseFIR::setMagnitudeResponse (const std::vector<double>& magnitudes, int sourceFFTSize)
{
    if (magnitudes.empty())
        return;

    std::lock_guard<std::mutex> lock (writerMutex);

    std::vector<double> bins ((size_t) numBins, 1.0);
    const int srcBins = static_cast<int> (magnitudes.size());
    const int expectedSrcBins = sourceFFTSize / 2 + 1;

    if (srcBins == numBins && expectedSrcBins == numBins)
    {
        bins = magnitudes;
    }
    else if (srcBins == 1)
    {
        std::fill (bins.begin(), bins.end(), magnitudes.front());
    }
    else
    {
        // Map each target bin (fraction of Nyquist) onto the source grid.
        const double srcLast = static_cast<double> (srcBins - 1);
        const double dstLast = static_cast<double> (numBins - 1);

        for (int k = 0; k < numBins; ++k)
        {
            const double pos = (static_cast<double> (k) / dstLast) * srcLast;
            const int i0 = std::min (static_cast<int> (pos), srcBins - 1);
            const int i1 = std::min (i0 + 1, srcBins - 1);
            const double frac = pos - static_cast<double> (i0);
            bins[(size_t) k] = magnitudes[(size_t) i0] + (magnitudes[(size_t) i1] - magnitudes[(size_t) i0]) * frac;
        }
    }

    lastBinMagnitudes = bins;

    std::vector<float> kernel ((size_t) (2 * numBins), 0.0f);
    designKernel (bins, kernel);
    publishKernel (kernel);
}

void LinearPhaseFIR::setUnityResponse()
{
    setMagnitudeResponse (std::vector<double> ((size_t) numBins, 1.0), fftSize);
}

void LinearPhaseFIR::setKaiserBeta (double newBeta)
{
    std::lock_guard<std::mutex> lock (writerMutex);

    kaiserBeta = std::max (0.0, newBeta);
    makeKaiserWindow (window.data(), numTaps, kaiserBeta);

    std::vector<float> kernel ((size_t) (2 * numBins), 0.0f);
    designKernel (lastBinMagnitudes, kernel);
    publishKernel (kernel);
}

//==============================================================================
void LinearPhaseFIR::reset() noexcept
{
    for (int ch = 0; ch < 2; ++ch)
    {
        std::fill (inputFifo[(size_t) ch].begin(), inputFifo[(size_t) ch].end(), 0.0);
        std::fill (outputFifo[(size_t) ch].begin(), outputFifo[(size_t) ch].end(), 0.0);
        std::fill (overlap[(size_t) ch].begin(), overlap[(size_t) ch].end(), 0.0);
    }

    fifoPosition = 0;
    acquireLatestKernel();
}

void LinearPhaseFIR::acquireLatestKernel() noexcept
{
    if ((middleIndex.load (std::memory_order_acquire) & dirtyFlag) == 0)
        return;

    const int previous = middleIndex.exchange (frontIndex, std::memory_order_acq_rel);
    frontIndex = previous & ~dirtyFlag;
}

void LinearPhaseFIR::processBlock (double* left, double* right, int numSamples) noexcept
{
    double* const channels[2] = { left, right };

    int done = 0;

    while (done < numSamples)
    {
        const int chunk = std::min (numSamples - done, hopSize - fifoPosition);

        for (int ch = 0; ch < 2; ++ch)
        {
            double* io = channels[ch];

            if (io == nullptr)
                continue;

            double* in  = inputFifo[(size_t) ch].data() + fifoPosition;
            double* out = outputFifo[(size_t) ch].data() + fifoPosition;

            for (int i = 0; i < chunk; ++i)
            {
                in[i] = io[done + i];
                io[done + i] = out[i];
            }
        }

        fifoPosition += chunk;
        done += chunk;

        if (fifoPosition == hopSize)
        {
            processHop();
            fifoPosition = 0;
        }
    }
}

void LinearPhaseFIR::processHop() noexcept
{
    acquireLatestKernel();
    const float* H = kernels[(size_t) frontIndex].data();

    for (int ch = 0; ch < 2; ++ch)
    {
        auto& in = inputFifo[(size_t) ch];
        auto& ola = overlap[(size_t) ch];
        auto& out = outputFifo[(size_t) ch];

        std::fill (fftWork.begin(), fftWork.end(), 0.0f);

        for (int i = 0; i < hopSize; ++i)
            fftWork[(size_t) i] = static_cast<float> (in[(size_t) i]);

        audioFFT->performRealOnlyForwardTransform (fftWork.data(), true);

        for (int k = 0; k < numBins; ++k)
        {
            const float xr = fftWork[(size_t) (2 * k)];
            const float xi = fftWork[(size_t) (2 * k + 1)];
            const float hr = H[2 * k];
            const float hi = H[2 * k + 1];
            fftWork[(size_t) (2 * k)]     = xr * hr - xi * hi;
            fftWork[(size_t) (2 * k + 1)] = xr * hi + xi * hr;
        }

        audioFFT->performRealOnlyInverseTransform (fftWork.data());

        // Linear convolution result occupies [0, B + L - 1) = [0, 3N/4).
        for (int i = 0; i < fftSize; ++i)
            ola[(size_t) i] += static_cast<double> (fftWork[(size_t) i]);

        std::copy (ola.begin(), ola.begin() + hopSize, out.begin());
        std::move (ola.begin() + hopSize, ola.end(), ola.begin());
        std::fill (ola.end() - hopSize, ola.end(), 0.0);
    }
}

} // namespace meridian
