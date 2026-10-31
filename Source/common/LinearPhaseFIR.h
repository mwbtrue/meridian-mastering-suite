#pragma once

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace meridian
{

/**
    Stereo linear-phase FIR filter driven by an arbitrary magnitude response,
    implemented as FFT overlap-add convolution.

    Design (N = fftSize, power of two, >= 64):
      - The requested magnitude response (N/2 + 1 bins, linear gain, bin k at
        k * fs / N) is treated as a zero-phase spectrum and inverse transformed.
      - The zero-phase impulse is truncated to L = N/2 + 1 taps around t = 0,
        Kaiser-windowed (beta configurable, default 8.0) and shifted to a causal
        kernel centred at N/4.
      - Audio is processed in hops of B = N/4 samples; each hop is zero-padded
        to N and convolved in the frequency domain (B + L - 1 = 3N/4 <= N, so
        there is no circular aliasing).
      - Total latency = B (hop buffering) + N/4 (kernel centre) = N/2 samples.

    Thread safety:
      - setMagnitudeResponse() / setKaiserBeta() may be called from any single
        non-audio thread at a time (internally serialised with a mutex that the
        audio thread never touches). The new kernel is published through a
        lock-free triple buffer and picked up by the audio thread at the next
        hop boundary.
      - processBlock() / reset() are audio-thread only, allocation-free and
        lock-free.

    FFTs use juce::dsp::FFT (single precision). Input/output and all buffers
    exposed to the caller are double precision.
*/
class LinearPhaseFIR
{
public:
    static constexpr int defaultFFTSize = 8192;
    static constexpr double defaultKaiserBeta = 8.0;

    explicit LinearPhaseFIR (int fftSize = defaultFFTSize, double kaiserBeta = defaultKaiserBeta);
    ~LinearPhaseFIR() = default;

    LinearPhaseFIR (const LinearPhaseFIR&) = delete;
    LinearPhaseFIR& operator= (const LinearPhaseFIR&) = delete;

    /** Clears all audio history (audio thread or while stopped). */
    void reset() noexcept;

    /** In-place stereo processing of any block length. Audio thread only. */
    void processBlock (double* left, double* right, int numSamples) noexcept;

    /** Designs and publishes a new kernel. magnitudes holds sourceFFTSize/2 + 1
        linear gains. If sourceFFTSize differs from the engine's FFT size, the
        response is linearly resampled onto the engine's bin grid. Non-RT. */
    void setMagnitudeResponse (const std::vector<double>& magnitudes, int sourceFFTSize);

    /** Publishes a flat (unity) kernel: the filter becomes a pure N/2 delay. Non-RT. */
    void setUnityResponse();

    /** Changes the Kaiser beta and re-designs the current kernel. Non-RT. */
    void setKaiserBeta (double newBeta);

    int getLatencySamples() const noexcept { return fftSize / 2; }
    int getFFTSize() const noexcept        { return fftSize; }
    int getNumTaps() const noexcept        { return numTaps; }
    int getHopSize() const noexcept        { return hopSize; }
    double getKaiserBeta() const noexcept  { return kaiserBeta; }

    /** Zeroth-order modified Bessel function of the first kind (series expansion). */
    static double besselI0 (double x) noexcept;

    /** Fills window[0..length) with a symmetric Kaiser window. */
    static void makeKaiserWindow (double* window, int length, double beta) noexcept;

private:
    void designKernel (const std::vector<double>& binMagnitudes, std::vector<float>& kernelOut);
    void publishKernel (const std::vector<float>& kernel);
    void acquireLatestKernel() noexcept;
    void processHop() noexcept;

    const int fftSize;
    const int fftOrder;
    const int hopSize;   // N / 4
    const int numTaps;   // N / 2 + 1
    const int numBins;   // N / 2 + 1
    double kaiserBeta;

    // ---- Audio-thread state ----
    std::unique_ptr<juce::dsp::FFT> audioFFT;
    std::vector<float> fftWork;                     // 2N floats
    std::array<std::vector<double>, 2> inputFifo;   // B samples per channel
    std::array<std::vector<double>, 2> outputFifo;  // B samples per channel
    std::array<std::vector<double>, 2> overlap;     // N samples per channel
    int fifoPosition = 0;

    // ---- Kernel triple buffer: 3 x (N/2 + 1) complex bins, interleaved re/im ----
    static constexpr int dirtyFlag = 4;
    std::array<std::vector<float>, 3> kernels;
    std::atomic<int> middleIndex { 1 };   // index (+ dirtyFlag when a new kernel is waiting)
    int frontIndex = 0;                   // owned by the audio thread
    int backIndex = 2;                    // owned by the writer (under writerMutex)

    // ---- Writer-side state ----
    std::mutex writerMutex;
    std::unique_ptr<juce::dsp::FFT> designFFT;
    std::vector<float> designWork;        // 2N floats
    std::vector<double> window;           // L taps
    std::vector<double> lastBinMagnitudes;
};

} // namespace meridian
