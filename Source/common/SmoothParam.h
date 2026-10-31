#pragma once

#include <cmath>

namespace meridian
{

/**
    One-pole IIR parameter smoother:  current += (target - current) * coeff

    The smoothing time is the time constant tau (time to reach ~63.2 % of a
    step). coeff = 1 - exp(-1 / (tau * fs)). Once the remaining distance falls
    below a tiny epsilon the value snaps to the target so isSmoothing() becomes
    false and callers can take a constant-value fast path.

    All methods are allocation-free and real-time safe.
*/
class SmoothParam
{
public:
    SmoothParam() noexcept { updateCoefficient(); }

    SmoothParam (double sampleRate, double smoothingTimeMs, double initialValue = 0.0) noexcept
        : fs (sampleRate > 0.0 ? sampleRate : 44100.0),
          timeMs (smoothingTimeMs),
          current (initialValue),
          target (initialValue)
    {
        updateCoefficient();
    }

    void setSampleRate (double newSampleRate) noexcept
    {
        if (newSampleRate > 0.0)
        {
            fs = newSampleRate;
            updateCoefficient();
        }
    }

    void setSmoothingTime (double newTimeMs) noexcept
    {
        timeMs = newTimeMs < 0.0 ? 0.0 : newTimeMs;
        updateCoefficient();
    }

    /** Convenience: set both in one call (e.g. from prepareToPlay). */
    void prepare (double newSampleRate, double newTimeMs) noexcept
    {
        fs = newSampleRate > 0.0 ? newSampleRate : fs;
        timeMs = newTimeMs < 0.0 ? 0.0 : newTimeMs;
        updateCoefficient();
    }

    void setTargetValue (double newTarget) noexcept { target = newTarget; }

    /** Snap to a value immediately (no ramp). */
    void reset (double value) noexcept
    {
        current = value;
        target = value;
    }

    /** Advance by one sample and return the new smoothed value. */
    double processSample() noexcept
    {
        if (current != target)
        {
            current += (target - current) * coeff;

            if (std::abs (target - current) <= snapEpsilon)
                current = target;
        }

        return current;
    }

    /** Alias of processSample() for call sites that read better with process(). */
    double process() noexcept { return processSample(); }

    /** Advance by numSamples at once (exact closed form of numSamples single steps). */
    double skip (int numSamples) noexcept
    {
        if (numSamples <= 0 || current == target)
            return current;

        const double remaining = std::pow (1.0 - coeff, static_cast<double> (numSamples));
        current = target + (current - target) * remaining;

        if (std::abs (target - current) <= snapEpsilon)
            current = target;

        return current;
    }

    double getCurrentValue() const noexcept { return current; }
    double getTargetValue() const noexcept  { return target; }
    bool isSmoothing() const noexcept       { return current != target; }

    double getSampleRate() const noexcept      { return fs; }
    double getSmoothingTimeMs() const noexcept { return timeMs; }
    double getCoefficient() const noexcept     { return coeff; }

private:
    void updateCoefficient() noexcept
    {
        const double samples = timeMs * 0.001 * fs;
        coeff = samples <= 1.0e-9 ? 1.0 : 1.0 - std::exp (-1.0 / samples);
    }

    static constexpr double snapEpsilon = 1.0e-9;

    double fs = 44100.0;
    double timeMs = 20.0;
    double coeff = 1.0;
    double current = 0.0;
    double target = 0.0;
};

} // namespace meridian
