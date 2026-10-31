#pragma once

#include "common/AnalysisTypes.h"
#include "common/Biquad.h"
#include "common/TruePeakDetector.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace meridian
{

/**
    ITU-R BS.1770-4 / EBU R128 / EBU Tech 3342 loudness and peak metering.

    K-weighting: BS.1770-4 pre-filter (high shelf) + RLB high-pass, per
    channel. At 48 kHz the exact published coefficients are used; at other
    rates they are derived from the analog prototype parameters with the
    bilinear transform pre-warped at each corner (f0 = 1681.97 Hz,
    G = +4.00 dB, Q = 0.7072; f0 = 38.135 Hz, Q = 0.5003), which reproduces
    the 48 kHz table to ~1e-9.

    Channel summation follows BS.1770 (sum of per-channel mean squares,
    G = 1.0 for L and R): L = -0.691 + 10 log10(z_L + z_R). (The brief's
    0.5 * (z_L + z_R) would read 3.01 dB low: a -23 dBFS 1 kHz sine on both
    channels must read -23.0 LUFS.)

    Energies are accumulated per 100 ms sub-block:
      Momentary   400 ms (last 4 sub-blocks), updated every 100 ms
      Short-term  3 s (last 30 sub-blocks), updated every 100 ms
      Integrated  gating blocks of 400 ms with 75 % overlap (= every momentary
                  value); absolute gate -70 LUFS, relative gate -10 LU below
                  the mean of the blocks above the absolute gate. Blocks are
                  stored in a 0.01 LU histogram (-70 .. +10 LUFS) that also
                  holds the exact energy sum per bin, so memory is constant
                  for any programme length and the gated mean is exact except
                  for the 0.01 LU quantisation of the relative-gate edge.
      LRA         EBU Tech 3342: short-term values (3 s window) sampled every
                  100 ms (the brief said 1 s; Tech 3342 requires >= 10 Hz),
                  absolute gate -70 LUFS, relative gate -20 LU,
                  LRA = P95 - P10 from a 0.1 LU histogram, recomputed every 1 s.
      True peak   TruePeakDetector at 4x (BS.1770-4 Annex 2), max of both channels,
                  held until resetTruePeak() / resetIntegrated().
      Sample peak max |x| held until reset.
      PLR         truePeak - integrated.
      PSR         max true peak over the last 3 s - short-term loudness
                  (MeterData's definition; the industry PSR).
      Correlation exponential 300 ms averages: sum(LR) / sqrt(sum(L^2) sum(R^2)),
                  full band and below 150 Hz (2nd-order Butterworth LPF).
                  Silence reads +1.

    Threading: process() runs on the audio thread and publishes every value
    through std::atomic<float> (relaxed); fillMeterData() may be called from
    any thread. resetIntegrated()/resetTruePeak() only raise atomic flags that
    process() services, so they are safe from any thread. No allocation or
    locking after prepare().
*/
class MeteringEngine
{
public:
    void prepare (double sampleRate, int maxBlockSize);
    void reset() noexcept;
    void resetIntegrated() noexcept   { integratedResetFlag.store (true, std::memory_order_release); }
    void resetTruePeak() noexcept     { peakResetFlag.store (true, std::memory_order_release); }

    void process (const double* left, const double* right, int numSamples) noexcept;
    void fillMeterData (MeterData& out) const noexcept;

    /** K-weighting coefficients for a sample rate: [0] pre-filter, [1] RLB high-pass. */
    static std::array<BiquadCoeffs, 2> kWeighting (double sampleRate) noexcept;

    static constexpr double kAbsoluteGate = -70.0;
    static constexpr double kHistMax = 10.0;
    static constexpr double kIntegratedBinLu = 0.01;
    static constexpr double kLraBinLu = 0.1;
    static constexpr int kSubBlocks = 30;

private:
    static double energyToLufs (double e) noexcept;
    void clearIntegrated() noexcept;
    void clearPeaks() noexcept;
    void finishSubBlock() noexcept;
    void computeIntegrated() noexcept;
    void computeLra() noexcept;

    double fs = 48000.0;
    int subBlockLength = 4800;
    int subBlockCount = 0;
    double subSum = 0.0;
    double subPeak = 0.0;

    std::array<Biquad, 2> kwL, kwR;
    std::array<double, kSubBlocks> subEnergy {};
    std::array<double, kSubBlocks> subTruePeak {};
    int subWrite = 0, subFilled = 0;

    std::vector<double> intEnergy;
    std::vector<std::uint32_t> intCount;
    double intTotalEnergy = 0.0;
    std::uint64_t intTotalCount = 0;

    std::vector<double> lraEnergy;
    std::vector<std::uint32_t> lraCount;
    int lraCounter = 0;

    TruePeakDetector tpDetector;
    double truePeakHold = 0.0, samplePeakHold = 0.0;

    Biquad lowL, lowR;
    double corrCoeff = 0.0;
    double sLR = 0.0, sLL = 0.0, sRR = 0.0;
    double lLR = 0.0, lLL = 0.0, lRR = 0.0;

    std::atomic<bool> integratedResetFlag { false }, peakResetFlag { false };

    std::atomic<float> momentary { MeterData::kSilenceDb }, shortTerm { MeterData::kSilenceDb },
                       integrated { MeterData::kSilenceDb }, lra { 0.0f },
                       truePeakDb { MeterData::kSilenceDb }, samplePeakDb { MeterData::kSilenceDb },
                       plr { 0.0f }, psr { 0.0f }, correlation { 1.0f }, correlationLow { 1.0f };
};

} // namespace meridian
