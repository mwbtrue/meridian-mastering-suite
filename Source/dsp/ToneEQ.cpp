#include "ToneEQ.h"

#include <algorithm>
#include <cmath>

namespace meridian
{

namespace
{
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kIdentityGainDb = 1.0e-6;
    constexpr double kColourC2 = 0.04;
    constexpr double kColourC3 = 0.02;

    template <size_t N>
    double choiceValue (const std::array<double, N>& table, int index) noexcept
    {
        return table[(size_t) std::clamp (index, 0, (int) N - 1)];
    }

    /** Bilinear transform of the 1st-order analog section (B1 s + B0) / (A1 s + A0),
        normalised so that s = j1 maps to freq. */
    BiquadCoeffs bilinearFirstOrder (double B1, double B0, double A1, double A0, double freq, double fs) noexcept
    {
        const double c = 1.0 / std::tan (kPi * Biquad::clampFrequency (freq, fs) / fs);
        const double a0 = A1 * c + A0;
        BiquadCoeffs r;
        r.b0 = (B1 * c + B0) / a0;
        r.b1 = (B0 - B1 * c) / a0;
        r.b2 = 0.0;
        r.a1 = (A0 - A1 * c) / a0;
        r.a2 = 0.0;
        return r;
    }
}

//==============================================================================
ToneEQ::ToneEQ (juce::AudioProcessorValueTreeState& state)
    : DSPModuleBase (state)
{
    bassFreqParam      = rawParam (pid::teqPultecBassFreq);
    bassBoostParam     = rawParam (pid::teqPultecBassBoost);
    bassAttenParam     = rawParam (pid::teqPultecBassAtten);
    attenFreqParam     = rawParam (pid::teqPultecAttenFreq);
    attenBwParam       = rawParam (pid::teqPultecAttenBw);
    midBoostParam      = rawParam (pid::teqPultecMidBoost);
    midFreqParam       = rawParam (pid::teqPultecMidFreq);
    midBwParam         = rawParam (pid::teqPultecMidBw);
    trebleBoostParam   = rawParam (pid::teqPultecTrebleBoost);
    trebleFreqParam    = rawParam (pid::teqPultecTrebleFreq);
    trebleBwParam      = rawParam (pid::teqPultecTrebleBw);
    baxBassParam       = rawParam (pid::teqBaxandallBass);
    baxBassFreqParam   = rawParam (pid::teqBaxandallBassFreq);
    baxTrebleParam     = rawParam (pid::teqBaxandallTreble);
    baxTrebleFreqParam = rawParam (pid::teqBaxandallTrebleFreq);
    passiveGainParam   = rawParam (pid::teqPassiveMidGain);
    passiveFreqParam   = rawParam (pid::teqPassiveMidFreq);
    passiveQParam      = rawParam (pid::teqPassiveMidQ);
    airGainParam       = rawParam (pid::teqSideAirGain);
    airFreqParam       = rawParam (pid::teqSideAirFreq);
    colourParam        = rawParam (pid::teqColour);

    sections[PultecLow].kind       = Kind::LowShelf;
    sections[PultecAtten].kind     = Kind::Bell;
    sections[PultecMid].kind       = Kind::Bell;
    sections[PultecTreble].kind    = Kind::HighShelf;
    sections[BaxandallBass].kind   = Kind::FirstOrderLowShelf;
    sections[BaxandallTreble].kind = Kind::FirstOrderHighShelf;
    sections[PassiveMid].kind      = Kind::Bell;
    sections[SideAir].kind         = Kind::HighShelf;
    sections[SideAir].stereo       = false;
}

double ToneEQ::qFromBandwidth (double octaves) noexcept
{
    const double p = std::pow (2.0, std::clamp (octaves, 0.1, 4.0));
    return std::sqrt (p) / (p - 1.0);
}

BiquadCoeffs ToneEQ::designSection (Kind kind, double freq, double gainDb, double q, double fs) noexcept
{
    if (std::abs (gainDb) < kIdentityGainDb)
        return BiquadCoeffs::identity();

    freq = Biquad::clampFrequency (freq, fs);
    q = std::max (q, 0.05);
    const double A = std::pow (10.0, gainDb / 40.0);   // sqrt of linear gain
    const double sqrtA = std::sqrt (A);

    switch (kind)
    {
        case Kind::LowShelf:
            // H(s) = A (s^2 + sqrtA/Q s + A) / (A s^2 + sqrtA/Q s + 1)
            return Biquad::bilinear (A, A * sqrtA / q, A * A, A, sqrtA / q, 1.0, freq, fs);

        case Kind::HighShelf:
            // H(s) = A (A s^2 + sqrtA/Q s + 1) / (s^2 + sqrtA/Q s + A)
            return Biquad::bilinear (A * A, A * sqrtA / q, A, 1.0, sqrtA / q, A, freq, fs);

        case Kind::Bell:
            // H(s) = (s^2 + A/Q s + 1) / (s^2 + 1/(A Q) s + 1)
            return Biquad::bilinear (1.0, A / q, 1.0, 1.0, 1.0 / (A * q), 1.0, freq, fs);

        case Kind::FirstOrderLowShelf:
        {
            // H(s) = (s + sqrt G) / (s + 1/sqrt G): DC gain G, HF gain 1. sqrt G = A.
            return bilinearFirstOrder (1.0, A, 1.0, 1.0 / A, freq, fs);
        }

        case Kind::FirstOrderHighShelf:
        default:
            // H(s) = (sqrt G s + 1) / (s / sqrt G + 1): DC gain 1, HF gain G.
            return bilinearFirstOrder (A, 1.0, 1.0 / A, 1.0, freq, fs);
    }
}

//==============================================================================
void ToneEQ::readTargets (double* gains, double* freqs, double* qs) const noexcept
{
    gains[PultecLow] = (double) load (bassBoostParam);
    freqs[PultecLow] = choiceValue (choices::pultecBassFreqs, loadInt (bassFreqParam));
    qs[PultecLow]    = kPultecLowQ;

    gains[PultecAtten] = -(double) load (bassAttenParam);
    freqs[PultecAtten] = choiceValue (choices::pultecAttenFreqs, loadInt (attenFreqParam));
    qs[PultecAtten]    = qFromBandwidth ((double) load (attenBwParam));

    gains[PultecMid] = (double) load (midBoostParam);
    freqs[PultecMid] = choiceValue (choices::pultecMidFreqs, loadInt (midFreqParam));
    qs[PultecMid]    = qFromBandwidth ((double) load (midBwParam));

    gains[PultecTreble] = (double) load (trebleBoostParam);
    freqs[PultecTreble] = choiceValue (choices::pultecTrebleFreqs, loadInt (trebleFreqParam));
    qs[PultecTreble]    = 0.5 + 0.45 / std::max (0.1, (double) load (trebleBwParam));

    gains[BaxandallBass] = (double) load (baxBassParam);
    freqs[BaxandallBass] = (double) load (baxBassFreqParam);
    qs[BaxandallBass]    = 0.70710678118654752;

    gains[BaxandallTreble] = (double) load (baxTrebleParam);
    freqs[BaxandallTreble] = (double) load (baxTrebleFreqParam);
    qs[BaxandallTreble]    = 0.70710678118654752;

    gains[PassiveMid] = (double) load (passiveGainParam);
    freqs[PassiveMid] = (double) load (passiveFreqParam);
    qs[PassiveMid]    = std::clamp ((double) load (passiveQParam), 0.3, 1.5);

    gains[SideAir] = (double) load (airGainParam);
    freqs[SideAir] = (double) load (airFreqParam);
    qs[SideAir]    = kSideAirQ;
}

void ToneEQ::prepareModule (double sampleRate, int /*maxBlockSize*/)
{
    double g[NumSections], f[NumSections], q[NumSections];
    readTargets (g, f, q);

    for (int i = 0; i < NumSections; ++i)
    {
        auto& s = sections[(size_t) i];
        s.gainDb.prepare (sampleRate, kSmoothingMs);
        s.logFreq.prepare (sampleRate, kSmoothingMs);
        s.logQ.prepare (sampleRate, kSmoothingMs);
        s.gainDb.reset (g[i]);
        s.logFreq.reset (std::log (Biquad::clampFrequency (f[i], sampleRate)));
        s.logQ.reset (std::log (q[i]));
        redesign (s);
    }

    colour.prepare (sampleRate, 20.0);
    colour.reset (std::clamp ((double) load (colourParam) * 0.01, 0.0, 1.0));
    dcCoeff = std::exp (-2.0 * kPi * 5.0 / sampleRate);

    setLatencySamples (0);
    setGainReductionDb (0.0f);
}

void ToneEQ::resetModule() noexcept
{
    for (auto& s : sections)
    {
        s.gainDb.reset (s.gainDb.getTargetValue());
        s.logFreq.reset (s.logFreq.getTargetValue());
        s.logQ.reset (s.logQ.getTargetValue());
        redesign (s);
        s.filter[0].reset();
        s.filter[1].reset();
    }

    colour.reset (colour.getTargetValue());
    dcX[0] = dcX[1] = dcY[0] = dcY[1] = 0.0;
    controlCounter = 0;
}

void ToneEQ::redesign (Section& s) noexcept
{
    const auto c = designSection (s.kind, std::exp (s.logFreq.getCurrentValue()), s.gainDb.getCurrentValue(),
                                  std::exp (s.logQ.getCurrentValue()), currentSampleRate);
    s.identity = c == BiquadCoeffs::identity();
    s.filter[0].setCoefficients (c);
    s.filter[1].setCoefficients (c);
}

bool ToneEQ::anySmoothing() const noexcept
{
    for (const auto& s : sections)
        if (s.gainDb.isSmoothing() || s.logFreq.isSmoothing() || s.logQ.isSmoothing())
            return true;

    return false;
}

void ToneEQ::updateParameters() noexcept
{
    double g[NumSections], f[NumSections], q[NumSections];
    readTargets (g, f, q);

    for (int i = 0; i < NumSections; ++i)
    {
        auto& s = sections[(size_t) i];
        s.gainDb.setTargetValue (g[i]);
        s.logFreq.setTargetValue (std::log (Biquad::clampFrequency (f[i], currentSampleRate)));
        s.logQ.setTargetValue (std::log (q[i]));
    }

    colour.setTargetValue (std::clamp ((double) load (colourParam) * 0.01, 0.0, 1.0));
}

double ToneEQ::getStereoMagnitudeDb (double freq) const noexcept
{
    double db = 0.0;

    for (int i = 0; i < NumSections; ++i)
    {
        const auto& s = sections[(size_t) i];
        if (! s.stereo)
            continue;

        const auto c = designSection (s.kind, std::exp (s.logFreq.getTargetValue()), s.gainDb.getTargetValue(),
                                      std::exp (s.logQ.getTargetValue()), currentSampleRate);
        db += 20.0 * std::log10 (std::max (1.0e-12, c.magnitude (freq, currentSampleRate)));
    }

    return db;
}

void ToneEQ::processBlock (double* L, double* R, int numSamples) noexcept
{
    int offset = 0;

    while (offset < numSamples)
    {
        // Control-rate coefficient updates while any parameter glides.
        int n = numSamples - offset;
        const bool smoothing = anySmoothing();

        if (smoothing)
        {
            n = std::min (n, kControlInterval);

            for (auto& s : sections)
            {
                s.gainDb.skip (n);
                s.logFreq.skip (n);
                s.logQ.skip (n);
                redesign (s);
            }
        }

        double* l = L + offset;
        double* r = R + offset;

        for (int i = 0; i < SideAir; ++i)
        {
            auto& s = sections[(size_t) i];
            if (s.identity)
                continue;

            s.filter[0].processBlock (l, n);
            s.filter[1].processBlock (r, n);
        }

        auto& air = sections[SideAir];
        if (! air.identity)
        {
            for (int k = 0; k < n; ++k)
            {
                const double m = 0.5 * (l[k] + r[k]);
                const double sd = air.filter[0].process (0.5 * (l[k] - r[k]));
                l[k] = m + sd;
                r[k] = m - sd;
            }

            air.filter[0].snapToZero();
        }

        if (colour.isSmoothing() || colour.getCurrentValue() > 0.0)
        {
            for (int k = 0; k < n; ++k)
            {
                const double a = colour.processSample();
                double* ch[2] = { l + k, r + k };

                for (int c = 0; c < 2; ++c)
                {
                    const double x = std::clamp (*ch[c], -2.0, 2.0);
                    const double d = a * (kColourC2 * x * x - kColourC3 * x * x * x);
                    const double y = d - dcX[c] + dcCoeff * dcY[c];   // 5 Hz DC blocker on the added harmonics
                    dcX[c] = d;
                    dcY[c] = std::abs (y) < 1.0e-30 ? 0.0 : y;
                    *ch[c] += y;
                }
            }
        }

        offset += n;
    }
}

} // namespace meridian
