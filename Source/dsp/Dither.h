#pragma once

#include "DSPModuleBase.h"

#include <array>
#include <cstdint>

namespace meridian
{

/**
    Final-stage word-length reduction: TPDF dither + error-feedback noise shaping.

    Per channel (independent PRNG pair and error history):
        v  = x - sum_i c[i] * e[n-1-i] * lsb          error-feedback (shaping)
        w  = v + (u1 - u2) * lsb                       TPDF dither, u ~ U[0,1)
        y  = clamp(round(w / lsb) * lsb, -1, 1 - lsb)  quantise
        e  = clamp((y - v) / lsb, -1.5, 1.5)           total error in LSB
    giving Y = X + E (1 - sum_i c[i] z^-(i+1)): the noise transfer function
    NTF(z) = 1 - sum c z^-k pushes the error away from the ear's most
    sensitive band.

    lsb = 2 / 2^bits (bits = 16 or 24). dither_bits = 32 (float target) or
    dither_enabled = off: bit-exact pass-through. Latency 0.

    PRNG: two independent xorshift64 generators (13/7/17), top 53 bits
    mapped to [0, 1). Their difference is triangular on (-1, 1) LSB.

    Shapes (dither_shape):
      TPDF          no feedback (flat TPDF floor).
      High-Shibata  9-tap psychoacoustic set (Wannamaker, "Psychoacoustically
                    Optimal Noise Shaping", JAES 1992, the F-weighted 9-tap
                    filter also used by SoX):
                    { 2.412, -3.370, 3.937, -4.174, 3.353, -2.205, 1.281, -0.569, 0.0847 }
                    NTF: about -12 dB around 3-4 kHz region / DC, +27 dB at Nyquist.
                    The brief's list ended in 1.015, -0.272, 0.028, which
                    does not match the published set; the published taps
                    are used.
      E-weighted    gentle 3-tap { 0.65, -0.25, 0.1 }: -6 dB at DC, +6 dB at Nyquist.

    Error definition: the TOTAL error y - v (dither inside the loop) is fed
    back, as in the Lipshitz / Wannamaker topology the coefficients were
    designed for. Feeding back only y - (v + dither) would leave the dither
    (2x the quantisation noise power) white and unshaped. Sign: the error is
    subtracted, so the NTF has its zeros where the ear is most sensitive;
    adding it would invert the curve.

    Stability: the history is clamped to +/-1.5 LSB, the largest error
    possible without output saturation (|d| < 1 plus 0.5 rounding), so
    clipping at full scale can never cause runaway feedback.
*/
class Dither final : public DSPModuleBase
{
public:
    explicit Dither (juce::AudioProcessorValueTreeState& state);

    ModuleId getId() const noexcept override { return ModuleId::Dither; }
    void updateParameters() noexcept override;
    void processBlock (double* L, double* R, int numSamples) noexcept override;

    static constexpr int kMaxTaps = 9;
    static constexpr double kErrorClampLsb = 1.5;

    static constexpr std::array<double, kMaxTaps> kHighShibata { 2.412, -3.370, 3.937, -4.174, 3.353, -2.205, 1.281, -0.569, 0.0847 };
    static constexpr std::array<double, kMaxTaps> kEWeighted   { 0.65, -0.25, 0.1, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };

    /** Active bit depth (16 / 24), or 32 when bypassed (float target). */
    int getBits() const noexcept { return bits; }

protected:
    void prepareModule (double sampleRate, int maxBlockSize) override;
    void resetModule() noexcept override;

private:
    struct XorShift64
    {
        std::uint64_t state = 0x9E3779B97F4A7C15ull;

        std::uint64_t next() noexcept
        {
            std::uint64_t x = state;
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            return state = x;
        }

        double uniform() noexcept { return double (next() >> 11) * (1.0 / 9007199254740992.0); }
    };

    struct Channel
    {
        XorShift64 a, b;
        std::array<double, kMaxTaps> error {};   // error[0] = most recent, in LSB
    };

    void seed() noexcept;

    std::atomic<float>* enabledParam = nullptr;
    std::atomic<float>* bitsParam = nullptr;
    std::atomic<float>* shapeParam = nullptr;

    bool active = true;
    int bits = 24;
    int shape = 1;
    int numTaps = 9;
    const std::array<double, kMaxTaps>* coeffs = &kHighShibata;
    std::array<Channel, 2> channels;
};

} // namespace meridian
