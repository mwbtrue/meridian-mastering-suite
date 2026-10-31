#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "TargetProfile.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace meridian
{

/**
    Offline analysis + parameter proposal engine. Deterministic: no network,
    no machine learning - every proposal follows from a documented rule.

    Threads
      analyzeBuffer()      message thread; copies the audio and runs the
                           analysis on a private juce::Thread.
      updateGRBudget()     AUDIO thread; atomics only (no locks, no allocation).
      everything else      message thread (results are mutex-protected and are
                           never touched by the audio thread).

    Analysis (runAnalysis)
      1. MeteringEngine offline pass (100 ms steps): integrated LUFS, max
         short-term LUFS (after the first 3 s), true peak, LRA; PLR = TP - I.
      2. 2048-pt Hann spectrum, hop 1024, power average of L and R
         (SpectrumAnalyzer dB scale: full-scale sine = 0 dB).
      3. Sibilance: RMS (dBFS) of Mid through LR4-style 4th-order band-pass
         5.5 - 9 kHz (two 2nd-order HPF + two 2nd-order LPF sections).
      4. Mud: RMS 200 - 400 Hz (same structure).
      5. Sub: RMS 20 - 80 Hz.
      6. Spectral tilt = mean dB(8 - 16 kHz) - mean dB(100 - 800 Hz), reported
         RELATIVE to kReferenceTilt (-24 dB, the raw value of a typical modern
         commercial master: ~ -4.5 dB/oct per FFT bin over the 5.3 octaves
         between the band centres). 0 = balanced, negative = dull, positive = bright.
      7. Low-band correlation: sum(LR) / sqrt(sum(L^2) sum(R^2)) over the
         whole buffer after a 150 Hz 4th-order low-pass.

    Proposals (generateProposals) start from the active profile's
    paramSuggestions (only where the current value differs) and are then
    overridden by analysis-driven rules (see IntelligentAssistant.cpp). Nothing
    is applied until acceptProposal() / acceptAllProposals().

    Choice parameters: a suggestion value that is not a valid choice index is
    matched against the numeric value shown by the choices ("60" -> "60 Hz",
    "14000" -> "14 kHz"); bool parameters take 0 / 1.
*/
class IntelligentAssistant
{
public:
    static constexpr int kNumBins = 1025;
    static constexpr int kNumModules = 12;
    static constexpr float kReferenceTilt = -24.0f;
    static constexpr float kSibilanceLimitDb = -18.0f;

    struct AnalysisResult
    {
        float integratedLUFS = -150.0f, shortTermPeakLUFS = -150.0f, truePeak_dBTP = -150.0f;
        float PLR = 0.0f, LRA = 0.0f, lowCorrelation = 1.0f;
        float sibilanceEnergy_dB = -150.0f;   // RMS 5.5 - 9 kHz, Mid
        float muddyEnergy_dB = -150.0f;       // RMS 200 - 400 Hz, Mid
        float subEnergy_dB = -150.0f;         // RMS 20 - 80 Hz, Mid
        float spectralTilt_dB = 0.0f;         // relative to kReferenceTilt
        float durationSeconds = 0.0f;
        std::array<float, kNumBins> avgSpectrum {};
        bool valid = false;
    };

    struct ParameterProposal
    {
        juce::String paramId;
        float currentValue = 0.0f;
        float proposedValue = 0.0f;
        juce::String reason;      // human-readable, <= 80 chars
        bool accepted = false;
    };

    IntelligentAssistant();
    ~IntelligentAssistant();

    /** The parameter tree proposals read current values from (message thread, once). */
    void attach (juce::AudioProcessorValueTreeState& state) { apvts = &state; }

    void prepare (double sampleRate, int maxBlockSize);

    /** Message thread: copies buf and starts an analysis pass on a background thread. */
    void analyzeBuffer (const juce::AudioBuffer<float>& buf, double sampleRate, const juce::String& profileName);
    bool isAnalyzing() const noexcept { return analyzing_.load (std::memory_order_acquire); }

    AnalysisResult getAnalysisResult() const;
    std::vector<ParameterProposal> getProposals() const;

    /** Applies one proposal (message thread) and marks it accepted. */
    void acceptProposal (int index, juce::AudioProcessorValueTreeState& state);
    void acceptAllProposals (juce::AudioProcessorValueTreeState& state);
    /** Restores the value the proposal was generated from and clears the accepted flag. */
    void rejectProposal (int index, juce::AudioProcessorValueTreeState& state);
    void rejectAllProposals (juce::AudioProcessorValueTreeState& state);

    /** Loads the embedded factory profiles, then *.json from dir (same name replaces). */
    void loadProfilesFromDirectory (const juce::File& dir);
    void loadFactoryProfiles();
    const std::vector<TargetProfile>& getProfiles() const { return profiles_; }
    /** Exact name match, else case-insensitive prefix match (e.g. "R&B Major Label"). */
    void setActiveProfile (const juce::String& name);
    const TargetProfile* getActiveProfile() const;

    // ---- Gain-reduction budget guard -----------------------------------------
    /** AUDIO thread. GR values in dB (sign ignored: 3 and -3 both mean 3 dB of reduction). */
    void updateGRBudget (float limiterGR_dB, const float* perModuleGR) noexcept;
    bool isGRBudgetWarning() const noexcept { return grWarningFlags.load (std::memory_order_relaxed) != 0; }
    /** Fills msg with the current warning text (empty if none); returns the held limiter GR (dB). */
    float getGRWarningMessage (juce::String& msg) const;

    /** Converts a value to the parameter's real-unit domain (choice matching etc.). */
    static float resolveValue (juce::RangedAudioParameter& param, float value);
    static void setParameter (juce::AudioProcessorValueTreeState& state, const juce::String& id, float value);

    static juce::String describeParameter (const juce::AudioProcessorValueTreeState& state, const juce::String& id);

private:
    class AnalysisThread;

    void runAnalysis (const juce::AudioBuffer<float>& buf, double sampleRate);
    void generateProposals (const AnalysisResult& r, const TargetProfile& profile,
                            juce::AudioProcessorValueTreeState* state);
    float measureSibilance (const juce::AudioBuffer<float>& buf, double sr);
    float measureSpectralTilt (const std::array<float, kNumBins>& spectrum, double sr);

    static float bandRmsDb (const juce::AudioBuffer<float>& buf, double sr, double lowHz, double highHz);
    static float lowBandCorrelation (const juce::AudioBuffer<float>& buf, double sr);

    std::vector<TargetProfile> profiles_;
    int activeIndex_ = -1;
    const TargetProfile* activeProfile_ = nullptr;
    juce::AudioProcessorValueTreeState* apvts = nullptr;

    std::atomic<bool> analyzing_ { false };
    std::unique_ptr<juce::Thread> analysisThread_;
    juce::AudioBuffer<float> pendingBuffer_;
    double pendingRate_ = 48000.0;
    TargetProfile pendingProfile_;

    AnalysisResult result_;
    std::vector<ParameterProposal> proposals_;
    mutable std::mutex resultMutex_;

    double sampleRate_ = 48000.0;

    // GR budget (audio thread writes, any thread reads)
    std::array<float, kNumModules> grHeld_ {};
    float limiterHeld_ = 0.0f;
    std::atomic<int> grWarningFlags { 0 };     // bit0: limiter > 4 dB, bit1: single-module dominance
    std::atomic<int> grDominantModule { -1 };
    std::atomic<float> grLimiterDb { 0.0f }, grTotalDb { 0.0f }, grDominantShare { 0.0f };

    JUCE_DECLARE_NON_COPYABLE (IntelligentAssistant)
};

} // namespace meridian
