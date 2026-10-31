#pragma once

#include "DSPModuleBase.h"
#include "common/Biquad.h"
#include "common/DelayLine.h"

#include <atomic>

namespace meridian
{

/**
    Feed-forward VCA compressor core shared by GlueCompressor and
    MultibandDynamics. Works on a detector power value (mean square) per
    sample and returns the gain to apply in dB.

      1. RMS detector: running mean of squares over a short symmetric window
         (10 ms) followed by attack / release ballistics on the power:
             p  += c10 * (x^2 - p)
             ms += a * (p - ms),  a = attack coeff if p > ms else release coeff
         The short pre-average keeps asymmetric ballistics from turning the
         detector into a peak detector, so steady signals read their true RMS.
      2. Optional auto-release: the detector level (dB) is tracked over a
         200 ms window (one-pole mean and variance). Dense, steady material
         (low standard deviation) and deeper gain reduction shorten the
         release:
             scale = clamp(stdDb / 6 dB, 0.2, 1) / (1 + GR / 12 dB)
             release_eff = release * scale   (floored at 5 ms)
         The release coefficient is refreshed every 32 samples.
      3. Gain computer, dB domain, soft knee of width W around threshold T:
             2(x - T) < -W        : y = x
             2|x - T| <= W        : y = x + (1/R - 1)(x - T + W/2)^2 / (2W)
             2(x - T) > W         : y = T + (x - T) / R
         downward gain = y - x  (<= 0)
      4. Optional upward expansion: below upward threshold Tu,
             up gain = (Tu - x)(1 - 1/Ru), capped at +12 dB and faded out
             between -90 and -80 dBFS so silence / noise floor is never lifted.
*/
class VcaCompressorCore
{
public:
    static constexpr double kAutoReleaseWindowMs = 200.0;
    static constexpr int kAutoReleaseInterval = 32;
    static constexpr double kMaxUpwardGainDb = 12.0;
    static constexpr double kRmsWindowMs = 10.0;

    void prepare (double sampleRate) noexcept;
    void reset() noexcept;

    void setParameters (double ratio, double thresholdDb, double kneeDb,
                        double attackMs, double releaseMs, bool autoRelease) noexcept;

    /** Upward expansion below thresholdDb; ratio 1 disables it. */
    void setUpwardExpansion (double thresholdDb, double ratio) noexcept;

    /** Feeds one detector power value (mean square) and returns the total gain in dB. */
    double processPower (double power) noexcept;

    /** Downward gain reduction of the last sample in positive dB. */
    double getGainReductionDb() const noexcept { return lastGrDb; }

    /** Static downward gain (dB, <= 0) for a detector level xDb. */
    static double downwardGainDb (double xDb, double thresholdDb, double ratio, double kneeDb) noexcept;

    /** Static upward gain (dB, >= 0) for a detector level xDb. */
    static double upwardGainDb (double xDb, double thresholdDb, double ratio) noexcept;

private:
    static double msToCoeff (double ms, double fs) noexcept;
    void updateAutoRelease() noexcept;

    double fs = 48000.0;
    double ratio = 2.0, thresholdDb = -18.0, kneeDb = 4.0;
    double attackCoeff = 1.0, releaseCoeff = 1.0, releaseMs = 200.0;
    bool autoRelease = false;
    double upThresholdDb = -50.0, upRatio = 1.0;

    double rmsCoeff = 1.0;
    double rmsPower = 0.0;
    double meanSquare = 0.0;
    double activeReleaseCoeff = 1.0;
    double windowCoeff = 1.0;
    double levelMean = -120.0, levelVar = 0.0;
    int autoCounter = 0;
    double lastGrDb = 0.0;
};

/**
    SSL-style stereo bus ("glue") compressor, feed-forward VCA topology,
    per-sample double-precision processing.

      - Sidechain source (glue_sc_channel):
          L+R : stereo-linked power (HPF(L)^2 + HPF(R)^2) / 2 - reacts to
                side content as well as centre content
          Mid : HPF((L+R)/2)^2
        The sidechain HPF is a Butterworth biquad at glue_sc_hpf.
      - Detector / gain computer / auto-release: VcaCompressorCore.
      - The same gain is applied to both channels (100 % stereo link).
      - Makeup gain (smoothed, 20 ms) applies to the compressed path; mix
        (smoothed) blends dry and compressed signal in linear amplitude.
      - Lookahead (glue_lookahead, 0-10 ms): the audio path (dry and wet) is
        delayed while the sidechain sees the undelayed input; the delay is
        reported as module latency.
      - GR meter: getGainReductionDbValue() (atomic) and ModuleBase GR.
*/
class GlueCompressor final : public DSPModuleBase
{
public:
    static constexpr double kMaxLookaheadMs = 10.0;

    explicit GlueCompressor (juce::AudioProcessorValueTreeState& state);

    ModuleId getId() const noexcept override { return ModuleId::GlueCompressor; }
    void updateParameters() noexcept override;
    void processBlock (double* L, double* R, int numSamples) noexcept override;

    /** Current gain reduction in positive dB (thread-safe). */
    double getGainReductionDbValue() const noexcept { return gainReductionDb.load (std::memory_order_relaxed); }

protected:
    void prepareModule (double sampleRate, int maxBlockSize) override;
    void resetModule() noexcept override;

private:
    void updateSidechainFilter (double freq) noexcept;

    std::atomic<float>* ratioParam = nullptr;
    std::atomic<float>* thresholdParam = nullptr;
    std::atomic<float>* attackParam = nullptr;
    std::atomic<float>* releaseParam = nullptr;
    std::atomic<float>* autoReleaseParam = nullptr;
    std::atomic<float>* kneeParam = nullptr;
    std::atomic<float>* makeupParam = nullptr;
    std::atomic<float>* mixParam = nullptr;
    std::atomic<float>* scHpfParam = nullptr;
    std::atomic<float>* lookaheadParam = nullptr;
    std::atomic<float>* scChannelParam = nullptr;

    VcaCompressorCore core;
    Biquad scFilter[2];
    double scFilterFreq = -1.0;
    bool scUseMid = false;

    DelayLine delay[2];
    int lookaheadSamples = 0;

    SmoothParam makeupGain;   // linear
    SmoothParam mix;          // 0..1

    std::atomic<double> gainReductionDb { 0.0 };
};

} // namespace meridian
