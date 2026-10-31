#pragma once

#include <juce_dsp/juce_dsp.h>

#include "common/AnalysisTypes.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace meridian
{

/**
    Real-time magnitude spectrum for the display.

    2048-point FFT (juce::dsp::FFT, order 11), Hann window, hop 512 (75 %
    overlap). Each frame carries four 1025-bin curves (see SpectrumFrame):
    stereo power average, Mid, Side and a decaying peak hold (tau = 2 s).
    Scaling: 20 log10(4 |X[k]| / N), so a full-scale sine reads 0 dBFS
    (4 = 2 for the one-sided spectrum x 2 for the Hann coherent gain of 0.5);
    floor -120 dB.

    Threading (pull model):
      pushSamples()  producer: copies into a 16384-sample stereo ring and
                     publishes the free-running write counter (release).
      getNextFrame() consumer: if a full hop of new samples is available,
                     snapshots the most recent 2048 samples ending at the next
                     hop boundary, runs the FFTs and returns true. If the
                     consumer has fallen more than the ring's slack behind,
                     it skips ahead to the newest data instead of reading
                     overwritten samples.
    The processor calls getNextFrame() right after pushSamples() and forwards
    frames to the UI through its LockFreeQueue<SpectrumFrame, 8>, so nothing
    ever blocks. JUCE's FFT for N = 2048 uses stack scratch space and the
    work buffers are pre-allocated in prepare(): no allocation per frame.
*/
class SpectrumAnalyzer
{
public:
    static constexpr int kFftOrder = SpectrumFrame::fftOrder;
    static constexpr int kFftSize  = SpectrumFrame::fftSize;
    static constexpr int kNumBins  = SpectrumFrame::numBins;
    static constexpr int kHop      = kFftSize / 4;
    static constexpr int kRingSize = 16384;
    static constexpr double kPeakHoldTau = 2.0;

    SpectrumAnalyzer();

    void prepare (double sampleRate, int maxBlockSize);
    void reset() noexcept;

    void pushSamples (const double* left, const double* right, int numSamples) noexcept;
    bool getNextFrame (SpectrumFrame& frame) noexcept;

private:
    void transform (const float* input, float* outDb) noexcept;

    juce::dsp::FFT fft { kFftOrder };
    std::array<float, kFftSize> window {};
    std::vector<float> ringL, ringR;
    std::atomic<std::uint64_t> writeCount { 0 };
    std::uint64_t nextFrameEnd = kFftSize;

    std::vector<float> work, frameL, frameR, frameM, frameS;
    std::array<float, kNumBins> dbL {}, dbR {}, peak {};
    double sampleRate = 48000.0;
    float peakDecayDbPerFrame = 0.0f;
};

} // namespace meridian
