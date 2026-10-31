#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "ParameterIDs.h"

#include <array>
#include <atomic>
#include <string_view>

namespace meridian
{

/** Stable module identifiers. Values index the per-module global parameters
    (module_bypass<N>, module_channel<N>, ...) and MeterData::gainReductionPerModule.
    The default slot order equals the enum order. */
enum class ModuleId : int
{
    Input = 0,
    CorrectiveEQ,
    LowEndControl,
    DynamicEQ,
    GlueCompressor,
    MultibandDynamics,
    ToneEQ,
    Saturation,
    StereoImager,
    Clipper,
    TruePeakLimiter,
    Dither,
    Count
};

static_assert (static_cast<int> (ModuleId::Count) == kNumModules, "ModuleId and kNumModules disagree");

inline constexpr std::array<std::string_view, kNumModules> kModuleNames
{
    "Input", "Corrective EQ", "Low End", "Dynamic EQ", "Glue Comp", "Multiband",
    "Tone EQ", "Saturation", "Imager", "Clipper", "Limiter", "Dither"
};

inline constexpr std::string_view getModuleName (ModuleId id) noexcept
{
    const int i = static_cast<int> (id);
    return (i >= 0 && i < kNumModules) ? kModuleNames[(size_t) i] : std::string_view { "Unknown" };
}

/**
    Common interface for every signal-chain module.

    Contract (enforced by PluginProcessor):
      - Constructed on the message thread with the processor's APVTS; derived
        classes cache RawParameterValue pointers in their constructor.
      - prepare() is called from prepareToPlay (audio stopped) and may allocate.
        maxBlockSize is a hard upper bound: process() never receives more.
      - updateParameters() is called on the audio thread at the start of every
        processed chunk, before process(). It reads atomics, sets smoother
        targets / recomputes coefficients and must call setLatencySamples()
        whenever its latency depends on parameters (phase mode, lookahead,
        oversampling, low_latency_mode).
      - process() is in-place stereo, double precision, real-time safe.
        When the processor routes a single channel (Mid/Side/L/R) it passes the
        same signal in both channels and uses channel 0 of the result.
      - Module latency must be identical on both channels; the processor
        latency-compensates the dry path, bypass and wet/dry mix with it.
      - getLatencySamples() / getGainReductionDb() are safe from any thread.
      - Bypass, wet/dry, delta solo and channel routing are handled by the
        processor; modules implement only their own processing.
*/
class ModuleBase
{
public:
    explicit ModuleBase (juce::AudioProcessorValueTreeState& state) : apvts (state) {}
    virtual ~ModuleBase() = default;

    ModuleBase (const ModuleBase&) = delete;
    ModuleBase& operator= (const ModuleBase&) = delete;

    virtual ModuleId getId() const noexcept = 0;
    virtual void prepare (double sampleRate, int maxBlockSize) = 0;
    virtual void reset() noexcept = 0;
    virtual void updateParameters() noexcept = 0;
    virtual void process (double* left, double* right, int numSamples) noexcept = 0;

    std::string_view getName() const noexcept { return getModuleName (getId()); }

    /** Current processing latency in samples (thread-safe). */
    int getLatencySamples() const noexcept { return latencySamples.load (std::memory_order_acquire); }

    /** Current gain reduction in positive dB, for metering (thread-safe). */
    float getGainReductionDb() const noexcept { return gainReductionDb.load (std::memory_order_relaxed); }

protected:
    void setLatencySamples (int samples) noexcept
    {
        latencySamples.store (samples < 0 ? 0 : samples, std::memory_order_release);
    }

    void setGainReductionDb (float db) noexcept
    {
        gainReductionDb.store (db, std::memory_order_relaxed);
    }

    /** Looks up a raw parameter pointer; asserts in debug builds if the ID is unknown. */
    std::atomic<float>* rawParam (const juce::String& id) const
    {
        auto* p = apvts.getRawParameterValue (id);
        jassert (p != nullptr);
        return p;
    }

    std::atomic<float>* rawParam (std::string_view id) const { return rawParam (pid::str (id)); }

    static float load (const std::atomic<float>* p) noexcept
    {
        return p != nullptr ? p->load (std::memory_order_relaxed) : 0.0f;
    }

    static bool loadBool (const std::atomic<float>* p) noexcept { return load (p) >= 0.5f; }
    static int  loadInt  (const std::atomic<float>* p) noexcept { return juce::roundToInt (load (p)); }

    juce::AudioProcessorValueTreeState& apvts;

private:
    std::atomic<int> latencySamples { 0 };
    std::atomic<float> gainReductionDb { 0.0f };
};

} // namespace meridian
