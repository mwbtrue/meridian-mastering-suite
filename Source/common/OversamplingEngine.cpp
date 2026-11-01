#include "OversamplingEngine.h"
#include "DotProduct.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace meridian
{

namespace
{
    double besselI0 (double x) noexcept
    {
        const double halfX = 0.5 * x;
        double sum = 1.0;
        double term = 1.0;

        for (int k = 1; k < 64; ++k)
        {
            const double f = halfX / static_cast<double> (k);
            term *= f * f;
            sum += term;

            if (term < sum * 1.0e-17)
                break;
        }

        return sum;
    }

    int sanitiseFactor (int requested, int maxAllowed) noexcept
    {
        int f = 1;
        while (f * 2 <= requested && f * 2 <= maxAllowed && f < 16)
            f *= 2;
        return f;
    }

    // Half-band lengths N = 4K - 1 (K nonzero taps per side), Kaiser beta.
    constexpr int kStageLength[OversamplingEngine::kMaxStages] = { 107, 27, 19, 19 };
    constexpr double kStageBeta = 8.36;
}

//==============================================================================
const std::array<OversamplingEngine::Stage, OversamplingEngine::kMaxStages>& OversamplingEngine::getStages()
{
    static const std::array<Stage, kMaxStages> stages = []
    {
        std::array<Stage, kMaxStages> s;

        for (int i = 0; i < kMaxStages; ++i)
        {
            const int N = kStageLength[i];
            const int K = (N + 1) / 4;
            const int c = (N - 1) / 2;
            const double i0Beta = besselI0 (kStageBeta);

            // a[k] = h[c + 2k - 1], k = 1..K: the nonzero off-centre taps of
            // h[j] = 0.5 sinc((j - c) / 2) w[j]. Normalised so sum(a) = 1/4,
            // which gives unity DC gain on both polyphase branches.
            std::vector<double> a ((size_t) K + 1, 0.0);
            double sum = 0.0;

            for (int k = 1; k <= K; ++k)
            {
                const int j = c + 2 * k - 1;
                const double x = 0.5 * (double) (j - c);
                const double sinc = std::sin (std::numbers::pi * x) / (std::numbers::pi * x);
                const double r = (double) (j - c) / (double) c;
                const double w = besselI0 (kStageBeta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0Beta;
                a[(size_t) k] = 0.5 * sinc * w;
                sum += a[(size_t) k];
            }

            for (int k = 1; k <= K; ++k)
                a[(size_t) k] *= 0.25 / sum;

            auto& st = s[(size_t) i];
            st.half = K;
            st.upKernel.assign ((size_t) (2 * K), 0.0);
            st.downKernel.assign ((size_t) (2 * K), 0.0);

            for (int k = 1; k <= K; ++k)
            {
                st.upKernel[(size_t) (K - k)] = st.upKernel[(size_t) (K - 1 + k)] = 2.0 * a[(size_t) k];
                st.downKernel[(size_t) (K - k)] = st.downKernel[(size_t) (K - 1 + k)] = a[(size_t) k];
            }
        }

        return s;
    }();

    return stages;
}

int OversamplingEngine::numStagesFor (int f) noexcept
{
    int s = 0;
    while ((1 << s) < f && s < kMaxStages)
        ++s;
    return s;
}

int OversamplingEngine::roundTripHighRate (int f) noexcept
{
    // Stage i (0-based) runs at 2^(i+1) x and delays by its centre c_i = (N_i - 1) / 2
    // samples there, once on the way up and once on the way down. Expressed in
    // samples at the top rate f: 2 * c_i * f / 2^(i+1) = (N_i - 1) * (f >> (i + 1)).
    const int S = numStagesFor (f);
    int total = 0;
    for (int i = 0; i < S; ++i)
        total += (kStageLength[i] - 1) * (f >> (i + 1));
    return total;
}

int OversamplingEngine::padFor (int f) noexcept
{
    return f > 1 ? kRoundTripLatency * f - roundTripHighRate (f) : 0;
}

int OversamplingEngine::latencyForFactor (int f) noexcept
{
    return sanitiseFactor (f, 16) > 1 ? kRoundTripLatency : 0;
}

//==============================================================================
OversamplingEngine::OversamplingEngine (int maxChans, int maxFact)
    : numChannels (std::max (1, maxChans)),
      maxFactor (sanitiseFactor (std::max (1, maxFact), 16))
{
    const auto& stages = getStages();
    channels.resize ((size_t) numChannels);

    for (auto& c : channels)
        for (int i = 0; i < kMaxStages; ++i)
        {
            const int K = stages[(size_t) i].half;
            auto& st = c.stages[(size_t) i];
            st.upHist.assign ((size_t) (4 * K), 0.0);
            st.evenHist.assign ((size_t) (4 * K), 0.0);
            st.oddRing.assign ((size_t) K, 0.0);
        }

    factor = 1;
    numStages = 0;
    padSamples = 0;
    latency = 0;
}

void OversamplingEngine::setOversamplingFactor (int requested) noexcept
{
    const int f = sanitiseFactor (requested, maxFactor);

    if (f == factor)
        return;

    factor = f;
    numStages = numStagesFor (f);
    padSamples = f > 1 ? padFor (f) : 0;
    latency = latencyForFactor (f);
    reset();
}

void OversamplingEngine::reset() noexcept
{
    for (auto& c : channels)
    {
        for (auto& st : c.stages)
        {
            std::fill (st.upHist.begin(), st.upHist.end(), 0.0);
            std::fill (st.evenHist.begin(), st.evenHist.end(), 0.0);
            std::fill (st.oddRing.begin(), st.oddRing.end(), 0.0);
            st.upPos = st.evenPos = st.oddPos = st.phase = 0;
        }

        c.pad.fill (0.0);
        c.padPos = 0;
    }
}

//==============================================================================
int OversamplingEngine::upsample (const double* in, double* out, int numSamplesIn, int channel) noexcept
{
    if (numSamplesIn <= 0 || channel < 0 || channel >= numChannels)
        return 0;

    if (factor == 1)
    {
        std::copy (in, in + numSamplesIn, out);
        return numSamplesIn;
    }

    auto& c = channels[(size_t) channel];
    const auto& stages = getStages();
    const int total = numSamplesIn * factor;

    // Each stage reads m samples from out[total - m, total) and writes 2m samples
    // to out[total - 2m, total). Output pair i lands at or before input i, so the
    // forward in-place expansion never overwrites an unread input.
    std::copy (in, in + numSamplesIn, out + (total - numSamplesIn));
    int m = numSamplesIn;

    for (int s = 0; s < numStages; ++s)
    {
        const auto& stage = stages[(size_t) s];
        auto& st = c.stages[(size_t) s];
        const int K = stage.half;
        const int L = 2 * K;
        const double* kernel = stage.upKernel.data();
        double* hist = st.upHist.data();
        int pos = st.upPos;

        const double* src = out + (total - m);
        double* dst = out + (total - 2 * m);

        for (int i = 0; i < m; ++i)
        {
            const double x = src[i];
            pos = (pos == 0) ? L - 1 : pos - 1;
            hist[pos] = x;
            hist[pos + L] = x;

            const double* w = hist + pos;                 // newest first
            dst[2 * i]     = symmetricDotProduct (kernel, w, L);
            dst[2 * i + 1] = w[K - 1];                    // centre branch: x[n - K + 1]
        }

        st.upPos = pos;
        m *= 2;
    }

    if (padSamples > 0)
    {
        // Short high-rate delay that brings the round trip to exactly
        // kRoundTripLatency base-rate samples for every factor (padSamples < kPadRing).
        int p = c.padPos;
        for (int i = 0; i < total; ++i)
        {
            c.pad[(size_t) p] = out[i];
            out[i] = c.pad[(size_t) ((p - padSamples) & (kPadRing - 1))];
            p = (p + 1) & (kPadRing - 1);
        }
        c.padPos = p;
    }

    return total;
}

int OversamplingEngine::downsample (const double* in, double* out, int numSamplesIn, int channel) noexcept
{
    if (numSamplesIn <= 0 || channel < 0 || channel >= numChannels)
        return 0;

    if (factor == 1)
    {
        std::copy (in, in + numSamplesIn, out);
        return numSamplesIn;
    }

    auto& c = channels[(size_t) channel];
    const auto& stages = getStages();
    int written = 0;

    for (int n = 0; n < numSamplesIn; ++n)
    {
        double v = in[n];
        bool emitted = true;

        // Highest-rate stage first. A half-band decimator emits one sample for
        // every even input: y = sum a (even taps, folded) + 0.5 * odd[n - K].
        for (int s = numStages - 1; s >= 0; --s)
        {
            const auto& stage = stages[(size_t) s];
            auto& st = c.stages[(size_t) s];
            const int K = stage.half;

            if (st.phase == 0)
            {
                const int L = 2 * K;
                st.evenPos = (st.evenPos == 0) ? L - 1 : st.evenPos - 1;
                st.evenHist[(size_t) st.evenPos] = v;
                st.evenHist[(size_t) (st.evenPos + L)] = v;

                v = symmetricDotProduct (stage.downKernel.data(), st.evenHist.data() + st.evenPos, L)
                    + 0.5 * st.oddRing[(size_t) st.oddPos];   // oldest of the last K odd samples
                st.phase = 1;
            }
            else
            {
                st.oddRing[(size_t) st.oddPos] = v;
                st.oddPos = (st.oddPos + 1 == K) ? 0 : st.oddPos + 1;
                st.phase = 0;
                emitted = false;
                break;
            }
        }

        if (emitted)
            out[written++] = v;
    }

    return written;
}

} // namespace meridian
