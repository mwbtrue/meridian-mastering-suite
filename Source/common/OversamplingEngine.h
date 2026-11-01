#pragma once

#include <array>
#include <vector>

namespace meridian
{

/**
    Multi-stage half-band polyphase oversampler (1x / 2x / 4x / 8x / 16x).

    Each factor is a cascade of 2x stages (log2(M) stages). Every stage is a
    linear-phase Kaiser-windowed half-band FIR, so one of its two polyphase
    branches is a pure delay and the other is a symmetric kernel evaluated with
    a folded dot product. Compared with a single long polyphase filter this needs
    about 8x fewer multiplies at 8x.

      stage  rate      taps  passband         stopband (>= 82 dB)
      1      1x->2x    107   0 .. 0.45 fs     0.55 fs ..
      2      2x->4x     27   0 .. 0.45 fs     1.45 fs ..
      3      4x->8x     19   0 .. 0.45 fs     3.45 fs ..
      4      8x->16x    19   0 .. 0.45 fs     7.45 fs ..

    Everything the oversampled stage generates that would alias into
    0 .. 0.45 fs is rejected by >= 82 dB; total passband ripple < 0.003 dB.

    Latency: the natural up + down round trip is 53 (2x), 59.5 (4x), 61.75 (8x)
    and 62.875 (16x) samples at the original rate. A short high-rate delay pads
    every factor to exactly kRoundTripLatency = 63 samples (0 at 1x), so changing
    the factor never changes the latency a module reports.

    All processing methods are real-time safe (no allocation, no locks).
*/
class OversamplingEngine
{
public:
    /** Upper bound of getLatencySamples() for any factor (for delay-line sizing). */
    static constexpr int kMaxLatencySamples = 64;
    /** Round-trip latency for every factor > 1. */
    static constexpr int kRoundTripLatency = 63;
    static constexpr int kMaxStages = 4;

    OversamplingEngine (int maxChannels, int maxFactor);

    /** Selects 1, 2, 4, 8 or 16 (clamped to maxFactor; invalid values round down
        to the nearest supported power of two). Resets filter state if changed. */
    void setOversamplingFactor (int factor) noexcept;
    int getOversamplingFactor() const noexcept { return factor; }
    int getMaxFactor() const noexcept          { return maxFactor; }
    int getNumChannels() const noexcept        { return numChannels; }

    /** Upsamples numSamplesIn samples of one channel into out (must hold
        getUpsampledBufferSize(numSamplesIn) samples; must not alias in).
        Returns samples written. */
    int upsample (const double* in, double* out, int numSamplesIn, int channel = 0) noexcept;

    /** Filters and decimates numSamplesIn high-rate samples of one channel.
        The decimation phase is kept across calls, so any block length works;
        for block lengths that are multiples of the factor the return value is
        exactly numSamplesIn / factor. Returns samples written. */
    int downsample (const double* in, double* out, int numSamplesIn, int channel = 0) noexcept;

    int getUpsampledBufferSize (int numSamples) const noexcept { return numSamples * factor; }

    /** Round-trip (upsample + downsample) latency at the original sample rate. */
    int getLatencySamples() const noexcept { return latency; }

    /** Clears all filter histories. */
    void reset() noexcept;

    /** Round-trip latency for a factor (static, for documentation and tests). */
    static int latencyForFactor (int factor) noexcept;

private:
    struct Stage
    {
        int half = 1;                     // K: nonzero taps per side
        std::vector<double> upKernel;     // 2K symmetric taps (interpolating branch, gain 2 folded in)
        std::vector<double> downKernel;   // 2K symmetric taps (= upKernel / 2)
    };

    static const std::array<Stage, kMaxStages>& getStages();
    static int numStagesFor (int factor) noexcept;
    static int padFor (int factor) noexcept;
    static int roundTripHighRate (int factor) noexcept;
    static constexpr int kPadRing = 32;

    struct StageState
    {
        std::vector<double> upHist;       // 2 * 2K, mirrored, newest first
        int upPos = 0;
        std::vector<double> evenHist;     // 2 * 2K, mirrored, newest first
        int evenPos = 0;
        std::vector<double> oddRing;      // K
        int oddPos = 0;
        int phase = 0;
    };

    struct ChannelState
    {
        std::array<StageState, kMaxStages> stages;
        std::array<double, kPadRing> pad {};   // high-rate alignment delay (< kPadRing samples)
        int padPos = 0;
    };

    const int numChannels;
    const int maxFactor;
    int factor = 1;
    int numStages = 0;
    int padSamples = 0;
    int latency = 0;
    std::vector<ChannelState> channels;
};

} // namespace meridian
