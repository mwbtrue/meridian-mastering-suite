#include "Saturation.h"
#include "../common/FastMath.h"

#include <algorithm>
#include <cmath>

namespace meridian
{

namespace
{
    constexpr double kDcCutoffHz = 5.0;
    constexpr double kLfCoreHz = 40.0;
    constexpr double kAutoGainMaxDb = 12.0;
    constexpr double kAutoGainFloor = 1.0e-8;   // mean square, -80 dBFS
    constexpr int kAutoGainInterval = 32;

    inline double sgn (double v) noexcept { return v > 0.0 ? 1.0 : (v < 0.0 ? -1.0 : 0.0); }
}

void Saturation::ModelState::reset() noexcept
{
    mIrr = mTotal = prevH = 0.0;
    hInit = false;
    lfFilter.reset();
    dcX = dcY = 0.0;
}

Saturation::Saturation (juce::AudioProcessorValueTreeState& state) : DSPModuleBase (state)
{
    modelParam    = rawParam (pid::satModel);
    driveParam    = rawParam (pid::satDrive);
    biasParam     = rawParam (pid::satBias);
    mixParam      = rawParam (pid::satMix);
    osParam       = rawParam (pid::satOversampling);
    mbParam       = rawParam (pid::satMultibandEnabled);
    autoGainParam = rawParam (pid::satAutoGain);

    for (int i = 0; i < kNumSatXovers; ++i)
        xoverParams[(size_t) i] = rawParam (pid::bandParam (pid::satBandPrefix, i, pid::field::freq));

    for (int i = 0; i < kNumSatBands; ++i)
        bandDriveParams[(size_t) i] = rawParam (pid::bandParam (pid::satBandPrefix, i, pid::field::drive));
}

void Saturation::prepareModule (double sampleRate, int maxBlockSize)
{
    osBuf.assign ((size_t) maxBlockSize * kMaxFactor, 0.0);
    dryL.assign ((size_t) maxBlockSize, 0.0);
    dryR.assign ((size_t) maxBlockSize, 0.0);

    for (auto& band : bandBuf)
        for (auto& ch : band)
            ch.assign ((size_t) maxBlockSize, 0.0);

    for (auto& d : dryDelay)
        d.prepare (OversamplingEngine::kMaxLatencySamples + 8);

    splitter.prepare (sampleRate);
    mix.prepare (sampleRate, 20.0);
    makeup.prepare (sampleRate, 100.0);
    rmsCoeff = 1.0 - std::exp (-1.0 / (0.1 * sampleRate));

    factor = 0;   // force configuration
    configureOversampling (1);
}

void Saturation::configureOversampling (int newFactor) noexcept
{
    newFactor = std::clamp (newFactor, 1, kMaxFactor);

    if (newFactor == factor)
        return;

    factor = newFactor;
    engine.setOversamplingFactor (factor);
    engine.reset();
    osRate = currentSampleRate * factor;
    dcCoeffOs = std::exp (-2.0 * juce::MathConstants<double>::pi * kDcCutoffHz / osRate);

    const auto lf = Biquad::makeLPF (kLfCoreHz, 0.70710678118654752, osRate);

    for (auto& band : states)
        for (auto& s : band)
        {
            s.reset();
            s.lfFilter.setCoefficients (lf);
        }

    setLatencySamples (engine.getLatencySamples());
}

void Saturation::resetModule() noexcept
{
    engine.reset();
    splitter.reset();

    for (auto& band : states)
        for (auto& s : band)
            s.reset();

    for (auto& d : dryDelay)
        d.reset();

    mix.reset (mix.getTargetValue());
    makeup.reset (1.0);
    msDry = msWet = 0.0;
    autoGainDb.store (0.0f, std::memory_order_relaxed);
}

void Saturation::updateParameters() noexcept
{
    model = std::clamp (loadInt (modelParam), 0, 2);
    driveDb = std::clamp ((double) load (driveParam), 0.0, 24.0);
    bias = std::clamp ((double) load (biasParam), -1.0, 1.0);
    autoGainOn = loadBool (autoGainParam);
    mix.setTargetValue (std::clamp ((double) load (mixParam) * 0.01, 0.0, 1.0));

    const bool mb = loadBool (mbParam);
    if (mb != multiband)
    {
        multiband = mb;
        engine.reset();
        splitter.reset();
        for (auto& band : states)
            for (auto& s : band)
                s.reset();
    }

    for (int i = 0; i < kNumSatBands; ++i)
        bandDriveDb[(size_t) i] = std::clamp ((double) load (bandDriveParams[(size_t) i]), 0.0, 24.0);

    double f0 = std::clamp ((double) load (xoverParams[0]), 20.0, 0.4 * currentSampleRate);
    double f1 = std::clamp ((double) load (xoverParams[1]), 20.0, 0.4 * currentSampleRate);
    f1 = std::max (f1, f0 * 1.12246204830937);   // keep >= 1/6 octave apart
    xovers = { f0, std::min (f1, 0.45 * currentSampleRate) };
    splitter.setCrossovers (xovers.data(), kNumSatBands);

    const int idx = std::clamp (loadInt (osParam), 0, (int) choices::satOversampling.size() - 1);
    configureOversampling (choices::satOversampling[(size_t) idx]);
}

// ---------------------------------------------------------------------------
// Models (input x at the oversampled rate, unity small-signal gain)
// ---------------------------------------------------------------------------
double Saturation::processTape (ModelState& s, double x, double g, double biasValue) const noexcept
{
    const double h1 = g * x + 0.5 * biasValue;

    if (! s.hInit)
    {
        // Start on the anhysteretic curve at the first field value (no transient).
        s.prevH = h1;
        s.mIrr = (1.0 - kTapeC) * fastmath::tanh (h1);
        s.mTotal = s.mIrr + kTapeC * fastmath::tanh (h1);
        s.hInit = true;
    }

    const double dH = h1 - s.prevH;
    constexpr double sigma = (1.0 - kTapeC) / (2.0 * kTapeK);
    constexpr double ms = 1.0 - kTapeC;

    if (dH != 0.0)
    {
        const double delta = sgn (dH);
        const int steps = std::clamp ((int) std::ceil (std::abs (dH) / 0.25), 1, 64);
        const double h = dH / steps;

        // dM/dH = sigma * (1 - delta * M / Ms)  (direction fixed within one sample)
        auto f = [delta] (double m) noexcept { return sigma * (1.0 - delta * m / ms); };

        double m = s.mIrr;
        for (int i = 0; i < steps; ++i)
        {
            const double k1 = f (m);
            const double k2 = f (m + 0.5 * h * k1);
            const double k3 = f (m + 0.5 * h * k2);
            const double k4 = f (m + h * k3);
            m += h * (k1 + 2.0 * k2 + 2.0 * k3 + k4) / 6.0;
        }

        s.mIrr = std::clamp (m, -ms, ms);
    }

    s.prevH = h1;
    const double mRev = kTapeC * fastmath::tanh (h1 + kTapeAlpha * s.mTotal);
    s.mTotal = s.mIrr + mRev;
    return s.mTotal / g;
}

double Saturation::processTube (double x, double g, double biasValue) const noexcept
{
    const double u = g * x;
    const double beta = std::clamp (0.15 + 0.35 * biasValue, -0.5, 0.5);
    const double t = fastmath::tanh (u);
    return fastmath::tanh (u + beta * t * t) / g;
}

double Saturation::processTransformer (ModelState& s, double x, double g, double driveDbValue) const noexcept
{
    const double eta = driveDbValue / 24.0;
    const double lf = s.lfFilter.process (x);
    const double u = g * (x + eta * lf);

    double core;
    if (u >= 1.0)        core = 2.0 / 3.0;
    else if (u <= -1.0)  core = -2.0 / 3.0;
    else                 core = u - u * u * u / 3.0;

    const double y = (core - g * eta * lf) / g;
    return std::clamp (y, -1.2, 1.2);
}

double Saturation::dcBlock (ModelState& s, double x) const noexcept
{
    const double y = x - s.dcX + dcCoeffOs * s.dcY;
    s.dcX = x;
    s.dcY = y;
    return y;
}

void Saturation::runBand (int band, int ch, double* data, int n, double stageDriveDb) noexcept
{
    const int engineCh = band * 2 + ch;
    const int nOs = engine.upsample (data, osBuf.data(), n, engineCh);
    auto& s = states[(size_t) band][(size_t) ch];
    const double g = dbToGain (stageDriveDb);

    switch (model)
    {
        case (int) choices::SatModel::Tape:
            for (int i = 0; i < nOs; ++i)
                osBuf[(size_t) i] = dcBlock (s, processTape (s, osBuf[(size_t) i], g, bias));
            break;

        case (int) choices::SatModel::Tube:
            for (int i = 0; i < nOs; ++i)
                osBuf[(size_t) i] = dcBlock (s, processTube (osBuf[(size_t) i], g, bias));
            break;

        case (int) choices::SatModel::Transformer:
        default:
            for (int i = 0; i < nOs; ++i)
                osBuf[(size_t) i] = processTransformer (s, osBuf[(size_t) i], g, stageDriveDb);
            break;
    }

    engine.downsample (osBuf.data(), data, nOs, engineCh);

    s.lfFilter.snapToZero();
    if (std::abs (s.dcY) < 1.0e-30) s.dcY = 0.0;
}

// ---------------------------------------------------------------------------
void Saturation::processBlock (double* L, double* R, int n) noexcept
{
    const int latency = engine.getLatencySamples();
    double* io[2] { L, R };
    double* dry[2] { dryL.data(), dryR.data() };

    if (multiband)
    {
        double bands[LinkwitzRileyBank::kMaxBands];

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < n; ++i)
            {
                splitter.processSample (ch, io[ch][i], bands);
                for (int b = 0; b < kNumSatBands; ++b)
                    bandBuf[(size_t) b][(size_t) ch][(size_t) i] = bands[b];
            }

        splitter.snapToZero();

        // Dry = all-pass band sum (phase-matched to the wet sum), latency-aligned.
        for (int ch = 0; ch < 2; ++ch)
        {
            for (int i = 0; i < n; ++i)
            {
                double sum = 0.0;
                for (int b = 0; b < kNumSatBands; ++b)
                    sum += bandBuf[(size_t) b][(size_t) ch][(size_t) i];
                dry[ch][i] = sum;
            }
            dryDelay[ch].process (dry[ch], dry[ch], n, latency);
        }

        for (int ch = 0; ch < 2; ++ch)
        {
            std::fill (io[ch], io[ch] + n, 0.0);

            for (int b = 0; b < kNumSatBands; ++b)
            {
                double* buf = bandBuf[(size_t) b][(size_t) ch].data();
                runBand (b, ch, buf, n, bandDriveDb[(size_t) b]);
                for (int i = 0; i < n; ++i)
                    io[ch][i] += buf[i];
            }
        }
    }
    else
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            dryDelay[ch].process (io[ch], dry[ch], n, latency);
            runBand (0, ch, io[ch], n, driveDb);
        }
    }

    // Auto-gain + mix
    const double mixTarget = mix.getTargetValue();
    const double effDrive = multiband ? std::max ({ bandDriveDb[0], bandDriveDb[1], bandDriveDb[2] }) : driveDb;
    const bool agActive = autoGainOn && mixTarget > 0.0 && effDrive > 0.0;

    for (int i = 0; i < n; ++i)
    {
        const double dl = dryL[(size_t) i], dr = dryR[(size_t) i];
        const double wl = L[i], wr = R[i];

        msDry += rmsCoeff * (0.5 * (dl * dl + dr * dr) - msDry);
        msWet += rmsCoeff * (0.5 * (wl * wl + wr * wr) - msWet);

        if ((i % kAutoGainInterval) == 0)
        {
            if (! agActive)
                makeup.setTargetValue (1.0);
            else if (msDry > kAutoGainFloor && msWet > kAutoGainFloor)
            {
                const double maxG = dbToGain (kAutoGainMaxDb);
                makeup.setTargetValue (std::clamp (std::sqrt (msDry / msWet), 1.0 / maxG, maxG));
            }
        }

        const double mg = makeup.processSample();
        const double m = mix.processSample();
        L[i] = dl + m * (mg * wl - dl);
        R[i] = dr + m * (mg * wr - dr);
    }

    if (msDry < 1.0e-30) msDry = 0.0;
    if (msWet < 1.0e-30) msWet = 0.0;

    autoGainDb.store ((float) gainToDb (makeup.getCurrentValue()), std::memory_order_relaxed);
    setGainReductionDb (0.0f);
}

} // namespace meridian
