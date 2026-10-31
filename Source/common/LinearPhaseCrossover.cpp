#include "LinearPhaseCrossover.h"
#include "LinearPhaseFIR.h"

#include <algorithm>
#include <cmath>

namespace meridian
{

namespace
{
    constexpr double kPi = 3.14159265358979323846;
    constexpr int P = LinearPhaseCrossover::kPartitionSize;
    constexpr int kBins = P + 1;
    constexpr double kMinSpacingRatio = 1.122462048309373;   // 2^(1/6)
}

int LinearPhaseCrossover::halfLengthForSampleRate (double sampleRate) noexcept
{
    if (sampleRate <= 50000.0)  return 1024;
    if (sampleRate <= 100000.0) return 2048;
    return 4096;
}

//==============================================================================
void LinearPhaseCrossover::prepare (double sampleRate, int /*maxBlockSize*/)
{
    fs = sampleRate > 0.0 ? sampleRate : 48000.0;
    halfLength = halfLengthForSampleRate (fs);
    numTaps = 2 * halfLength + 1;
    numPartitions = (numTaps + P - 1) / P;

    window.assign ((size_t) numTaps, 0.0);
    LinearPhaseFIR::makeKaiserWindow (window.data(), numTaps, kKaiserBeta);
    lpPrev.assign ((size_t) numTaps, 0.0);
    lpCur.assign ((size_t) numTaps, 0.0);
    bandKernel.assign ((size_t) numTaps, 0.0);

    fft = std::make_unique<juce::dsp::FFT> (juce::roundToInt (std::log2 (2.0 * P)));
    fftWork.assign ((size_t) (4 * P), 0.0f);
    accum.assign ((size_t) (2 * kBins), 0.0f);
    accumOld.assign ((size_t) (2 * kBins), 0.0f);
    tempOld.assign ((size_t) P, 0.0);
    tempNew.assign ((size_t) P, 0.0);

    for (auto& set : kernelSets)
        for (auto& band : set.parts)
            band.assign ((size_t) numPartitions, Spectrum ((size_t) (2 * kBins), 0.0f));

    for (auto& ch : channels)
    {
        ch.history.assign ((size_t) (2 * P), 0.0f);
        ch.fdl.assign ((size_t) numPartitions, Spectrum ((size_t) (2 * kBins), 0.0f));
        ch.inFifo.assign ((size_t) P, 0.0);
        ch.delayFifo.assign ((size_t) P, 0.0);
        for (auto& o : ch.outFifo)
            o.assign ((size_t) P, 0.0);
        ch.delay.prepare (halfLength);
    }

    prepared = true;
    designPending = true;
    reset();
}

void LinearPhaseCrossover::reset() noexcept
{
    if (! prepared)
        return;

    if (designPending)
    {
        designInto (kernelSets[(size_t) activeSet]);
        designedBands = kernelSets[(size_t) activeSet].numBands;
        designPending = false;
    }

    fading = false;

    for (auto& ch : channels)
    {
        std::fill (ch.history.begin(), ch.history.end(), 0.0f);
        for (auto& s : ch.fdl)
            std::fill (s.begin(), s.end(), 0.0f);
        std::fill (ch.inFifo.begin(), ch.inFifo.end(), 0.0);
        std::fill (ch.delayFifo.begin(), ch.delayFifo.end(), 0.0);
        for (auto& o : ch.outFifo)
            std::fill (o.begin(), o.end(), 0.0);
        ch.delay.reset();
    }

    fdlPos = 0;
    fifoPos = 0;
}

//==============================================================================
void LinearPhaseCrossover::setCrossoverFrequencies (const double* freqs, int numFreqs) noexcept
{
    numFreqs = std::clamp (numFreqs, 0, kMaxCrossovers);
    const double lo = 20.0;
    const double hi = 0.45 * fs;

    bool changed = (numFreqs + 1) != requestedBands;
    double previous = 0.0;

    for (int i = 0; i < numFreqs; ++i)
    {
        double f = std::clamp (freqs[i], lo, hi);

        if (i > 0)
            f = std::max (f, previous * kMinSpacingRatio);

        f = std::min (f, hi);
        changed = changed || std::abs (f - requestedFreqs[(size_t) i]) > 1.0e-6;
        requestedFreqs[(size_t) i] = f;
        previous = f;
    }

    requestedBands = numFreqs + 1;

    if (changed)
        designPending = true;
}

void LinearPhaseCrossover::setCrossoverFrequencies (const std::vector<double>& freqs, double sampleRate)
{
    if (! prepared || std::abs (sampleRate - fs) > 1.0e-9)
        prepare (sampleRate, 0);

    setCrossoverFrequencies (freqs.data(), (int) freqs.size());

    if (designPending)
    {
        designInto (kernelSets[(size_t) activeSet]);
        designedBands = kernelSets[(size_t) activeSet].numBands;
        designPending = false;
        fading = false;
    }
}

//==============================================================================
void LinearPhaseCrossover::designLowPass (double cutoff, double* out) noexcept
{
    const double x = 2.0 * cutoff / fs;   // normalised cutoff (1 = Nyquist)
    double sum = 0.0;

    for (int n = 0; n < numTaps; ++n)
    {
        const double t = static_cast<double> (n - halfLength);
        const double arg = kPi * x * t;
        const double sinc = (n == halfLength) ? 1.0 : std::sin (arg) / arg;
        out[n] = x * sinc * window[(size_t) n];
        sum += out[n];
    }

    const double norm = sum != 0.0 ? 1.0 / sum : 1.0;
    for (int n = 0; n < numTaps; ++n)
        out[n] *= norm;
}

void LinearPhaseCrossover::designInto (KernelSet& set) noexcept
{
    const int nb = std::clamp (requestedBands, 1, kMaxBands);
    set.numBands = nb;

    for (int b = 0; b < nb - 1; ++b)
    {
        designLowPass (requestedFreqs[(size_t) b], lpCur.data());

        for (int n = 0; n < numTaps; ++n)
            bandKernel[(size_t) n] = (b == 0) ? lpCur[(size_t) n] : lpCur[(size_t) n] - lpPrev[(size_t) n];

        for (int p = 0; p < numPartitions; ++p)
        {
            std::fill (fftWork.begin(), fftWork.end(), 0.0f);
            const int start = p * P;
            const int count = std::min (P, numTaps - start);

            for (int i = 0; i < count; ++i)
                fftWork[(size_t) i] = static_cast<float> (bandKernel[(size_t) (start + i)]);

            fft->performRealOnlyForwardTransform (fftWork.data(), true);
            std::copy (fftWork.begin(), fftWork.begin() + 2 * kBins, set.parts[(size_t) b][(size_t) p].begin());
        }

        std::swap (lpPrev, lpCur);
    }
}

//==============================================================================
void LinearPhaseCrossover::processBlock (const double* inL, const double* inR,
                                         double* const* bandL, double* const* bandR,
                                         int numSamples, int numBands) noexcept
{
    const int nb = std::clamp (numBands, 1, designedBands);
    const double* inputs[2] = { inL, inR };
    double* const* outputs[2] = { bandL, bandR };

    int done = 0;

    while (done < numSamples)
    {
        const int chunk = std::min (numSamples - done, P - fifoPos);

        for (int c = 0; c < 2; ++c)
        {
            auto& ch = channels[(size_t) c];
            const double* in = inputs[c] + done;

            for (int i = 0; i < chunk; ++i)
            {
                ch.inFifo[(size_t) (fifoPos + i)] = in[i];
                ch.delayFifo[(size_t) (fifoPos + i)] = ch.delay.processSample (in[i], halfLength);
            }

            for (int b = 0; b < nb; ++b)
                std::copy (ch.outFifo[(size_t) b].begin() + fifoPos,
                           ch.outFifo[(size_t) b].begin() + fifoPos + chunk,
                           outputs[c][b] + done);

            // Bands beyond the designed count (only if the caller asks for more) are silent.
            for (int b = nb; b < numBands && b < kMaxBands; ++b)
                std::fill (outputs[c][b] + done, outputs[c][b] + done + chunk, 0.0);
        }

        fifoPos += chunk;
        done += chunk;

        if (fifoPos == P)
        {
            processPartition();
            fifoPos = 0;
        }
    }
}

void LinearPhaseCrossover::convolveBand (int c, const KernelSet& set, int band, double* out) noexcept
{
    auto& ch = channels[(size_t) c];
    std::fill (accum.begin(), accum.end(), 0.0f);
    float* acc = accum.data();

    for (int p = 0; p < numPartitions; ++p)
    {
        const int idx = (fdlPos - p + numPartitions) % numPartitions;
        const float* X = ch.fdl[(size_t) idx].data();
        const float* H = set.parts[(size_t) band][(size_t) p].data();

        for (int k = 0; k < kBins; ++k)
        {
            const float xr = X[2 * k], xi = X[2 * k + 1];
            const float hr = H[2 * k], hi = H[2 * k + 1];
            acc[2 * k]     += xr * hr - xi * hi;
            acc[2 * k + 1] += xr * hi + xi * hr;
        }
    }

    std::fill (fftWork.begin(), fftWork.end(), 0.0f);
    std::copy (accum.begin(), accum.end(), fftWork.begin());
    fft->performRealOnlyInverseTransform (fftWork.data());

    // Overlap-save: the second half of the circular result is the valid linear convolution.
    for (int i = 0; i < P; ++i)
        out[i] = static_cast<double> (fftWork[(size_t) (P + i)]);
}

void LinearPhaseCrossover::processPartition() noexcept
{
    if (designPending)
    {
        designInto (kernelSets[(size_t) spareSet]);
        const int oldActive = activeSet;
        activeSet = spareSet;
        spareSet = previousSet;
        previousSet = oldActive;
        fading = kernelSets[(size_t) previousSet].numBands == kernelSets[(size_t) activeSet].numBands;
        designedBands = kernelSets[(size_t) activeSet].numBands;
        designPending = false;
    }

    const auto& active = kernelSets[(size_t) activeSet];
    const auto& previous = kernelSets[(size_t) previousSet];
    const int nb = active.numBands;

    for (int c = 0; c < 2; ++c)
    {
        auto& ch = channels[(size_t) c];

        // Slide the 2P input history and transform it into the FDL slot.
        std::copy (ch.history.begin() + P, ch.history.end(), ch.history.begin());
        for (int i = 0; i < P; ++i)
            ch.history[(size_t) (P + i)] = static_cast<float> (ch.inFifo[(size_t) i]);

        std::fill (fftWork.begin(), fftWork.end(), 0.0f);
        std::copy (ch.history.begin(), ch.history.end(), fftWork.begin());
        fft->performRealOnlyForwardTransform (fftWork.data(), true);
        std::copy (fftWork.begin(), fftWork.begin() + 2 * kBins, ch.fdl[(size_t) fdlPos].begin());

        auto& top = ch.outFifo[(size_t) (nb - 1)];
        std::copy (ch.delayFifo.begin(), ch.delayFifo.end(), top.begin());

        for (int b = 0; b < nb - 1; ++b)
        {
            auto& out = ch.outFifo[(size_t) b];
            convolveBand (c, active, b, tempNew.data());

            if (fading)
            {
                convolveBand (c, previous, b, tempOld.data());
                const double step = 1.0 / static_cast<double> (P);

                for (int i = 0; i < P; ++i)
                {
                    const double t = (i + 1) * step;
                    out[(size_t) i] = tempOld[(size_t) i] + (tempNew[(size_t) i] - tempOld[(size_t) i]) * t;
                }
            }
            else
            {
                std::copy (tempNew.begin(), tempNew.end(), out.begin());
            }

            for (int i = 0; i < P; ++i)
                top[(size_t) i] -= out[(size_t) i];
        }
    }

    fading = false;
    fdlPos = (fdlPos + 1) % numPartitions;
}

//==============================================================================
void LinearPhaseCrossover::sumBands (const double* const* bandL, const double* const* bandR,
                                     double* outL, double* outR, int numSamples, int numBands) noexcept
{
    numBands = std::clamp (numBands, 1, kMaxBands);

    for (int i = 0; i < numSamples; ++i)
    {
        double l = 0.0, r = 0.0;

        for (int b = 0; b < numBands; ++b)
        {
            l += bandL[b][i];
            r += bandR[b][i];
        }

        outL[i] = l;
        outR[i] = r;
    }
}

} // namespace meridian
