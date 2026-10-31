#pragma once

#include <array>
#include <vector>

namespace meridian
{

/**
    Single-stage polyphase FIR oversampler (1x / 2x / 4x / 8x / 16x).

    Anti-imaging / anti-aliasing prototype (one per factor M):
      - Kaiser-windowed sinc, beta = 0.1102 * (80 - 8.7)  -> >= 80 dB stopband
      - Passband edge 0.45 * fs, stopband edge 0.55 * fs (fs = original rate),
        i.e. cutoff at the original Nyquist with a 0.1 * fs transition band.
        Everything the oversampled stage generates above 0.55 * fs is rejected
        by >= 80 dB before decimation.
      - Length L = 52 * M + 1 taps (odd, symmetric, linear phase). Kaiser's
        estimate for this spec is ~50.2 * M, so 52 * M leaves margin.

    The prototypes for 2x/4x/8x/16x are computed once per process (thread-safe
    static initialisation) and shared by all instances, so changing the factor
    at run time is allocation-free.

    Latency: the up + down round trip delays by (L - 1) high-rate samples =
    52 samples at the original rate, for every factor > 1 (0 for 1x).

    All processing methods are real-time safe (no allocation, no locks).
*/
class OversamplingEngine
{
public:
    static constexpr int tapsPerPhase = 52;
    static constexpr double stopbandAttenuationDb = 80.0;
    static constexpr double passbandEdge = 0.45;   // fraction of the original sample rate
    static constexpr double stopbandEdge = 0.55;   // fraction of the original sample rate

    OversamplingEngine (int maxChannels, int maxFactor);

    /** Selects 1, 2, 4, 8 or 16 (clamped to maxFactor; invalid values round down
        to the nearest supported power of two). Resets filter state if changed. */
    void setOversamplingFactor (int factor) noexcept;
    int getOversamplingFactor() const noexcept { return factor; }
    int getMaxFactor() const noexcept          { return maxFactor; }
    int getNumChannels() const noexcept        { return numChannels; }

    /** Upsamples numSamplesIn samples of one channel into out (must hold
        getUpsampledBufferSize(numSamplesIn) samples). Returns samples written. */
    int upsample (const double* in, double* out, int numSamplesIn, int channel = 0) noexcept;

    /** Filters and decimates numSamplesIn high-rate samples of one channel.
        The decimation phase is kept across calls, so any block length works;
        for block lengths that are multiples of the factor the return value is
        exactly numSamplesIn / factor. Returns samples written. */
    int downsample (const double* in, double* out, int numSamplesIn, int channel = 0) noexcept;

    int getUpsampledBufferSize (int numSamples) const noexcept { return numSamples * factor; }

    /** Round-trip (upsample + downsample) latency at the original sample rate. */
    int getLatencySamples() const noexcept { return factor > 1 ? tapsPerPhase : 0; }

    /** Clears all filter histories. */
    void reset() noexcept;

    /** Prototype length for a factor (L = tapsPerPhase * factor + 1). */
    static int getFilterLength (int factor) noexcept { return factor > 1 ? tapsPerPhase * factor + 1 : 1; }

    /** Shared prototype low-pass for 2/4/8/16 (DC gain 1). */
    static const std::vector<double>& getPrototype (int factor);

private:
    struct FactorTables
    {
        int factor = 1;
        int length = 1;
        int phaseLength = 1;                 // tapsPerPhase + 1 (zero padded)
        std::vector<double> prototype;       // L taps, DC gain 1
        std::vector<double> polyphase;       // factor * phaseLength, scaled by factor
    };

    static const std::array<FactorTables, 5>& getTables();
    static FactorTables buildTables (int factor);
    static int tableIndexForFactor (int factor) noexcept;

    struct ChannelState
    {
        std::vector<double> upHistory;       // 2 * (tapsPerPhase + 1), mirrored
        int upPos = 0;
        std::vector<double> downHistory;     // 2 * maxLength, mirrored
        int downPos = 0;
        int downPhase = 0;
    };

    const int numChannels;
    const int maxFactor;
    int factor = 1;
    const FactorTables* active = nullptr;
    std::vector<ChannelState> channels;
};

} // namespace meridian
