#pragma once

#include <algorithm>
#include <vector>

namespace meridian
{

/**
    Single-channel integer delay line with a run-time variable delay.

    prepare() allocates (non-RT). process() is allocation-free and supports
    in-place operation (in == out). Delays are clamped to [0, maxDelay].
    Used for latency-compensated dry paths, lookahead and delta monitoring.
*/
class DelayLine
{
public:
    void prepare (int maxDelaySamples)
    {
        maxDelay = std::max (0, maxDelaySamples);

        int size = 1;
        while (size < maxDelay + 1)
            size <<= 1;

        buffer.assign ((size_t) size, 0.0);
        mask = size - 1;
        writePos = 0;
    }

    void reset() noexcept
    {
        std::fill (buffer.begin(), buffer.end(), 0.0);
        writePos = 0;
    }

    /** Writes numSamples from in and reads them back delayed by delaySamples into out. */
    void process (const double* in, double* out, int numSamples, int delaySamples) noexcept
    {
        if (buffer.empty())
        {
            if (in != out)
                std::copy (in, in + numSamples, out);
            return;
        }

        const int d = std::clamp (delaySamples, 0, maxDelay);
        double* buf = buffer.data();

        for (int i = 0; i < numSamples; ++i)
        {
            buf[writePos] = in[i];
            out[i] = buf[(writePos - d) & mask];
            writePos = (writePos + 1) & mask;
        }
    }

    /** Single-sample variant of process(). */
    double processSample (double in, int delaySamples) noexcept
    {
        if (buffer.empty())
            return in;

        const int d = std::clamp (delaySamples, 0, maxDelay);
        buffer[(size_t) writePos] = in;
        const double out = buffer[(size_t) ((writePos - d) & mask)];
        writePos = (writePos + 1) & mask;
        return out;
    }

    int getMaxDelay() const noexcept { return maxDelay; }

private:
    std::vector<double> buffer;
    int mask = 0;
    int writePos = 0;
    int maxDelay = 0;
};

} // namespace meridian
