#pragma once

#include <juce_dsp/juce_dsp.h>

#include "DelayLine.h"
#include "RealFFT.h"

#include <array>
#include <memory>
#include <vector>

namespace meridian
{

/**
    Stereo linear-phase crossover bank (2..4 bands), Linkwitz-Riley magnitude
    behaviour (-6 dB at each crossover, bands sum to exactly unity).

    Design:
      - One Kaiser-windowed (beta = 6) sinc low-pass per crossover frequency,
        length 2D + 1 taps (type-I, integer group delay D), normalised to unity
        DC gain. A windowed sinc is -6 dB at its cutoff, i.e. LR-style.
      - Band kernels are differences of adjacent low-passes:
            band0 = LP0, band k = LP(k) - LP(k-1)
        The top band is the D-delayed input minus all lower bands, so the band
        sum is mathematically identical to the delayed input (flat to within
        floating-point rounding, far inside +/-0.05 dB).

    Frequency resolution vs. latency:
      A crossover's transition width is ~ 5.8 * fs / taps for beta = 6. A
      64-tap kernel would give a ~4 kHz wide transition at 48 kHz, which cannot
      separate a 120 Hz or 800 Hz crossover at all. D therefore scales with the
      sample rate (1024 at <= 50 kHz, 2048 at <= 100 kHz, 4096 above) to keep a
      ~130 Hz transition width at every rate.

    Convolution:
      Uniformly partitioned overlap-save (partition P = 256, FFT 2P) using
      meridian::RealFFT (split re/im spectra). The input spectrum delay line is shared by all bands of a
      channel. Latency = P + D samples (1280 at 44.1/48 kHz).

    Frequency changes:
      setCrossoverFrequencies(const double*, int) is real-time safe and must be
      called from the audio thread (or while stopped). Kernels are redesigned
      at the next partition boundary into pre-allocated storage and the output
      is crossfaded from the old to the new kernels over one partition.

    All buffers are allocated in prepare(); processBlock() is allocation- and
    lock-free.
*/
class LinearPhaseCrossover
{
public:
    static constexpr int kMaxBands      = 4;
    static constexpr int kMaxCrossovers = kMaxBands - 1;
    static constexpr int kPartitionSize = 256;
    static constexpr double kKaiserBeta = 6.0;

    LinearPhaseCrossover() = default;

    LinearPhaseCrossover (const LinearPhaseCrossover&) = delete;
    LinearPhaseCrossover& operator= (const LinearPhaseCrossover&) = delete;

    /** Allocates everything for the given rate. Non-RT. */
    void prepare (double sampleRate, int maxBlockSize);

    /** Clears audio history; applies any pending kernel design immediately. */
    void reset() noexcept;

    /** Real-time safe. freqs must be ascending; values are clamped to
        [20 Hz, 0.45 fs] and forced at least 1/6 octave apart. numFreqs = numBands - 1. */
    void setCrossoverFrequencies (const double* freqs, int numFreqs) noexcept;

    /** Convenience overload (non-RT): re-prepares if sampleRate differs from the
        prepared rate, then sets the frequencies and designs the kernels at once. */
    void setCrossoverFrequencies (const std::vector<double>& freqs, double sampleRate);

    /** Splits the stereo input into numBands band buffers (each n samples).
        Inputs may alias neither band buffer. numBands must equal the number of
        crossover frequencies + 1 (otherwise it is clamped to the designed count). */
    void processBlock (const double* inL, const double* inR,
                       double* const* bandL, double* const* bandR,
                       int numSamples, int numBands) noexcept;

    /** Sums band buffers back into outL/outR. */
    static void sumBands (const double* const* bandL, const double* const* bandR,
                          double* outL, double* outR, int numSamples, int numBands) noexcept;

    int getLatencySamples() const noexcept { return kPartitionSize + halfLength; }
    int getNumTaps() const noexcept        { return 2 * halfLength + 1; }
    int getNumBands() const noexcept       { return designedBands; }
    double getSampleRate() const noexcept  { return fs; }
    bool isPrepared() const noexcept       { return prepared; }

    /** Group delay of the kernels alone (D), without partition buffering. */
    int getKernelDelay() const noexcept    { return halfLength; }

    static int halfLengthForSampleRate (double sampleRate) noexcept;

private:
    using Spectrum = std::vector<float>;   // (P + 1) complex bins, interleaved re/im

    struct KernelSet
    {
        // [band 0..kMaxCrossovers-1][partition] -> spectrum
        std::array<std::vector<Spectrum>, kMaxCrossovers> parts;
        int numBands = 1;
    };

    void designInto (KernelSet& set) noexcept;
    void designLowPass (double cutoff, double* out) noexcept;
    void processPartition() noexcept;
    void convolveBand (int ch, const KernelSet& set, int band, double* out) noexcept;

    double fs = 48000.0;
    int halfLength = 1024;
    int numTaps = 2049;
    int numPartitions = 9;
    bool prepared = false;

    // Requested / designed crossover state
    std::array<double, kMaxCrossovers> requestedFreqs { 120.0, 800.0, 5000.0 };
    int requestedBands = kMaxBands;
    bool designPending = true;
    int designedBands = kMaxBands;

    // Kernel storage: active, previous (for crossfade), spare (design target)
    std::array<KernelSet, 3> kernelSets;
    int activeSet = 0, previousSet = 1, spareSet = 2;
    bool fading = false;

    // Design scratch (double precision)
    std::vector<double> window, lpPrev, lpCur, bandKernel;

    // FFT
    RealFFT fft;
    std::vector<float> fftWork;      // 2P time-domain floats
    std::vector<float> accum;        // (P + 1) complex

    // Per-channel state
    struct Channel
    {
        std::vector<float> history;              // 2P samples (previous + current partition)
        std::vector<Spectrum> fdl;               // frequency-domain delay line, numPartitions entries
        std::vector<double> inFifo, delayFifo;   // P samples
        std::array<std::vector<double>, kMaxBands> outFifo;   // P samples per band
        DelayLine delay;
    };

    std::array<Channel, 2> channels;
    std::vector<double> tempOld, tempNew;
    int fdlPos = 0;
    int fifoPos = 0;
};

} // namespace meridian
