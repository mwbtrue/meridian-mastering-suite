#pragma once

#include "DSPModuleBase.h"
#include "common/Biquad.h"

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <atomic>
#include <complex>
#include <memory>
#include <mutex>
#include <vector>

namespace meridian
{

/**
    8-band corrective parametric EQ with three phase modes.

    Band types: Bell, Low Shelf, High Shelf, Notch, Tilt, High Pass, Low Pass,
    Elliptic High Pass. HP/LP slopes 6..96 dB/oct are Butterworth cascades
    (order = slope / 6, up to 8 biquad sections); the band Q scales the
    resonance of the highest-Q section. Elliptic HP is a fixed 4th-order
    design (0.1 dB ripple, 80 dB stopband) scaled to the band frequency.

    Channel routing per band (Stereo / Mid / Side / Left / Right) uses a fixed,
    mode-independent topology: all Stereo/Left/Right bands are applied first in
    the L/R domain, then all Mid/Side bands in the M/S domain. Every phase mode
    realises exactly this cascade, so switching modes changes only the phase.

    Phase modes (ceq_phase_mode; low_latency_mode forces Minimum):
      Minimum  - biquad cascade, zero latency. Frequency/gain/Q are smoothed and
                 coefficients recomputed every 16 samples while gliding.
                 Discrete changes (type, slope, channel, enable) fade the band
                 out, swap, and fade it back in (click-free).
      Linear   - the cascade's zero-phase magnitude responses are combined into
                 a 2x2 L/R filter matrix and realised as Kaiser-windowed
                 (beta 8) FIR kernels of N/2 + 1 taps, convolved by FFT
                 overlap-add (hop N/4). Latency N/2 (4096 at 44.1/48 kHz).
      Natural  - each stage response is converted to minimum phase with the
                 real-cepstrum method (fold the cepstrum onto positive quefrency
                 and exponentiate), combined into the same matrix, truncated to
                 N/2 taps with a raised-cosine tail. Latency N/4 (2048).
      N = 8192 at <= 50 kHz, 16384 at <= 100 kHz, 32768 above.

    Thread safety: FIR kernels are designed on a background thread that polls
    the APVTS atomics every 30 ms and publishes kernel spectra to the audio
    thread through lock-free triple buffers. The biquad path is updated on the
    audio thread in updateParameters()/processBlock() (pure arithmetic, RT-safe).
*/
class CorrectiveEQ final : public DSPModuleBase
{
public:
    using FilterType = choices::CeqFilterType;
    using PhaseMode  = choices::PhaseMode;

    struct BandSettings
    {
        double freq   = 1000.0;
        double gainDb = 0.0;
        double q      = 0.7;
        int type      = 0;   // CeqFilterType
        int slopeIndex = choices::ceqDefaultSlopeIndex;
        bool enabled  = true;
        int channel   = 0;   // ChannelMode

        bool operator== (const BandSettings& o) const noexcept
        {
            return freq == o.freq && gainDb == o.gainDb && q == o.q && type == o.type
                && slopeIndex == o.slopeIndex && enabled == o.enabled && channel == o.channel;
        }
        bool operator!= (const BandSettings& o) const noexcept { return ! (*this == o); }
    };

    using AllBands = std::array<BandSettings, kNumCeqBands>;

    static constexpr int kMaxSections = Biquad::kMaxCascadeSections;

    explicit CorrectiveEQ (juce::AudioProcessorValueTreeState& state);
    ~CorrectiveEQ() override;

    ModuleId getId() const noexcept override { return ModuleId::CorrectiveEQ; }
    void updateParameters() noexcept override;
    void processBlock (double* L, double* R, int numSamples) noexcept override;

    /** Biquad sections realising one band; returns the section count (0..8). */
    static int computeBandSections (const BandSettings& band, double sampleRate, BiquadCoeffs* out) noexcept;

    /** Linear magnitude of one band at freq (ignores the enabled flag). */
    static double computeBandMagnitude (const BandSettings& band, double freq, double sampleRate) noexcept;

    /** Whether the band type uses the gain parameter. */
    static bool typeUsesGain (int type) noexcept;

    /** Current (smoothed) applied gain of a band in dB, for display. Thread-safe. */
    double getBandGainDb (int band) const noexcept;

    /** Phase mode in effect (low-latency mode forces Minimum). Thread-safe. */
    PhaseMode getEffectivePhaseMode() const noexcept
    {
        return static_cast<PhaseMode> (effectiveMode.load (std::memory_order_relaxed));
    }

    /** Reads the current parameters and designs/publishes the FIR kernels
        synchronously. Non-RT; used by prepare() and offline/test code. */
    void designFirKernelsNow();

    /** FFT size of the FIR engines for a sample rate. */
    static int firSizeForSampleRate (double sampleRate) noexcept;

protected:
    void prepareModule (double sampleRate, int maxBlockSize) override;
    void resetModule() noexcept override;

private:
    //==========================================================================
    /** Stereo 2x2 matrix FIR convolver, FFT overlap-add, arbitrary kernels.
        y_L = h_LL * x_L + h_LR * x_R,  y_R = h_RL * x_L + h_RR * x_R */
    class MatrixConvolver
    {
    public:
        struct Kernel
        {
            std::array<std::vector<float>, 4> spectra;   // LL, LR, RL, RR: (N/2 + 1) complex, interleaved
            bool diagonal = true;
        };

        void prepare (int fftSize);
        void reset() noexcept;
        void process (double* L, double* R, int numSamples) noexcept;

        /** Writer side (single writer at a time). kernels are time-domain, length <= N - N/4 + 1. */
        void publish (const std::array<std::vector<double>, 4>& timeKernels, bool diagonal);

        int getFFTSize() const noexcept { return fftSize; }
        int getHopSize() const noexcept { return hopSize; }

    private:
        void acquire() noexcept;
        void processHop() noexcept;

        int fftSize = 0, hopSize = 0, numBins = 0;
        std::unique_ptr<juce::dsp::FFT> fft, writerFft;
        std::vector<float> workL, workR, outWorkL, outWorkR;
        std::array<std::vector<double>, 2> inFifo, outFifo, overlap;
        int fifoPos = 0;

        static constexpr int dirtyFlag = 4;
        std::array<Kernel, 3> kernels;
        std::atomic<int> middle { 1 };
        int front = 0;
        int back = 2;
        std::vector<float> writerWork;
    };

    //==========================================================================
    class DesignThread final : public juce::Thread
    {
    public:
        explicit DesignThread (CorrectiveEQ& o) : juce::Thread ("Meridian CEQ FIR Designer"), owner (o) {}
        void run() override;

    private:
        CorrectiveEQ& owner;
    };

    struct BandState
    {
        BandSettings active;          // discrete settings currently realised
        SmoothParam logFreq, gain, q; // continuous parameters (log2 Hz, dB, Q)
        SmoothParam amount;           // 0..1 fade (enable / discrete swaps)
        BiquadCoeffs coeffs[kMaxSections];
        Biquad filters[2][kMaxSections];
        int numSections = 0;
        bool swapPending = false;
    };

    AllBands readSettings() const noexcept;
    PhaseMode readEffectiveMode() const noexcept;
    void designAndPublish (const AllBands& bands);   // writer side, holds designMutex
    void designerTick();

    void setBandTargets (int b, const BandSettings& s, bool snap) noexcept;
    void recomputeBand (int b) noexcept;
    void advanceBands (int n, double* amountOut) noexcept;   // amountOut: [band][n] or nullptr
    void processMinimumPhase (double* L, double* R, int numSamples) noexcept;
    void resetBiquads() noexcept;

    // ---- Parameters ----------------------------------------------------------
    struct BandParams
    {
        std::atomic<float>* freq = nullptr;
        std::atomic<float>* gain = nullptr;
        std::atomic<float>* q = nullptr;
        std::atomic<float>* type = nullptr;
        std::atomic<float>* slope = nullptr;
        std::atomic<float>* enabled = nullptr;
        std::atomic<float>* channel = nullptr;
    };

    std::array<BandParams, kNumCeqBands> params;
    std::atomic<float>* phaseModeParam = nullptr;
    std::atomic<float>* lowLatencyParam = nullptr;

    // ---- Minimum-phase path ----------------------------------------------------
    std::array<BandState, kNumCeqBands> bands;
    std::array<BandSettings, kNumCeqBands> requested;
    std::array<std::atomic<double>, kNumCeqBands> bandGainDisplay {};
    static constexpr int kControlInterval = 16;
    std::vector<double> amountBuffer;   // kNumCeqBands * kControlInterval
    std::vector<double> segTemp;

    // ---- FIR paths ---------------------------------------------------------------
    MatrixConvolver linearEngine, naturalEngine;
    int firSize = 8192;
    int currentMode = 0;
    std::atomic<int> effectiveMode { 0 };

    std::mutex designMutex;
    AllBands lastDesigned {};
    bool hasDesign = false;
    DesignThread designThread { *this };
};

} // namespace meridian
