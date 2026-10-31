#pragma once

#include "DSPModuleBase.h"
#include "common/Biquad.h"

#include <array>
#include <atomic>

namespace meridian
{

/**
    Analog-modelled tone EQ (zero latency, minimum phase).

    Every section is an analog prototype H(s), normalised to its design
    frequency and mapped with the bilinear transform pre-warped at that
    frequency (so corner/centre frequencies are exact at any sample rate):

      Pultec low boost   2nd-order resonant low shelf, Q 0.9 (the slight
                         resonance gives the EQP-1A "bump" above the shelf),
                         frequency from the 20/30/60/100 Hz selector.
      Pultec low atten   bell cut at the 100/200/300/400 Hz selector, bandwidth
                         in octaves. Boost and atten run simultaneously, so the
                         classic "boost 60 + cut 200" low-end trick is
                         reproduced (shelf lift with a tightening dip above).
      Pultec mid boost   bell, bandwidth in octaves (Q = sqrt(2^N) / (2^N - 1)).
      Pultec treble      resonant high shelf; the bandwidth control sets the
                         shelf Q (Q = 0.5 + 0.45 / BW): narrow = more presence
                         bump below the corner, broad = smooth air lift.
      Baxandall bass     1st-order shelf  H(s) = (s + sqrt G) / (s + 1/sqrt G)
      Baxandall treble   1st-order shelf  H(s) = (sqrt G s + 1) / (s / sqrt G + 1)
                         (6 dB/oct tone-control slopes; frequency = half-gain point)
      Passive mid        bell, Q 0.3 - 1.5 (passive LC range)
      Side air           2nd-order high shelf (Q 0.707) on the Side signal only

    Low boost frequency: the parameter layout keeps the authentic Pultec
    switch positions (20/30/60/100 Hz) so automation and presets stay stable;
    the spec's "~70 Hz" default maps to the closest position, 60 Hz.

    Smoothing: gain (dB), log-frequency and log-Q are smoothed (10 ms time
    constant); while any of them moves, coefficients are redesigned every
    16 samples. A section with |gain| < 1e-6 dB uses exact identity
    coefficients and is skipped, so with all gains at 0 dB the module is a
    bit-exact pass-through (nulls).

    Analog colour (teq_colour, 0-100 %, default 0): y = x + HP5Hz(a (c2 x^2 - c3 x^3))
    with c2 = 0.04, c3 = 0.02 (subtle 2nd/3rd harmonics, tube/transformer
    flavour, DC removed). At 0 % the term is not computed at all.
*/
class ToneEQ final : public DSPModuleBase
{
public:
    explicit ToneEQ (juce::AudioProcessorValueTreeState& state);

    ModuleId getId() const noexcept override { return ModuleId::ToneEQ; }
    void updateParameters() noexcept override;
    void processBlock (double* L, double* R, int numSamples) noexcept override;

    enum class Kind { LowShelf, HighShelf, Bell, FirstOrderLowShelf, FirstOrderHighShelf };

    enum SectionIndex
    {
        PultecLow = 0, PultecAtten, PultecMid, PultecTreble,
        BaxandallBass, BaxandallTreble, PassiveMid, SideAir,
        NumSections
    };

    /** Designs one section (exposed for tests and the UI curve display). */
    static BiquadCoeffs designSection (Kind kind, double freq, double gainDb, double q, double sampleRate) noexcept;

    /** Magnitude (dB) of the complete stereo-path curve at freq for the current targets (UI). */
    double getStereoMagnitudeDb (double freq) const noexcept;

    static constexpr double kSmoothingMs = 10.0;
    static constexpr int kControlInterval = 16;
    static constexpr double kPultecLowQ = 0.9;
    static constexpr double kSideAirQ = 0.70710678118654752;

protected:
    void prepareModule (double sampleRate, int maxBlockSize) override;
    void resetModule() noexcept override;

private:
    struct Section
    {
        Kind kind = Kind::Bell;
        SmoothParam gainDb, logFreq, logQ;
        Biquad filter[2];
        bool identity = true;
        bool stereo = true;   // false: Side-only (filter[0] used on S)
    };

    void readTargets (double* gains, double* freqs, double* qs) const noexcept;
    void redesign (Section& s) noexcept;
    bool anySmoothing() const noexcept;

    static double qFromBandwidth (double octaves) noexcept;

    std::array<Section, NumSections> sections;

    std::atomic<float>* bassFreqParam = nullptr;
    std::atomic<float>* bassBoostParam = nullptr;
    std::atomic<float>* bassAttenParam = nullptr;
    std::atomic<float>* attenFreqParam = nullptr;
    std::atomic<float>* attenBwParam = nullptr;
    std::atomic<float>* midBoostParam = nullptr;
    std::atomic<float>* midFreqParam = nullptr;
    std::atomic<float>* midBwParam = nullptr;
    std::atomic<float>* trebleBoostParam = nullptr;
    std::atomic<float>* trebleFreqParam = nullptr;
    std::atomic<float>* trebleBwParam = nullptr;
    std::atomic<float>* baxBassParam = nullptr;
    std::atomic<float>* baxBassFreqParam = nullptr;
    std::atomic<float>* baxTrebleParam = nullptr;
    std::atomic<float>* baxTrebleFreqParam = nullptr;
    std::atomic<float>* passiveGainParam = nullptr;
    std::atomic<float>* passiveFreqParam = nullptr;
    std::atomic<float>* passiveQParam = nullptr;
    std::atomic<float>* airGainParam = nullptr;
    std::atomic<float>* airFreqParam = nullptr;
    std::atomic<float>* colourParam = nullptr;

    SmoothParam colour;
    double dcCoeff = 0.0;
    double dcX[2] {}, dcY[2] {};
    int controlCounter = 0;
};

} // namespace meridian
