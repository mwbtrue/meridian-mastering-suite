#pragma once

#include <array>
#include <vector>

namespace meridian
{

/**
    Streaming inter-sample (true) peak estimator.

    A polyphase interpolator evaluates the band-limited reconstruction of the
    input at M equally spaced positions per input sample (M = 1, 2, 4, 8, 16)
    and returns, for every input sample, the largest magnitude found on the
    interval [c, c + 1), where c = n - halfSpan is the centre sample.

    Kernel: ideal reconstruction sinc(t) with cutoff exactly at the input
    Nyquist frequency, Kaiser-windowed over |t| < halfSpan input samples
    (2 * halfSpan taps per phase). Because sinc has zeros at the integers,
    phase 0 reproduces the input sample exactly; every phase is normalised to
    unity DC gain. Unlike an anti-imaging filter with a 0.45 fs passband edge,
    this kernel keeps near-Nyquist content at (nearly) full weight, which is
    what an ideal reconstruction (DAC / lossy encoder resampler) sees.

    Latency: halfSpan input samples. All tables for every factor up to
    maxFactor are built in prepare(), so setFactor() is real-time safe.
*/
class TruePeakDetector
{
public:
    static constexpr int kMaxChannels = 2;

    /** Non-RT: builds the phase tables (factors 1..maxFactor) and the histories. */
    void prepare (int maxFactor, int halfSpan, double kaiserBeta);

    /** Selects the oversampling factor (power of two, clamped to [1, maxFactor]). */
    void setFactor (int newFactor) noexcept;
    int getFactor() const noexcept { return factor; }

    void reset() noexcept;

    /** Pushes one sample of channel ch and returns the peak magnitude of the
        reconstructed signal on [n - halfSpan, n - halfSpan + 1). */
    double process (int ch, double x) noexcept;

    int getLatencySamples() const noexcept { return halfSpan; }
    int getHalfSpan() const noexcept       { return halfSpan; }

private:
    struct Table
    {
        int factor = 1;
        std::vector<double> coeffs;   // factor * length, phase-major, oldest -> newest
    };

    static int indexForFactor (int f) noexcept;

    int halfSpan = 12;
    int length = 24;
    int maxFactor = 16;
    int factor = 4;
    const Table* active = nullptr;
    std::array<Table, 5> tables;

    struct Channel
    {
        std::vector<double> history;   // 2 * length, mirrored
        int writePos = 0;
    };

    std::array<Channel, kMaxChannels> channels;
};

/**
    Look-ahead gain smoother with a hard guarantee:
        applied gain for input sample t  <=  required gain r[t]

    Pipeline per sample (all O(1) amortised, pre-allocated):
        m[n] = min(r[n - H + 1 .. n])            sliding minimum, monotonic deque
        e[n] = m[n] if m[n] < e[n-1]
               else e[n-1] + rel * (m[n] - e[n-1])   (release never exceeds m)
        g[n] = mean(e[n - K + 1 .. n])           box filter -> linear attack ramp

    with H = K + S. If the audio is delayed by D = K - 1 + S / 2 samples, every
    term of the mean that multiplies sample t satisfies e <= m <= r[t], hence
    g <= r[t] exactly. In addition, an isolated requirement r[t] is applied
    with constant gain to all samples within +/- S/2 of t (a gain "plateau"),
    so an interpolation kernel spanning <= S samples around a peak sees a pure
    scalar gain: the reconstructed peak is scaled exactly, with no
    modulation-induced inter-sample overshoot.
*/
class GuaranteedGainSmoother
{
public:
    /** Non-RT. maxHold >= K + S, maxBox >= K. */
    void prepare (int maxHold, int maxBox);

    /** Sets the ramp length K (>= 1) and plateau S (even, >= 0); resets to initialGain. */
    void configure (int rampLength, int plateau, double initialGain = 1.0) noexcept;

    void reset (double initialGain = 1.0) noexcept;

    /** Pushes r[n] in (0, 1] and returns g for the sample delayed by getDelay(). */
    double process (double required, double releaseCoeff) noexcept;

    /** Audio delay D that makes the guarantee hold. */
    int getDelay() const noexcept { return ramp - 1 + plateau / 2; }
    int getRampLength() const noexcept { return ramp; }
    int getPlateau() const noexcept { return plateau; }

private:
    std::vector<double> dqValue;
    std::vector<long long> dqIndex;
    int dqCapacity = 0, dqHead = 0, dqSize = 0;

    std::vector<double> box;
    int boxPos = 0;
    double boxSum = 0.0;

    long long sampleIndex = 0;
    int ramp = 1, plateau = 0, hold = 1;
    double envelope = 1.0;
};

} // namespace meridian
