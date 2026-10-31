#pragma once

#include "DSPModuleBase.h"
#include "common/Biquad.h"

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <atomic>
#include <vector>

namespace meridian
{

/**
    Spectral resonance suppressor used by DynamicEQ.

      - Analysis: Mid signal, 2048-point FFT, Hann window, 75 % overlap
        (hop 512). Each bin keeps an exponential long-term average of its
        magnitude (time constant ~2 s).
      - Bins whose instantaneous / average ratio exceeds kRatioThresholdDb are
        attenuated, reaching kMaxAttenuationDb (-6 dB) at
        kRatioThresholdDb + kRatioRangeDb.
      - The per-bin gains are resampled onto the frequency grid of a short
        linear-phase FIR (frequency sampling + Hann window, kFirLength taps)
        which is applied identically to L and R. New taps are ramped linearly
        over one hop so updates never click.

    kFirLength is 33 (32nd order) rather than 32 so that the group delay is an
    integer number of samples (16) and can be latency-compensated exactly.
    With every gain at 1.0 the FIR is a pure 16-sample delay, so the latency
    is constant whether or not the suppressor is enabled.
*/
class ResonanceSuppressor
{
public:
    static constexpr int kFftOrder = 11;
    static constexpr int kFftSize = 1 << kFftOrder;   // 2048
    static constexpr int kHopSize = kFftSize / 4;     // 75 % overlap
    static constexpr int kNumBins = kFftSize / 2 + 1;
    static constexpr int kFirLength = 33;
    static constexpr int kFirHalf = kFirLength / 2;   // 16
    static constexpr int kNumGridPoints = kFirHalf + 1;
    static constexpr double kAverageTimeSec = 2.0;
    static constexpr double kRatioThresholdDb = 6.0;
    static constexpr double kRatioRangeDb = 6.0;
    static constexpr double kMaxAttenuationDb = -6.0;

    ResonanceSuppressor();

    /** Allocates everything. Not real-time safe. */
    void prepare (double sampleRate);
    void reset() noexcept;
    void setEnabled (bool shouldBeEnabled) noexcept { enabled = shouldBeEnabled; }

    /** In-place stereo processing; adds kFirHalf samples of latency. */
    void process (double* L, double* R, int numSamples) noexcept;

    int getLatencySamples() const noexcept { return kFirHalf; }

    /** Deepest current attenuation in positive dB (audio thread value). */
    double getMaxAttenuationDb() const noexcept { return currentMaxAttenuationDb; }

private:
    void analyseFrame() noexcept;
    void designTargetFir() noexcept;

    juce::dsp::FFT fft { kFftOrder };

    bool enabled = true;
    double fs = 48000.0;
    double averageCoeff = 0.0;

    std::vector<float> analysisRing;     // kFftSize (Mid signal)
    std::vector<float> fftData;          // 2 * kFftSize
    std::vector<float> hannWindow;       // kFftSize
    std::vector<double> binAverage;      // kNumBins
    std::vector<double> binGain;         // kNumBins
    int ringPos = 0;
    int hopCounter = 0;

    std::array<double, kNumGridPoints> gridGain {};
    std::array<double, kFirLength> firWindow {};
    std::array<double, kFirLength> taps {};
    std::array<double, kFirLength> targetTaps {};
    std::array<double, kFirLength> tapStep {};
    int tapRampLeft = 0;

    // Doubled circular delay lines so the convolution never wraps.
    std::array<std::array<double, 2 * kFirLength>, 2> history {};
    int historyPos = 0;

    double currentMaxAttenuationDb = 0.0;
};

/**
    Six-band dynamic EQ with a spectral resonance suppressor.

    Per band:
      - Sidechain: band-pass biquad at the band frequency / Q, taken from the
        signal entering the band (series topology).
      - Detector: RMS (one-pole on the squared signal) with separate attack /
        release coefficients. Stereo bands use a linked detector (mean power of
        the band-passed L and R).
      - Gain computer: below threshold 0 dB; above threshold the gain moves
        linearly toward maxGain, reaching it kGainRangeDb above threshold.
        Negative maxGain = dynamic cut, positive = dynamic boost.
      - Filter: RBJ bell with the time-varying gain. Every kControlInterval
        (64) samples a new target coefficient set is computed and the active
        coefficients are interpolated linearly toward it over the next 64
        samples. The gain change per update is limited to +-12 dB, which keeps
        the interpolated bell coefficients inside the stable region.
      - Channel: Stereo (L and R), Mid or Side (M/S encode/decode around the
        band - lossless, so mixed modes can be chained in series).
      - Disabled bands glide to 0 dB, then are skipped entirely.

    Frequency and Q are smoothed (20 ms). After the bands, the resonance
    suppressor runs (deq_reson_enabled). Module latency = 16 samples.
*/
class DynamicEQ final : public DSPModuleBase
{
public:
    static constexpr int kNumBands = kNumDeqBands;
    static constexpr int kControlInterval = 64;
    static constexpr double kMaxGainStepDb = 12.0;
    static constexpr double kGainRangeDb = 12.0;

    explicit DynamicEQ (juce::AudioProcessorValueTreeState& state);

    ModuleId getId() const noexcept override { return ModuleId::DynamicEQ; }
    void updateParameters() noexcept override;
    void processBlock (double* L, double* R, int numSamples) noexcept override;

    /** Current dynamic gain of a band in dB (negative = cut). Thread-safe. */
    double getBandGainDb (int band) const noexcept
    {
        return bandGainDb[(size_t) juce::jlimit (0, kNumBands - 1, band)].load (std::memory_order_relaxed);
    }

protected:
    void prepareModule (double sampleRate, int maxBlockSize) override;
    void resetModule() noexcept override;

private:
    enum BandChannel { BandStereo = 0, BandMid = 1, BandSide = 2 };

    struct Band
    {
        std::atomic<float>* freqParam = nullptr;
        std::atomic<float>* qParam = nullptr;
        std::atomic<float>* thresholdParam = nullptr;
        std::atomic<float>* maxGainParam = nullptr;
        std::atomic<float>* attackParam = nullptr;
        std::atomic<float>* releaseParam = nullptr;
        std::atomic<float>* channelParam = nullptr;
        std::atomic<float>* enabledParam = nullptr;

        SmoothParam logFreq, q;
        double thresholdDb = -24.0, maxGainDb = -2.0;
        double attackCoeff = 1.0, releaseCoeff = 1.0;
        int channel = BandStereo;
        bool enabled = true;
        bool active = true;

        Biquad sidechain[2];
        double meanSquare = 0.0;

        BiquadCoeffs current, target, step;
        double appliedGainDb = 0.0;
        int rampLeft = 0;
        double z[2][2] {};
    };

    void controlUpdate (Band& band) noexcept;
    double computeGainDb (const Band& band) const noexcept;
    static double msToCoeff (double ms, double fs) noexcept;

    std::array<Band, kNumBands> bands;
    std::array<std::atomic<double>, kNumBands> bandGainDb {};
    std::atomic<float>* resonEnabledParam = nullptr;

    ResonanceSuppressor suppressor;
    int controlCounter = 0;
};

} // namespace meridian
