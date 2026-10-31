#include "ReferencePanel.h"

#include "MeteringEngine.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace meridian
{

namespace
{
    constexpr double kMainAverageAlpha = 0.006;   // ~3 s at ~94 frames/s (48 kHz, hop 512)
    constexpr float kFloorDb = SpectrumFrame::kFloorDb;
    constexpr double kMatchLowHz = 100.0, kMatchHighHz = 10000.0;
    constexpr double kGaussSigma = 2.5;           // bins; kernel +/-5 bins (~10 bins wide)
    constexpr int kGaussHalf = 5;

    double dbToPower (float db) noexcept { return std::pow (10.0, 0.1 * static_cast<double> (db)); }
    float powerToDb (double p) noexcept
    {
        return p > 1.0e-30 ? std::max (kFloorDb, static_cast<float> (10.0 * std::log10 (p))) : kFloorDb;
    }
}

ReferencePanel::ReferencePanel()
{
    refDb.fill (kFloorDb);
}

ReferencePanel::~ReferencePanel()
{
    joinLoader();
}

void ReferencePanel::joinLoader()
{
    cancelLoad.store (true);
    if (loader.joinable())
        loader.join();
    cancelLoad.store (false);
}

void ReferencePanel::loadReference (const juce::File& file)
{
    joinLoader();
    listening.store (false);
    state.store (static_cast<int> (LoadState::Loading));
    loader = std::thread ([this, file] { loaderThread (file); });
}

juce::String ReferencePanel::getReferenceName() const
{
    const std::lock_guard<std::mutex> lock (dataMutex);
    return refName;
}

juce::String ReferencePanel::getLastError() const
{
    const std::lock_guard<std::mutex> lock (dataMutex);
    return lastError;
}

float ReferencePanel::getLevelMatchGain_dB() const noexcept
{
    const float m = mainLufs.load (std::memory_order_relaxed);
    const float r = refLufs.load (std::memory_order_relaxed);
    if (m <= -70.0f || r <= -70.0f)
        return 0.0f;
    return r - m;
}

//==============================================================================
void ReferencePanel::loaderThread (juce::File file)
{
    auto failWith = [this] (const juce::String& why)
    {
        {
            const std::lock_guard<std::mutex> lock (dataMutex);
            lastError = why;
        }
        state.store (static_cast<int> (LoadState::Failed));
    };

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr)
        return failWith ("Unsupported or unreadable file: " + file.getFileName());

    const double srcRate = reader->sampleRate > 0.0 ? reader->sampleRate : 44100.0;
    const auto maxSamples = static_cast<juce::int64> (srcRate * 60.0 * 20.0);   // 20 min cap
    const int length = static_cast<int> (std::min (reader->lengthInSamples, maxSamples));
    if (length < kFftSize)
        return failWith ("Reference is too short");

    juce::AudioBuffer<float> decoded (2, length);
    reader->read (&decoded, 0, length, 0, true, true);
    if (reader->numChannels == 1)
        decoded.copyFrom (1, 0, decoded, 0, 0, length);
    reader.reset();

    if (cancelLoad.load())
        return failWith ("Cancelled");

    // ---- Resample to the playback rate ----------------------------------------------
    const double dstRate = playbackRate.load();
    auto resampled = std::make_unique<juce::AudioBuffer<float>>();

    if (std::abs (dstRate - srcRate) < 0.5)
    {
        resampled->makeCopyOf (decoded);
    }
    else
    {
        const double ratio = srcRate / dstRate;
        const int outLength = static_cast<int> (std::floor ((length - 4) / ratio));
        resampled->setSize (2, std::max (1, outLength));

        for (int ch = 0; ch < 2; ++ch)
        {
            juce::LagrangeInterpolator interp;
            interp.process (ratio, decoded.getReadPointer (ch), resampled->getWritePointer (ch), outLength);
        }
    }

    const int n = resampled->getNumSamples();
    const float* L = resampled->getReadPointer (0);
    const float* R = resampled->getReadPointer (1);

    // ---- Loudness (offline MeteringEngine pass) ------------------------------------------
    MeteringEngine meter;
    constexpr int kBlock = 4096;
    meter.prepare (dstRate, kBlock);
    meter.reset();

    std::vector<double> bl ((size_t) kBlock), br ((size_t) kBlock);
    double sumM = 0.0, sumS = 0.0;

    for (int pos = 0; pos < n; pos += kBlock)
    {
        const int len = std::min (kBlock, n - pos);
        for (int i = 0; i < len; ++i)
        {
            bl[(size_t) i] = L[pos + i];
            br[(size_t) i] = R[pos + i];
            const double m = 0.5 * (bl[(size_t) i] + br[(size_t) i]);
            const double s = 0.5 * (bl[(size_t) i] - br[(size_t) i]);
            sumM += m * m;
            sumS += s * s;
        }
        meter.process (bl.data(), br.data(), len);

        if (cancelLoad.load())
            return failWith ("Cancelled");
    }

    MeterData md;
    meter.fillMeterData (md);

    // ---- Average spectrum ---------------------------------------------------------------
    juce::dsp::FFT fft (kFftOrder);
    std::vector<float> window ((size_t) kFftSize), work ((size_t) kFftSize * 2);
    for (int i = 0; i < kFftSize; ++i)
        window[(size_t) i] = static_cast<float> (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * i / kFftSize));

    std::vector<double> power ((size_t) kNumBins, 0.0);
    int frames = 0;
    const double scale = 4.0 / kFftSize;

    for (int start = 0; start + kFftSize <= n; start += kFftSize / 2)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            const float* x = ch == 0 ? L : R;
            std::fill (work.begin(), work.end(), 0.0f);
            for (int i = 0; i < kFftSize; ++i)
                work[(size_t) i] = x[start + i] * window[(size_t) i];

            fft.performFrequencyOnlyForwardTransform (work.data(), true);

            for (int k = 0; k < kNumBins; ++k)
            {
                const double mag = static_cast<double> (work[(size_t) k]) * scale;
                power[(size_t) k] += 0.5 * mag * mag;
            }
        }

        ++frames;
        if ((frames & 63) == 0 && cancelLoad.load())
            return failWith ("Cancelled");
    }

    std::array<float, kNumBins> spectrum {};
    for (int k = 0; k < kNumBins; ++k)
        spectrum[(size_t) k] = powerToDb (frames > 0 ? power[(size_t) k] / frames : 0.0);

    // ---- Publish ----------------------------------------------------------------------------
    {
        const std::lock_guard<std::mutex> lock (dataMutex);
        refDb = spectrum;
        refWidth = sumM > 0.0 ? static_cast<float> (100.0 * std::sqrt (sumS / sumM)) : 0.0f;
        refCrest = md.integratedLUFS > -70.0f ? md.truePeak - md.integratedLUFS : 0.0f;
        refName = file.getFileNameWithoutExtension();
        lastError.clear();
    }

    refLufs.store (md.integratedLUFS);

    {
        const juce::SpinLock::ScopedLockType sl (playbackLock);
        std::swap (playback, resampled);
        playhead = 0;
    }

    resampled.reset();   // previous buffer freed here, outside the lock
    state.store (static_cast<int> (LoadState::Loaded));
}

//==============================================================================
bool ReferencePanel::renderReference (double* L, double* R, int numSamples) noexcept
{
    if (! listening.load (std::memory_order_relaxed))
        return false;

    const juce::SpinLock::ScopedTryLockType tl (playbackLock);
    if (! tl.isLocked() || playback == nullptr || playback->getNumSamples() == 0)
        return false;

    const float target = juce::Decibels::decibelsToGain (-getLevelMatchGain_dB());
    const float step = (target - appliedGain) / static_cast<float> (std::max (1, numSamples));

    const int len = playback->getNumSamples();
    const float* srcL = playback->getReadPointer (0);
    const float* srcR = playback->getReadPointer (1);

    for (int i = 0; i < numSamples; ++i)
    {
        appliedGain += step;
        L[i] = static_cast<double> (srcL[playhead] * appliedGain);
        R[i] = static_cast<double> (srcR[playhead] * appliedGain);
        if (++playhead >= len)
            playhead = 0;
    }

    appliedGain = target;
    return true;
}

void ReferencePanel::getOutputBuffer (float* L, float* R, int numSamples) noexcept
{
    constexpr int kChunk = 256;
    double tl[kChunk], tr[kChunk];

    for (int pos = 0; pos < numSamples; pos += kChunk)
    {
        const int len = std::min (kChunk, numSamples - pos);
        if (renderReference (tl, tr, len))
        {
            for (int i = 0; i < len; ++i)
            {
                L[pos + i] = static_cast<float> (tl[i]);
                R[pos + i] = static_cast<float> (tr[i]);
            }
        }
        else
        {
            std::fill (L + pos, L + pos + len, 0.0f);
            std::fill (R + pos, R + pos + len, 0.0f);
        }
    }
}

//==============================================================================
void ReferencePanel::setMainFFT (const float* binsDb, int numBins)
{
    if (binsDb == nullptr || numBins <= 0)
        return;

    const int count = std::min (numBins, kNumBins);
    const std::lock_guard<std::mutex> lock (dataMutex);

    for (int k = 0; k < count; ++k)
    {
        const double p = dbToPower (binsDb[k]);
        mainPower[(size_t) k] = mainPowerValid ? mainPower[(size_t) k] + kMainAverageAlpha * (p - mainPower[(size_t) k]) : p;
    }

    mainPowerValid = true;
}

void ReferencePanel::setMainSpectrumFrame (const SpectrumFrame& frame)
{
    double m = 0.0, s = 0.0;
    for (int k = 1; k < kNumBins; ++k)
    {
        m += dbToPower (frame.magnitudeMid[k]);
        s += dbToPower (frame.magnitudeSide[k]);
    }

    {
        const std::lock_guard<std::mutex> lock (dataMutex);
        mainMidPower += kMainAverageAlpha * (m - mainMidPower);
        mainSidePower += kMainAverageAlpha * (s - mainSidePower);
    }

    setMainFFT (frame.magnitude, SpectrumFrame::numBins);
}

std::array<float, ReferencePanel::kNumBins> ReferencePanel::getReferenceSpectrum() const
{
    const float offset = isLoaded() ? -getLevelMatchGain_dB() : 0.0f;
    const std::lock_guard<std::mutex> lock (dataMutex);
    auto out = refDb;
    for (auto& v : out)
        v = std::max (kFloorDb, v + offset);
    return out;
}

ReferencePanel::SpectralComparison ReferencePanel::getSpectralComparison() const
{
    SpectralComparison c;
    const float offset = -getLevelMatchGain_dB();

    c.mainLoudness = mainLufs.load();
    c.refLoudness  = refLufs.load();
    c.mainCrest    = mainCrest.load();

    const std::lock_guard<std::mutex> lock (dataMutex);
    c.refWidth  = refWidth;
    c.refCrest  = refCrest;
    c.mainWidth = mainMidPower > 0.0 ? static_cast<float> (100.0 * std::sqrt (mainSidePower / mainMidPower)) : 0.0f;

    const bool haveBoth = mainPowerValid && state.load() == static_cast<int> (LoadState::Loaded);
    for (int k = 0; k < kNumBins; ++k)
        c.diffCurve[k] = haveBoth ? (refDb[(size_t) k] + offset) - powerToDb (mainPower[(size_t) k]) : 0.0f;

    return c;
}

ReferencePanel::MatchEQCurve ReferencePanel::getMatchEQ (float amountPct) const
{
    MatchEQCurve curve;
    curve.amount_pct = juce::jlimit (0.0f, 100.0f, amountPct);

    const auto cmp = getSpectralComparison();
    const double rate = playbackRate.load();
    const double binHz = rate / kFftSize;

    // Remove the broadband offset: tonal correction only.
    double mean = 0.0;
    int count = 0;
    for (int k = 1; k < kNumBins; ++k)
    {
        const double f = k * binHz;
        if (f >= kMatchLowHz && f <= kMatchHighHz)
        {
            mean += cmp.diffCurve[k];
            ++count;
        }
    }
    mean = count > 0 ? mean / count : 0.0;

    const float amount = curve.amount_pct * 0.01f;
    std::array<float, kNumBins> clamped {};
    for (int k = 0; k < kNumBins; ++k)
        clamped[(size_t) k] = juce::jlimit (-kMaxMatchDb, kMaxMatchDb,
                                            static_cast<float> ((cmp.diffCurve[k] - mean) * amount));

    std::array<double, 2 * kGaussHalf + 1> kernel {};
    for (int j = -kGaussHalf; j <= kGaussHalf; ++j)
        kernel[(size_t) (j + kGaussHalf)] = std::exp (-0.5 * (j * j) / (kGaussSigma * kGaussSigma));

    for (int k = 0; k < kNumBins; ++k)
    {
        double acc = 0.0, wsum = 0.0;
        for (int j = -kGaussHalf; j <= kGaussHalf; ++j)
        {
            const int idx = k + j;
            if (idx < 0 || idx >= kNumBins)
                continue;
            const double w = kernel[(size_t) (j + kGaussHalf)];
            acc += w * clamped[(size_t) idx];
            wsum += w;
        }
        curve.gainPerBin[k] = juce::jlimit (-kMaxMatchDb, kMaxMatchDb, static_cast<float> (acc / wsum));
    }

    return curve;
}

} // namespace meridian
