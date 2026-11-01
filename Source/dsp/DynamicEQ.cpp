#include "DynamicEQ.h"
#include "../common/FastMath.h"

#include <algorithm>
#include <cmath>

namespace meridian
{

//==============================================================================
// ResonanceSuppressor
//==============================================================================
ResonanceSuppressor::ResonanceSuppressor()
{
    // Hann window of length kFirLength + 2 with the zero end points dropped,
    // so the centre tap weight is exactly 1.
    for (int n = 0; n < kFirLength; ++n)
        firWindow[(size_t) n] = 0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * (n + 1) / (kFirLength + 1));
}

void ResonanceSuppressor::prepare (double sampleRate)
{
    fs = sampleRate;
    averageCoeff = 1.0 - std::exp (-(double) kHopSize / (kAverageTimeSec * fs));

    analysisRing.assign ((size_t) kFftSize, 0.0f);
    fftData.assign ((size_t) (2 * kFftSize), 0.0f);
    hannWindow.resize ((size_t) kFftSize);
    binAverage.assign ((size_t) kNumBins, 0.0);
    binGain.assign ((size_t) kNumBins, 1.0);

    for (int i = 0; i < kFftSize; ++i)
        hannWindow[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * i / kFftSize));

    reset();
}

void ResonanceSuppressor::reset() noexcept
{
    std::fill (analysisRing.begin(), analysisRing.end(), 0.0f);
    std::fill (binAverage.begin(), binAverage.end(), 0.0);
    std::fill (binGain.begin(), binGain.end(), 1.0);
    ringPos = 0;
    hopCounter = 0;

    gridGain.fill (1.0);
    taps.fill (0.0);
    taps[(size_t) kFirHalf] = 1.0;
    targetTaps = taps;
    tapStep.fill (0.0);
    tapRampLeft = 0;

    for (auto& h : history)
        h.fill (0.0);

    historyPos = 0;
    currentMaxAttenuationDb = 0.0;
}

void ResonanceSuppressor::analyseFrame() noexcept
{
    constexpr int mask = kFftSize - 1;
    constexpr double magnitudeFloor = 1.0e-4;   // ~ -140 dB relative to a full-scale sine peak bin

    for (int i = 0; i < kFftSize; ++i)
        fftData[(size_t) i] = analysisRing[(size_t) ((ringPos + i) & mask)] * hannWindow[(size_t) i];

    std::fill (fftData.begin() + kFftSize, fftData.end(), 0.0f);
    fft.performFrequencyOnlyForwardTransform (fftData.data(), true);

    for (int k = 0; k < kNumBins; ++k)
    {
        const double mag = (double) fftData[(size_t) k];
        double& avg = binAverage[(size_t) k];
        double gainDb = 0.0;

        if (avg < magnitudeFloor)
        {
            avg = mag;   // (re)initialise silent bins so onsets are not treated as resonances
        }
        else
        {
            if (enabled && mag > magnitudeFloor)
            {
                const double ratioDb = 20.0 * std::log10 (mag / avg);
                const double t = std::clamp ((ratioDb - kRatioThresholdDb) / kRatioRangeDb, 0.0, 1.0);
                gainDb = kMaxAttenuationDb * t;
            }

            avg += averageCoeff * (mag - avg);
        }

        binGain[(size_t) k] = gainDb < 0.0 ? std::pow (10.0, gainDb * 0.05) : 1.0;
    }
}

void ResonanceSuppressor::designTargetFir() noexcept
{
    const double binsPerGridPoint = (double) kFftSize / (double) kFirLength;
    const int halfWidth = (int) std::floor (binsPerGridPoint * 0.5);
    double minGain = 1.0;

    for (int j = 0; j < kNumGridPoints; ++j)
    {
        const int centre = (int) std::lround (j * binsPerGridPoint);
        const int lo = std::max (0, centre - halfWidth);
        const int hi = std::min (kNumBins - 1, centre + halfWidth);

        double sum = 0.0;
        for (int k = lo; k <= hi; ++k)
            sum += binGain[(size_t) k];

        const double g = enabled ? sum / (double) (hi - lo + 1) : 1.0;
        gridGain[(size_t) j] += 0.5 * (g - gridGain[(size_t) j]);

        if (std::abs (gridGain[(size_t) j] - 1.0) < 1.0e-9)
            gridGain[(size_t) j] = 1.0;

        minGain = std::min (minGain, gridGain[(size_t) j]);
    }

    // Frequency sampling, type I linear phase: real, even-symmetric response.
    const double twoPiOverN = 2.0 * juce::MathConstants<double>::pi / (double) kFirLength;

    for (int n = 0; n < kFirLength; ++n)
    {
        double acc = gridGain[0];
        for (int j = 1; j < kNumGridPoints; ++j)
            acc += 2.0 * gridGain[(size_t) j] * std::cos (twoPiOverN * j * (n - kFirHalf));

        targetTaps[(size_t) n] = firWindow[(size_t) n] * acc / (double) kFirLength;
    }

    for (int n = 0; n < kFirLength; ++n)
        tapStep[(size_t) n] = (targetTaps[(size_t) n] - taps[(size_t) n]) / (double) kHopSize;

    tapRampLeft = kHopSize;
    currentMaxAttenuationDb = -20.0 * std::log10 (std::max (minGain, 1.0e-6));
}

void ResonanceSuppressor::process (double* L, double* R, int numSamples) noexcept
{
    constexpr int mask = kFftSize - 1;
    double* io[2] = { L, R };

    for (int i = 0; i < numSamples; ++i)
    {
        analysisRing[(size_t) ringPos] = (float) (0.5 * (L[i] + R[i]));
        ringPos = (ringPos + 1) & mask;

        if (++hopCounter >= kHopSize)
        {
            hopCounter = 0;
            analyseFrame();
            designTargetFir();
        }

        if (tapRampLeft > 0)
        {
            if (--tapRampLeft == 0)
                taps = targetTaps;
            else
                for (int k = 0; k < kFirLength; ++k)
                    taps[(size_t) k] += tapStep[(size_t) k];
        }

        historyPos = historyPos == 0 ? kFirLength - 1 : historyPos - 1;

        for (int c = 0; c < 2; ++c)
        {
            auto& h = history[(size_t) c];
            const double x = io[c][i];
            h[(size_t) historyPos] = x;
            h[(size_t) (historyPos + kFirLength)] = x;

            const double* xp = h.data() + historyPos;
            double y = 0.0;
            for (int k = 0; k < kFirLength; ++k)
                y += taps[(size_t) k] * xp[k];

            io[c][i] = y;
        }
    }
}

//==============================================================================
// DynamicEQ
//==============================================================================
DynamicEQ::DynamicEQ (juce::AudioProcessorValueTreeState& state)
    : DSPModuleBase (state)
{
    namespace f = pid::field;

    for (int i = 0; i < kNumBands; ++i)
    {
        auto& b = bands[(size_t) i];
        b.freqParam      = rawParam (pid::bandParam (pid::deqBandPrefix, i, f::freq));
        b.qParam         = rawParam (pid::bandParam (pid::deqBandPrefix, i, f::q));
        b.thresholdParam = rawParam (pid::bandParam (pid::deqBandPrefix, i, f::threshold));
        b.maxGainParam   = rawParam (pid::bandParam (pid::deqBandPrefix, i, f::maxGain));
        b.attackParam    = rawParam (pid::bandParam (pid::deqBandPrefix, i, f::attack));
        b.releaseParam   = rawParam (pid::bandParam (pid::deqBandPrefix, i, f::release));
        b.channelParam   = rawParam (pid::bandParam (pid::deqBandPrefix, i, f::channel));
        b.enabledParam   = rawParam (pid::bandParam (pid::deqBandPrefix, i, f::enabled));
    }

    resonEnabledParam = rawParam (pid::deqResonEnabled);
}

double DynamicEQ::msToCoeff (double ms, double fs) noexcept
{
    const double samples = std::max (ms, 0.01) * 0.001 * fs;
    return 1.0 - std::exp (-1.0 / samples);
}

void DynamicEQ::prepareModule (double sampleRate, int /*maxBlockSize*/)
{
    for (auto& b : bands)
    {
        b.logFreq.prepare (sampleRate, 20.0);
        b.q.prepare (sampleRate, 20.0);

        const double freq = Biquad::clampFrequency ((double) load (b.freqParam), sampleRate);
        const double q = std::max (0.05, (double) load (b.qParam));
        b.logFreq.reset (std::log (freq));
        b.q.reset (q);

        b.thresholdDb = (double) load (b.thresholdParam);
        b.maxGainDb = (double) load (b.maxGainParam);
        b.attackCoeff = msToCoeff ((double) load (b.attackParam), sampleRate);
        b.releaseCoeff = msToCoeff ((double) load (b.releaseParam), sampleRate);
        b.channel = juce::jlimit (0, 2, loadInt (b.channelParam));
        b.enabled = loadBool (b.enabledParam);

        const auto bp = Biquad::makeBPF (freq, q, sampleRate);
        b.sidechain[0].setCoefficients (bp);
        b.sidechain[1].setCoefficients (bp);
        b.current = b.target = Biquad::makeBell (freq, q, 0.0, sampleRate);
    }

    suppressor.prepare (sampleRate);
    suppressor.setEnabled (loadBool (resonEnabledParam));
    setLatencySamples (suppressor.getLatencySamples());
}

void DynamicEQ::resetModule() noexcept
{
    for (size_t i = 0; i < bands.size(); ++i)
    {
        auto& b = bands[i];
        b.logFreq.reset (b.logFreq.getTargetValue());
        b.q.reset (b.q.getTargetValue());
        b.sidechain[0].reset();
        b.sidechain[1].reset();
        b.meanSquare = 0.0;
        b.appliedGainDb = 0.0;
        b.rampLeft = 0;
        b.current = b.target = Biquad::makeBell (Biquad::clampFrequency (std::exp (b.logFreq.getCurrentValue()), currentSampleRate),
                                                 b.q.getCurrentValue(), 0.0, currentSampleRate);
        b.z[0][0] = b.z[0][1] = b.z[1][0] = b.z[1][1] = 0.0;
        b.active = b.enabled;
        bandGainDb[i].store (0.0, std::memory_order_relaxed);
    }

    suppressor.reset();
    controlCounter = 0;
}

void DynamicEQ::updateParameters() noexcept
{
    double maxCutDb = 0.0;

    for (size_t i = 0; i < bands.size(); ++i)
    {
        auto& b = bands[i];
        b.logFreq.setTargetValue (std::log (Biquad::clampFrequency ((double) load (b.freqParam), currentSampleRate)));
        b.q.setTargetValue (std::max (0.05, (double) load (b.qParam)));
        b.thresholdDb = (double) load (b.thresholdParam);
        b.maxGainDb = (double) load (b.maxGainParam);
        b.attackCoeff = msToCoeff ((double) load (b.attackParam), currentSampleRate);
        b.releaseCoeff = msToCoeff ((double) load (b.releaseParam), currentSampleRate);
        b.enabled = loadBool (b.enabledParam);

        const int newChannel = juce::jlimit (0, 2, loadInt (b.channelParam));
        if (newChannel != b.channel)
        {
            // Different signal path: restart detector and filter state.
            b.channel = newChannel;
            b.sidechain[0].reset();
            b.sidechain[1].reset();
            b.meanSquare = 0.0;
            b.z[0][0] = b.z[0][1] = b.z[1][0] = b.z[1][1] = 0.0;
        }

        maxCutDb = std::max (maxCutDb, -bandGainDb[i].load (std::memory_order_relaxed));
    }

    suppressor.setEnabled (loadBool (resonEnabledParam));
    setGainReductionDb ((float) std::max (maxCutDb, suppressor.getMaxAttenuationDb()));
}

double DynamicEQ::computeGainDb (const Band& b) const noexcept
{
    const double envDb = fastmath::powerToDb (b.meanSquare + 1.0e-30);
    const double over = envDb - b.thresholdDb;

    if (over <= 0.0)
        return 0.0;

    return b.maxGainDb * std::min (over / kGainRangeDb, 1.0);
}

void DynamicEQ::controlUpdate (Band& b) noexcept
{
    const double freq = Biquad::clampFrequency (std::exp (b.logFreq.skip (kControlInterval)), currentSampleRate);
    const double q = b.q.skip (kControlInterval);

    if (! b.enabled && b.active && b.appliedGainDb == 0.0 && b.rampLeft == 0)
    {
        // Fully faded out: skip the band from now on.
        b.active = false;
        b.z[0][0] = b.z[0][1] = b.z[1][0] = b.z[1][1] = 0.0;
        b.sidechain[0].reset();
        b.sidechain[1].reset();
        b.meanSquare = 0.0;
        return;
    }

    if (! b.active)
    {
        if (! b.enabled)
            return;

        b.active = true;
        b.current = b.target = Biquad::makeBell (freq, q, 0.0, currentSampleRate);
    }

    const auto bp = Biquad::makeBPF (freq, q, currentSampleRate);
    b.sidechain[0].setCoefficients (bp);
    b.sidechain[1].setCoefficients (bp);

    double g = b.enabled ? computeGainDb (b) : 0.0;
    g = std::clamp (g, b.appliedGainDb - kMaxGainStepDb, b.appliedGainDb + kMaxGainStepDb);
    if (std::abs (g) < 1.0e-6)
        g = 0.0;
    b.appliedGainDb = g;

    b.target = Biquad::makeBell (freq, q, g, currentSampleRate);

    constexpr double inv = 1.0 / (double) kControlInterval;
    b.step.b0 = (b.target.b0 - b.current.b0) * inv;
    b.step.b1 = (b.target.b1 - b.current.b1) * inv;
    b.step.b2 = (b.target.b2 - b.current.b2) * inv;
    b.step.a1 = (b.target.a1 - b.current.a1) * inv;
    b.step.a2 = (b.target.a2 - b.current.a2) * inv;
    b.rampLeft = kControlInterval;
}

void DynamicEQ::processBlock (double* L, double* R, int numSamples) noexcept
{
    int i = 0;

    while (i < numSamples)
    {
        if (controlCounter <= 0)
        {
            for (size_t k = 0; k < bands.size(); ++k)
            {
                controlUpdate (bands[k]);
                bandGainDb[k].store (bands[k].appliedGainDb, std::memory_order_relaxed);
            }

            controlCounter = kControlInterval;
        }

        const int chunk = std::min (numSamples - i, controlCounter);

        for (int n = i; n < i + chunk; ++n)
        {
            double l = L[n], r = R[n];

            for (auto& b : bands)
            {
                if (! b.active)
                    continue;

                if (b.rampLeft > 0)
                {
                    if (--b.rampLeft == 0)
                    {
                        b.current = b.target;
                    }
                    else
                    {
                        b.current.b0 += b.step.b0;
                        b.current.b1 += b.step.b1;
                        b.current.b2 += b.step.b2;
                        b.current.a1 += b.step.a1;
                        b.current.a2 += b.step.a2;
                    }
                }

                const BiquadCoeffs& c = b.current;
                auto filter = [&c] (double x, double* z) noexcept
                {
                    const double y = c.b0 * x + z[0];
                    z[0] = c.b1 * x - c.a1 * y + z[1];
                    z[1] = c.b2 * x - c.a2 * y;
                    return y;
                };

                double power;

                if (b.channel == BandStereo)
                {
                    const double sl = b.sidechain[0].process (l);
                    const double sr = b.sidechain[1].process (r);
                    power = 0.5 * (sl * sl + sr * sr);
                    l = filter (l, b.z[0]);
                    r = filter (r, b.z[1]);
                }
                else
                {
                    double m = 0.5 * (l + r);
                    double s = 0.5 * (l - r);
                    double& target = b.channel == BandMid ? m : s;
                    const double sc = b.sidechain[0].process (target);
                    power = sc * sc;
                    target = filter (target, b.z[0]);
                    l = m + s;
                    r = m - s;
                }

                b.meanSquare += (power > b.meanSquare ? b.attackCoeff : b.releaseCoeff) * (power - b.meanSquare);
            }

            L[n] = l;
            R[n] = r;
        }

        controlCounter -= chunk;
        i += chunk;
    }

    for (auto& b : bands)
    {
        for (auto& zz : b.z)
            for (auto& v : zz)
                if (std::abs (v) < 1.0e-30) v = 0.0;

        b.sidechain[0].snapToZero();
        b.sidechain[1].snapToZero();
        if (b.meanSquare < 1.0e-30) b.meanSquare = 0.0;
    }

    suppressor.process (L, R, numSamples);
}

} // namespace meridian
