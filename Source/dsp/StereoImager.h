#pragma once

#include "DSPModuleBase.h"
#include "common/LinearPhaseCrossover.h"
#include "common/LinkwitzRileyBank.h"

#include <array>
#include <atomic>
#include <vector>

namespace meridian
{

/**
    4-band M/S stereo width control with a per-band correlation guard.

    Split: LinearPhaseCrossover (3 crossovers, default 120 / 1000 / 8000 Hz,
    bands sum to the exactly delayed input). In Low-Latency mode
    (low_latency_mode) the minimum-phase LR4 splitter (LinkwitzRileyBank) is
    used instead: 0 samples latency, flat-magnitude all-pass sum.

    Per band: M = (L + R)/2, S = (L - R)/2, S *= width/100 * guard, L = M + S,
    R = M - S. Width 0 % = mono, 100 % = unchanged, 200 % = double side.
    A disabled band passes unchanged (width 100 %, no guard).

    Correlation guard (img_corr_guard): the standard L/R phase correlation of
    a band, r = sum(L R) / sqrt(sum L^2 sum R^2), reduces with L = M + S,
    R = M - S over a common window to (E_M - E_S') / (E_M + E_S') with
    E_S' = (width * guard)^2 E_S. r < 0 exactly when the widened side
    energy exceeds the mid energy, so the guard target is
        guard = min(1, sqrt(E_M / (width^2 E_S)))
    which places the band at r = 0 at worst. (The M*S cross-term formula in
    the brief measures L/R balance, not phase correlation; the L/R
    correlation above is what correlation meters display and what mono
    compatibility depends on.) Energies are 50 ms sliding sums built from
    64-sample block sums in a pre-allocated ring; the target is updated every
    64 samples and the applied multiplier follows it with a 20 ms one-pole,
    so the guard is click-free (and r can dip transiently below 0 by the
    smoother's lag only). Per-band output correlation is exposed through
    atomics for metering.

    Latency: LinearPhaseCrossover::getLatencySamples() (1280 at 44.1/48 kHz),
    0 in Low-Latency mode.
*/
class StereoImager final : public DSPModuleBase
{
public:
    explicit StereoImager (juce::AudioProcessorValueTreeState& state);

    ModuleId getId() const noexcept override { return ModuleId::StereoImager; }
    void updateParameters() noexcept override;
    void processBlock (double* L, double* R, int numSamples) noexcept override;

    static constexpr int kBands = kNumImgBands;
    static constexpr int kGuardInterval = 64;
    static constexpr double kWindowMs = 50.0;
    static constexpr double kGuardSmoothingMs = 20.0;

    /** Per-band output L/R correlation (-1..+1), thread-safe. */
    float getBandCorrelation (int band) const noexcept
    {
        return band >= 0 && band < kBands ? bandCorrelation[(size_t) band].load (std::memory_order_relaxed) : 1.0f;
    }

    /** Per-band guard multiplier on S (1 = no reduction), thread-safe. */
    float getBandGuardGain (int band) const noexcept
    {
        return band >= 0 && band < kBands ? bandGuard[(size_t) band].load (std::memory_order_relaxed) : 1.0f;
    }

protected:
    void prepareModule (double sampleRate, int maxBlockSize) override;
    void resetModule() noexcept override;

private:
    struct Band
    {
        double width = 1.0;
        bool enabled = true;
        double guard = 1.0;          // smoothed multiplier
        double guardTarget = 1.0;
        double accM = 0.0, accS = 0.0;
        std::vector<double> ringM, ringS;
        double sumM = 0.0, sumS = 0.0;
        int ringPos = 0;
    };

    void processBand (int b, double* bl, double* br, int n) noexcept;
    void updateGuard (Band& band, int index) noexcept;

    LinearPhaseCrossover linearSplit;
    LinkwitzRileyBank minSplit;
    bool lowLatency = false;
    bool corrGuard = true;

    std::array<Band, kBands> bands;
    std::array<std::vector<double>, kBands> bufL, bufR;
    std::vector<double> inL, inR;
    int guardCounter = 0;
    int ringBlocks = 1;
    double guardCoeff = 0.0;

    std::array<std::atomic<float>*, kNumImgXovers> xoverParams {};
    std::array<std::atomic<float>*, kBands> widthParams {}, enabledParams {};
    std::atomic<float>* guardParam = nullptr;
    std::atomic<float>* lowLatencyParam = nullptr;

    std::array<std::atomic<float>, kBands> bandCorrelation {};
    std::array<std::atomic<float>, kBands> bandGuard {};
};

} // namespace meridian
