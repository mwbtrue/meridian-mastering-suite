#pragma once

#include "DSPModuleBase.h"
#include "GlueCompressor.h"
#include "common/LinearPhaseCrossover.h"

#include <array>
#include <atomic>
#include <vector>

namespace meridian
{

/**
    Four-band dynamics processor (Low, LowMid, HighMid, Air).

    Processing per block:
      1. Optional M/S encode (setMidSideProcessing): L <- M, R <- S.
      2. LinearPhaseCrossover::processBlock splits into 4 band pairs
         (linear-phase Linkwitz-Riley-magnitude FIR crossovers at
         mbd_xover0..2_freq; the bands sum back exactly flat).
      3. Per band, per sample, feed-forward VCA dynamics (VcaCompressorCore,
         identical algorithm to GlueCompressor): downward compression
         (ratio, threshold, knee, attack, release) plus upward expansion
         (upward_thresh, upward_ratio), then makeup gain.
           - L/R mode: one stereo-linked detector per band, same gain on L and R.
           - M/S mode: independent detectors / gains for M and S.
         Band enable crossfades over 20 ms; disabled bands pass at unity.
      4. LinearPhaseCrossover::sumBands recombines; optional M/S decode.

    Latency = crossover latency (constant for a given sample rate). The
    crossover stays linear-phase in low-latency mode because band splitting
    without phase-coherent recombination would colour the sum.

    Per-band gain reduction is published through getBandGainReductionDb().
    All buffers are pre-allocated in prepare().
*/
class MultibandDynamics final : public DSPModuleBase
{
public:
    static constexpr int kNumBands = kNumMbdBands;
    static constexpr int kNumCrossovers = kNumMbdXovers;

    explicit MultibandDynamics (juce::AudioProcessorValueTreeState& state);

    ModuleId getId() const noexcept override { return ModuleId::MultibandDynamics; }
    void updateParameters() noexcept override;
    void processBlock (double* L, double* R, int numSamples) noexcept override;

    /** Downward gain reduction of a band in positive dB (thread-safe). */
    double getBandGainReductionDb (int band) const noexcept
    {
        return bandGr[(size_t) juce::jlimit (0, kNumBands - 1, band)].load (std::memory_order_relaxed);
    }

    /** true: per-band processing on Mid / Side instead of linked L/R. */
    void setMidSideProcessing (bool shouldUseMidSide) noexcept { midSideRequested.store (shouldUseMidSide); }
    bool isMidSideProcessing() const noexcept { return midSideRequested.load(); }

protected:
    void prepareModule (double sampleRate, int maxBlockSize) override;
    void resetModule() noexcept override;

private:
    struct Band
    {
        std::atomic<float>* ratioParam = nullptr;
        std::atomic<float>* thresholdParam = nullptr;
        std::atomic<float>* attackParam = nullptr;
        std::atomic<float>* releaseParam = nullptr;
        std::atomic<float>* kneeParam = nullptr;
        std::atomic<float>* makeupParam = nullptr;
        std::atomic<float>* upThreshParam = nullptr;
        std::atomic<float>* upRatioParam = nullptr;
        std::atomic<float>* enabledParam = nullptr;

        VcaCompressorCore core[2];
        SmoothParam makeup;    // linear
        SmoothParam enable;    // 0..1
    };

    void readCrossoverFrequencies (double* out) const noexcept;

    std::array<Band, kNumBands> bands;
    std::array<std::atomic<float>*, kNumCrossovers> xoverParams {};
    std::array<std::atomic<double>, kNumBands> bandGr {};

    LinearPhaseCrossover crossover;
    std::vector<double> bandStorage;   // 2 * kNumBands * maxBlock
    std::array<double*, kNumBands> bandL {}, bandR {};

    std::atomic<bool> midSideRequested { false };
    bool midSideActive = false;
};

} // namespace meridian
