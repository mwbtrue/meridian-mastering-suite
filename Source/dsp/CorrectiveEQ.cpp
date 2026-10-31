#include "CorrectiveEQ.h"
#include "common/LinearPhaseFIR.h"

#include <algorithm>
#include <cmath>

namespace meridian
{

namespace
{
    using cd = std::complex<double>;
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kMinMagnitude = 1.0e-6;   // -120 dB floor for the cepstrum

    /** In-place iterative radix-2 complex FFT (double precision, design thread only).
        The inverse transform is scaled by 1/n. */
    void fftDouble (std::vector<cd>& a, bool inverse)
    {
        const int n = static_cast<int> (a.size());

        for (int i = 1, j = 0; i < n; ++i)
        {
            int bit = n >> 1;
            for (; (j & bit) != 0; bit >>= 1)
                j ^= bit;
            j ^= bit;
            if (i < j)
                std::swap (a[(size_t) i], a[(size_t) j]);
        }

        for (int len = 2; len <= n; len <<= 1)
        {
            const double ang = 2.0 * kPi / len * (inverse ? 1.0 : -1.0);
            const cd wlen (std::cos (ang), std::sin (ang));

            for (int i = 0; i < n; i += len)
            {
                cd w (1.0, 0.0);
                for (int j = 0; j < len / 2; ++j)
                {
                    const cd u = a[(size_t) (i + j)];
                    const cd v = a[(size_t) (i + j + len / 2)] * w;
                    a[(size_t) (i + j)] = u + v;
                    a[(size_t) (i + j + len / 2)] = u - v;
                    w *= wlen;
                }
            }
        }

        if (inverse)
        {
            const double inv = 1.0 / n;
            for (auto& x : a)
                x *= inv;
        }
    }

    /** Minimum-phase spectrum (full n-point grid) from magnitudes on bins 0..n/2,
        by the real-cepstrum folding method. */
    void minimumPhaseSpectrum (const std::vector<double>& mag, std::vector<cd>& out)
    {
        const int n = static_cast<int> (out.size());
        const int half = n / 2;

        for (int k = 0; k <= half; ++k)
        {
            const double lm = std::log (std::max (mag[(size_t) k], kMinMagnitude));
            out[(size_t) k] = cd (lm, 0.0);
            if (k > 0 && k < half)
                out[(size_t) (n - k)] = cd (lm, 0.0);
        }

        fftDouble (out, true);   // real cepstrum

        for (int i = 1; i < half; ++i)
            out[(size_t) i] = cd (2.0 * out[(size_t) i].real(), 0.0);
        out[0] = cd (out[0].real(), 0.0);
        out[(size_t) half] = cd (out[(size_t) half].real(), 0.0);
        for (int i = half + 1; i < n; ++i)
            out[(size_t) i] = cd (0.0, 0.0);

        fftDouble (out, false);

        for (auto& x : out)
            x = std::exp (x);
    }

    struct DesignBand
    {
        BiquadCoeffs sections[Biquad::kMaxCascadeSections];
        int numSections = 0;
        int channel = 0;
    };

    /** Zero-phase stage magnitudes (L/R stage per channel, M/S stage). */
    void stageMagnitudes (const DesignBand* db, int count, double freq, double fs,
                          double& hL, double& hR, double& hM, double& hS) noexcept
    {
        hL = hR = hM = hS = 1.0;

        for (int b = 0; b < count; ++b)
        {
            double m = 1.0;
            for (int s = 0; s < db[b].numSections; ++s)
                m *= db[b].sections[s].magnitude (freq, fs);

            switch (db[b].channel)
            {
                case DSPModuleBase::Left:  hL *= m; break;
                case DSPModuleBase::Right: hR *= m; break;
                case DSPModuleBase::Mid:   hM *= m; break;
                case DSPModuleBase::Side:  hS *= m; break;
                default:                   hL *= m; hR *= m; break;
            }
        }
    }
}

//==============================================================================
// MatrixConvolver
//==============================================================================
void CorrectiveEQ::MatrixConvolver::prepare (int size)
{
    fftSize = size;
    hopSize = size / 4;
    numBins = size / 2 + 1;

    const int order = juce::roundToInt (std::log2 ((double) size));
    fft = std::make_unique<juce::dsp::FFT> (order);
    writerFft = std::make_unique<juce::dsp::FFT> (order);

    for (auto* w : { &workL, &workR, &outWorkL, &outWorkR, &writerWork })
        w->assign ((size_t) (2 * fftSize), 0.0f);

    for (int c = 0; c < 2; ++c)
    {
        inFifo[(size_t) c].assign ((size_t) hopSize, 0.0);
        outFifo[(size_t) c].assign ((size_t) hopSize, 0.0);
        overlap[(size_t) c].assign ((size_t) fftSize, 0.0);
    }

    for (auto& k : kernels)
    {
        for (auto& s : k.spectra)
            s.assign ((size_t) (2 * numBins), 0.0f);
        k.diagonal = true;
    }

    middle.store (1);
    front = 0;
    back = 2;
    fifoPos = 0;
}

void CorrectiveEQ::MatrixConvolver::reset() noexcept
{
    for (int c = 0; c < 2; ++c)
    {
        std::fill (inFifo[(size_t) c].begin(), inFifo[(size_t) c].end(), 0.0);
        std::fill (outFifo[(size_t) c].begin(), outFifo[(size_t) c].end(), 0.0);
        std::fill (overlap[(size_t) c].begin(), overlap[(size_t) c].end(), 0.0);
    }

    fifoPos = 0;
    acquire();
}

void CorrectiveEQ::MatrixConvolver::publish (const std::array<std::vector<double>, 4>& timeKernels, bool diagonal)
{
    auto& dst = kernels[(size_t) back];
    const int maxLen = fftSize - hopSize + 1;

    for (int e = 0; e < 4; ++e)
    {
        std::fill (writerWork.begin(), writerWork.end(), 0.0f);
        const auto& src = timeKernels[(size_t) e];
        const int len = std::min (static_cast<int> (src.size()), maxLen);

        for (int i = 0; i < len; ++i)
            writerWork[(size_t) i] = static_cast<float> (src[(size_t) i]);

        writerFft->performRealOnlyForwardTransform (writerWork.data(), true);
        std::copy (writerWork.begin(), writerWork.begin() + 2 * numBins, dst.spectra[(size_t) e].begin());
    }

    dst.diagonal = diagonal;

    const int previous = middle.exchange (back | dirtyFlag, std::memory_order_acq_rel);
    back = previous & ~dirtyFlag;
}

void CorrectiveEQ::MatrixConvolver::acquire() noexcept
{
    if ((middle.load (std::memory_order_acquire) & dirtyFlag) == 0)
        return;

    const int previous = middle.exchange (front, std::memory_order_acq_rel);
    front = previous & ~dirtyFlag;
}

void CorrectiveEQ::MatrixConvolver::process (double* L, double* R, int numSamples) noexcept
{
    double* io[2] = { L, R };
    int done = 0;

    while (done < numSamples)
    {
        const int chunk = std::min (numSamples - done, hopSize - fifoPos);

        for (int c = 0; c < 2; ++c)
        {
            double* in  = inFifo[(size_t) c].data() + fifoPos;
            double* out = outFifo[(size_t) c].data() + fifoPos;

            for (int i = 0; i < chunk; ++i)
            {
                in[i] = io[c][done + i];
                io[c][done + i] = out[i];
            }
        }

        fifoPos += chunk;
        done += chunk;

        if (fifoPos == hopSize)
        {
            processHop();
            fifoPos = 0;
        }
    }
}

void CorrectiveEQ::MatrixConvolver::processHop() noexcept
{
    acquire();
    const auto& K = kernels[(size_t) front];

    std::fill (workL.begin(), workL.end(), 0.0f);
    std::fill (workR.begin(), workR.end(), 0.0f);

    for (int i = 0; i < hopSize; ++i)
    {
        workL[(size_t) i] = static_cast<float> (inFifo[0][(size_t) i]);
        workR[(size_t) i] = static_cast<float> (inFifo[1][(size_t) i]);
    }

    fft->performRealOnlyForwardTransform (workL.data(), true);
    fft->performRealOnlyForwardTransform (workR.data(), true);

    std::fill (outWorkL.begin(), outWorkL.end(), 0.0f);
    std::fill (outWorkR.begin(), outWorkR.end(), 0.0f);

    const float* hLL = K.spectra[0].data();
    const float* hLR = K.spectra[1].data();
    const float* hRL = K.spectra[2].data();
    const float* hRR = K.spectra[3].data();

    for (int k = 0; k < numBins; ++k)
    {
        const float lr = workL[(size_t) (2 * k)], li = workL[(size_t) (2 * k + 1)];
        const float rr = workR[(size_t) (2 * k)], ri = workR[(size_t) (2 * k + 1)];

        float yLr = lr * hLL[2 * k] - li * hLL[2 * k + 1];
        float yLi = lr * hLL[2 * k + 1] + li * hLL[2 * k];
        float yRr = rr * hRR[2 * k] - ri * hRR[2 * k + 1];
        float yRi = rr * hRR[2 * k + 1] + ri * hRR[2 * k];

        if (! K.diagonal)
        {
            yLr += rr * hLR[2 * k] - ri * hLR[2 * k + 1];
            yLi += rr * hLR[2 * k + 1] + ri * hLR[2 * k];
            yRr += lr * hRL[2 * k] - li * hRL[2 * k + 1];
            yRi += lr * hRL[2 * k + 1] + li * hRL[2 * k];
        }

        outWorkL[(size_t) (2 * k)] = yLr;  outWorkL[(size_t) (2 * k + 1)] = yLi;
        outWorkR[(size_t) (2 * k)] = yRr;  outWorkR[(size_t) (2 * k + 1)] = yRi;
    }

    fft->performRealOnlyInverseTransform (outWorkL.data());
    fft->performRealOnlyInverseTransform (outWorkR.data());

    const float* results[2] = { outWorkL.data(), outWorkR.data() };

    for (int c = 0; c < 2; ++c)
    {
        auto& ola = overlap[(size_t) c];

        for (int i = 0; i < fftSize; ++i)
            ola[(size_t) i] += static_cast<double> (results[c][i]);

        std::copy (ola.begin(), ola.begin() + hopSize, outFifo[(size_t) c].begin());
        std::move (ola.begin() + hopSize, ola.end(), ola.begin());
        std::fill (ola.end() - hopSize, ola.end(), 0.0);
    }
}

//==============================================================================
// Design thread
//==============================================================================
void CorrectiveEQ::DesignThread::run()
{
    while (! threadShouldExit())
    {
        owner.designerTick();
        wait (30);
    }
}

//==============================================================================
// CorrectiveEQ
//==============================================================================
CorrectiveEQ::CorrectiveEQ (juce::AudioProcessorValueTreeState& state)
    : DSPModuleBase (state)
{
    namespace f = pid::field;

    for (int b = 0; b < kNumCeqBands; ++b)
    {
        auto& p = params[(size_t) b];
        p.freq    = rawParam (pid::bandParam (pid::ceqBandPrefix, b, f::freq));
        p.gain    = rawParam (pid::bandParam (pid::ceqBandPrefix, b, f::gain));
        p.q       = rawParam (pid::bandParam (pid::ceqBandPrefix, b, f::q));
        p.type    = rawParam (pid::bandParam (pid::ceqBandPrefix, b, f::type));
        p.slope   = rawParam (pid::bandParam (pid::ceqBandPrefix, b, f::slope));
        p.enabled = rawParam (pid::bandParam (pid::ceqBandPrefix, b, f::enabled));
        p.channel = rawParam (pid::bandParam (pid::ceqBandPrefix, b, f::channel));
    }

    phaseModeParam  = rawParam (pid::ceqPhaseMode);
    lowLatencyParam = rawParam (pid::lowLatencyMode);
}

CorrectiveEQ::~CorrectiveEQ()
{
    designThread.stopThread (2000);
}

int CorrectiveEQ::firSizeForSampleRate (double sampleRate) noexcept
{
    if (sampleRate <= 50000.0)  return 8192;
    if (sampleRate <= 100000.0) return 16384;
    return 32768;
}

bool CorrectiveEQ::typeUsesGain (int type) noexcept
{
    const auto t = static_cast<FilterType> (type);
    return t == FilterType::Bell || t == FilterType::LowShelf || t == FilterType::HighShelf || t == FilterType::Tilt;
}

int CorrectiveEQ::computeBandSections (const BandSettings& band, double fs, BiquadCoeffs* out) noexcept
{
    const double f = Biquad::clampFrequency (band.freq, fs);
    const double q = std::max (band.q, 0.05);
    const int slopeIdx = std::clamp (band.slopeIndex, 0, (int) choices::ceqSlopesDbPerOct.size() - 1);
    const int order = choices::ceqSlopesDbPerOct[(size_t) slopeIdx] / 6;

    switch (static_cast<FilterType> (band.type))
    {
        case FilterType::Bell:             out[0] = Biquad::makeBell (f, q, band.gainDb, fs);      return 1;
        case FilterType::LowShelf:         out[0] = Biquad::makeLowShelf (f, q, band.gainDb, fs);  return 1;
        case FilterType::HighShelf:        out[0] = Biquad::makeHighShelf (f, q, band.gainDb, fs); return 1;
        case FilterType::Notch:            out[0] = Biquad::makeNotch (f, q, fs);                   return 1;
        case FilterType::Tilt:             out[0] = Biquad::makeTilt (f, band.gainDb, fs);          return 1;
        case FilterType::HighPass:         return Biquad::makeButterworthCascade (true,  order, f, q, fs, out);
        case FilterType::LowPass:          return Biquad::makeButterworthCascade (false, order, f, q, fs, out);
        case FilterType::EllipticHighPass: return Biquad::makeEllipticHPF (f, fs, out);
        default:                           break;
    }

    return 0;
}

double CorrectiveEQ::computeBandMagnitude (const BandSettings& band, double freq, double fs) noexcept
{
    BiquadCoeffs sections[kMaxSections];
    const int n = computeBandSections (band, fs, sections);
    double m = 1.0;

    for (int s = 0; s < n; ++s)
        m *= sections[s].magnitude (freq, fs);

    return m;
}

double CorrectiveEQ::getBandGainDb (int band) const noexcept
{
    return (band >= 0 && band < kNumCeqBands) ? bandGainDisplay[(size_t) band].load (std::memory_order_relaxed) : 0.0;
}

CorrectiveEQ::AllBands CorrectiveEQ::readSettings() const noexcept
{
    AllBands out;

    for (int b = 0; b < kNumCeqBands; ++b)
    {
        const auto& p = params[(size_t) b];
        auto& s = out[(size_t) b];
        s.freq       = load (p.freq);
        s.gainDb     = load (p.gain);
        s.q          = load (p.q);
        s.type       = loadInt (p.type);
        s.slopeIndex = loadInt (p.slope);
        s.enabled    = loadBool (p.enabled);
        s.channel    = std::clamp (loadInt (p.channel), 0, 4);
    }

    return out;
}

CorrectiveEQ::PhaseMode CorrectiveEQ::readEffectiveMode() const noexcept
{
    if (loadBool (lowLatencyParam))
        return PhaseMode::Minimum;

    return static_cast<PhaseMode> (std::clamp (loadInt (phaseModeParam), 0, 2));
}

//==============================================================================
void CorrectiveEQ::prepareModule (double sampleRate, int /*maxBlockSize*/)
{
    designThread.stopThread (2000);

    firSize = firSizeForSampleRate (sampleRate);
    linearEngine.prepare (firSize);
    naturalEngine.prepare (firSize);

    amountBuffer.assign ((size_t) (kNumCeqBands * kControlInterval), 0.0);
    segTemp.assign ((size_t) kControlInterval, 0.0);

    const auto settings = readSettings();

    for (int b = 0; b < kNumCeqBands; ++b)
    {
        auto& st = bands[(size_t) b];
        st.logFreq.prepare (sampleRate, 15.0);
        st.gain.prepare (sampleRate, 15.0);
        st.q.prepare (sampleRate, 15.0);
        st.amount.prepare (sampleRate, 5.0);
        setBandTargets (b, settings[(size_t) b], true);
    }

    resetBiquads();

    currentMode = static_cast<int> (readEffectiveMode());
    effectiveMode.store (currentMode);
    setLatencySamples (currentMode == (int) PhaseMode::Linear  ? firSize / 2
                     : currentMode == (int) PhaseMode::Natural ? firSize / 4 : 0);
    setGainReductionDb (0.0f);

    {
        std::lock_guard<std::mutex> lock (designMutex);
        hasDesign = false;
    }

    designFirKernelsNow();
    linearEngine.reset();
    naturalEngine.reset();

    designThread.startThread (juce::Thread::Priority::low);
}

void CorrectiveEQ::resetModule() noexcept
{
    for (int b = 0; b < kNumCeqBands; ++b)
        setBandTargets (b, requested[(size_t) b], true);

    resetBiquads();
    linearEngine.reset();
    naturalEngine.reset();
}

void CorrectiveEQ::resetBiquads() noexcept
{
    for (auto& st : bands)
        for (auto& ch : st.filters)
            for (auto& f : ch)
                f.reset();
}

//==============================================================================
void CorrectiveEQ::setBandTargets (int b, const BandSettings& s, bool snap) noexcept
{
    auto& st = bands[(size_t) b];
    requested[(size_t) b] = s;

    const double logF = std::log2 (std::max (s.freq, 1.0));
    st.logFreq.setTargetValue (logF);
    st.gain.setTargetValue (s.gainDb);
    st.q.setTargetValue (s.q);

    const bool discreteChanged = s.type != st.active.type || s.slopeIndex != st.active.slopeIndex
                              || s.channel != st.active.channel;

    if (snap)
    {
        st.active = s;
        st.logFreq.reset (logF);
        st.gain.reset (s.gainDb);
        st.q.reset (s.q);
        st.amount.reset (s.enabled ? 1.0 : 0.0);
        st.swapPending = false;
        recomputeBand (b);
        return;
    }

    st.active.enabled = s.enabled;

    if (discreteChanged)
    {
        st.swapPending = true;
        st.amount.setTargetValue (0.0);
    }
    else if (! st.swapPending)
    {
        st.amount.setTargetValue (s.enabled ? 1.0 : 0.0);
    }
}

void CorrectiveEQ::recomputeBand (int b) noexcept
{
    auto& st = bands[(size_t) b];
    BandSettings s = st.active;
    s.freq   = std::exp2 (st.logFreq.getCurrentValue());
    s.gainDb = st.gain.getCurrentValue();
    s.q      = st.q.getCurrentValue();

    st.numSections = computeBandSections (s, currentSampleRate, st.coeffs);

    for (int c = 0; c < 2; ++c)
        for (int k = 0; k < st.numSections; ++k)
            st.filters[c][k].setCoefficients (st.coeffs[k]);
}

void CorrectiveEQ::advanceBands (int n, double* amountOut) noexcept
{
    for (int b = 0; b < kNumCeqBands; ++b)
    {
        auto& st = bands[(size_t) b];

        // Fade-in from fully off: start from a clean filter state.
        if (st.amount.getCurrentValue() == 0.0 && st.amount.getTargetValue() > 0.0)
            for (auto& ch : st.filters)
                for (auto& f : ch)
                    f.reset();

        double* amt = amountOut + b * kControlInterval;
        for (int i = 0; i < n; ++i)
            amt[i] = st.amount.processSample();

        if (st.logFreq.isSmoothing() || st.gain.isSmoothing() || st.q.isSmoothing())
        {
            st.logFreq.skip (n);
            st.gain.skip (n);
            st.q.skip (n);
            recomputeBand (b);
        }

        if (st.swapPending && st.amount.getCurrentValue() == 0.0)
        {
            const auto& r = requested[(size_t) b];
            st.active.type = r.type;
            st.active.slopeIndex = r.slopeIndex;
            st.active.channel = r.channel;
            st.active.enabled = r.enabled;
            st.swapPending = false;
            recomputeBand (b);

            for (auto& ch : st.filters)
                for (auto& f : ch)
                    f.reset();

            st.amount.setTargetValue (r.enabled ? 1.0 : 0.0);
        }

        const double shown = typeUsesGain (st.active.type) ? st.gain.getCurrentValue() * st.amount.getCurrentValue() : 0.0;
        bandGainDisplay[(size_t) b].store (shown, std::memory_order_relaxed);
    }
}

void CorrectiveEQ::updateParameters() noexcept
{
    const auto settings = readSettings();

    for (int b = 0; b < kNumCeqBands; ++b)
        if (settings[(size_t) b] != requested[(size_t) b])
            setBandTargets (b, settings[(size_t) b], false);

    const int mode = static_cast<int> (readEffectiveMode());

    if (mode != currentMode)
    {
        if (mode == (int) PhaseMode::Linear)
            linearEngine.reset();
        else if (mode == (int) PhaseMode::Natural)
            naturalEngine.reset();
        else
            resetBiquads();

        currentMode = mode;
        effectiveMode.store (mode, std::memory_order_relaxed);
    }

    setLatencySamples (currentMode == (int) PhaseMode::Linear  ? firSize / 2
                     : currentMode == (int) PhaseMode::Natural ? firSize / 4 : 0);
}

void CorrectiveEQ::processBlock (double* L, double* R, int numSamples) noexcept
{
    if (currentMode == (int) PhaseMode::Minimum)
    {
        processMinimumPhase (L, R, numSamples);
        return;
    }

    // FIR modes: keep the biquad-path smoothers in sync for seamless switching back.
    for (int offset = 0; offset < numSamples; offset += kControlInterval)
        advanceBands (std::min (kControlInterval, numSamples - offset), amountBuffer.data());

    if (currentMode == (int) PhaseMode::Linear)
        linearEngine.process (L, R, numSamples);
    else
        naturalEngine.process (L, R, numSamples);
}

void CorrectiveEQ::processMinimumPhase (double* L, double* R, int numSamples) noexcept
{
    for (int offset = 0; offset < numSamples; offset += kControlInterval)
    {
        const int n = std::min (kControlInterval, numSamples - offset);
        double* seg[2] = { L + offset, R + offset };

        advanceBands (n, amountBuffer.data());

        auto applyBand = [this, n] (BandState& st, const double* amt, int filterChannel, double* x) noexcept
        {
            double* tmp = segTemp.data();
            std::copy (x, x + n, tmp);

            for (int k = 0; k < st.numSections; ++k)
                st.filters[filterChannel][k].processBlock (tmp, n);

            for (int i = 0; i < n; ++i)
                x[i] += amt[i] * (tmp[i] - x[i]);
        };

        bool anyMidSide = false;

        // Stage A: Stereo / Left / Right bands in the L/R domain.
        for (int b = 0; b < kNumCeqBands; ++b)
        {
            auto& st = bands[(size_t) b];
            const double* amt = amountBuffer.data() + b * kControlInterval;

            if (amt[0] == 0.0 && amt[n - 1] == 0.0)
                continue;

            switch (st.active.channel)
            {
                case Stereo: applyBand (st, amt, 0, seg[0]); applyBand (st, amt, 1, seg[1]); break;
                case Left:   applyBand (st, amt, 0, seg[0]); break;
                case Right:  applyBand (st, amt, 1, seg[1]); break;
                default:     anyMidSide = true; break;
            }
        }

        // Stage B: Mid / Side bands in the M/S domain.
        if (anyMidSide)
        {
            MidSide::encodeStereoBlock (seg[0], seg[1], n);

            for (int b = 0; b < kNumCeqBands; ++b)
            {
                auto& st = bands[(size_t) b];
                const double* amt = amountBuffer.data() + b * kControlInterval;

                if (amt[0] == 0.0 && amt[n - 1] == 0.0)
                    continue;

                if (st.active.channel == Mid)
                    applyBand (st, amt, 0, seg[0]);
                else if (st.active.channel == Side)
                    applyBand (st, amt, 1, seg[1]);
            }

            MidSide::decodeStereoBlock (seg[0], seg[1], n);
        }
    }

    for (auto& st : bands)
        for (auto& ch : st.filters)
            for (auto& f : ch)
                f.snapToZero();
}

//==============================================================================
// FIR design (background thread / prepare)
//==============================================================================
void CorrectiveEQ::designerTick()
{
    if (readEffectiveMode() == PhaseMode::Minimum)
        return;

    const auto settings = readSettings();

    {
        std::lock_guard<std::mutex> lock (designMutex);
        if (hasDesign && settings == lastDesigned)
            return;
    }

    designAndPublish (settings);
}

void CorrectiveEQ::designFirKernelsNow()
{
    designAndPublish (readSettings());
}

void CorrectiveEQ::designAndPublish (const AllBands& settings)
{
    std::lock_guard<std::mutex> lock (designMutex);

    const double fs = currentSampleRate;
    const int N = firSize;

    // ---- Band sections (enabled bands only) --------------------------------------
    std::array<DesignBand, kNumCeqBands> db {};
    int count = 0;
    bool anyMidSide = false;

    for (const auto& s : settings)
    {
        if (! s.enabled)
            continue;

        auto& d = db[(size_t) count++];
        d.numSections = computeBandSections (s, fs, d.sections);
        d.channel = s.channel;
        anyMidSide = anyMidSide || s.channel == Mid || s.channel == Side;
    }

    const bool diagonal = ! anyMidSide;

    // ---- Linear phase: zero-phase 2x2 matrix -> windowed, centred kernels --------
    {
        std::array<std::vector<cd>, 4> spec;
        for (auto& s : spec)
            s.assign ((size_t) N, cd (0.0, 0.0));

        for (int k = 0; k <= N / 2; ++k)
        {
            double hL, hR, hM, hS;
            stageMagnitudes (db.data(), count, k * fs / N, fs, hL, hR, hM, hS);

            const double sum = 0.5 * (hM + hS), diff = 0.5 * (hM - hS);
            const double t[4] = { sum * hL, diff * hR, diff * hL, sum * hR };

            for (int e = 0; e < 4; ++e)
            {
                spec[(size_t) e][(size_t) k] = cd (t[e], 0.0);
                if (k > 0 && k < N / 2)
                    spec[(size_t) e][(size_t) (N - k)] = cd (t[e], 0.0);
            }
        }

        const int taps = N / 2 + 1;
        const int half = N / 4;
        std::vector<double> window ((size_t) taps);
        LinearPhaseFIR::makeKaiserWindow (window.data(), taps, 8.0);

        std::array<std::vector<double>, 4> kernels;

        for (int e = 0; e < 4; ++e)
        {
            fftDouble (spec[(size_t) e], true);
            kernels[(size_t) e].assign ((size_t) taps, 0.0);

            for (int n = 0; n < taps; ++n)
            {
                const int src = (n - half + N) % N;
                kernels[(size_t) e][(size_t) n] = spec[(size_t) e][(size_t) src].real() * window[(size_t) n];
            }
        }

        linearEngine.publish (kernels, diagonal);
    }

    // ---- Natural phase: minimum-phase stages -> causal matrix kernels -------------
    {
        const int Nc = 2 * N;
        const int halfC = Nc / 2;

        std::array<std::vector<double>, 4> mags;   // L, R, M, S on bins 0..Nc/2
        for (auto& m : mags)
            m.assign ((size_t) (halfC + 1), 1.0);

        for (int k = 0; k <= halfC; ++k)
            stageMagnitudes (db.data(), count, k * fs / Nc, fs,
                             mags[0][(size_t) k], mags[1][(size_t) k], mags[2][(size_t) k], mags[3][(size_t) k]);

        std::array<std::vector<cd>, 4> mp;
        for (int s = 0; s < 4; ++s)
        {
            mp[(size_t) s].assign ((size_t) Nc, cd (1.0, 0.0));
            const bool flat = std::all_of (mags[(size_t) s].begin(), mags[(size_t) s].end(),
                                           [] (double v) { return v == 1.0; });
            if (! flat)
                minimumPhaseSpectrum (mags[(size_t) s], mp[(size_t) s]);
        }

        std::array<std::vector<cd>, 4> spec;
        for (auto& s : spec)
            s.assign ((size_t) Nc, cd (0.0, 0.0));

        for (int k = 0; k <= halfC; ++k)
        {
            const cd hL = mp[0][(size_t) k], hR = mp[1][(size_t) k];
            const cd hM = mp[2][(size_t) k], hS = mp[3][(size_t) k];
            const cd sum = 0.5 * (hM + hS), diff = 0.5 * (hM - hS);
            const cd t[4] = { sum * hL, diff * hR, diff * hL, sum * hR };

            for (int e = 0; e < 4; ++e)
            {
                spec[(size_t) e][(size_t) k] = t[e];
                if (k > 0 && k < halfC)
                    spec[(size_t) e][(size_t) (Nc - k)] = std::conj (t[e]);
            }
        }

        const int taps = N / 2;
        const int fadeLen = N / 16;
        std::array<std::vector<double>, 4> kernels;

        for (int e = 0; e < 4; ++e)
        {
            fftDouble (spec[(size_t) e], true);
            kernels[(size_t) e].assign ((size_t) taps, 0.0);

            for (int n = 0; n < taps; ++n)
            {
                double w = 1.0;
                const int fromEnd = taps - 1 - n;
                if (fromEnd < fadeLen)
                    w = 0.5 - 0.5 * std::cos (kPi * fromEnd / fadeLen);

                kernels[(size_t) e][(size_t) n] = spec[(size_t) e][(size_t) n].real() * w;
            }
        }

        naturalEngine.publish (kernels, diagonal);
    }

    lastDesigned = settings;
    hasDesign = true;
}

} // namespace meridian
