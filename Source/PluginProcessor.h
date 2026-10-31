#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "ParameterIDs.h"
#include "common/AnalysisTypes.h"
#include "common/DelayLine.h"
#include "common/LockFreeQueue.h"
#include "common/SmoothParam.h"
#include "dsp/ModuleBase.h"

#include "dsp/InputModule.h"
#include "dsp/CorrectiveEQ.h"
#include "dsp/LowEndControl.h"
#include "dsp/DynamicEQ.h"
#include "dsp/GlueCompressor.h"
#include "dsp/MultibandDynamics.h"
#include "dsp/ToneEQ.h"
#include "dsp/Saturation.h"
#include "dsp/StereoImager.h"
#include "dsp/Clipper.h"
#include "dsp/TruePeakLimiter.h"
#include "dsp/Dither.h"

#include "analysis/MeteringEngine.h"
#include "analysis/SpectrumAnalyzer.h"
#include "analysis/CodecPreview.h"
#include "analysis/ReferencePanel.h"
#include "assistant/IntelligentAssistant.h"

#include <array>
#include <atomic>
#include <memory>
#include <string_view>
#include <vector>

/**
    Meridian Mastering Suite - main AudioProcessor.

    Signal flow per processed chunk (chunk <= prepared block size):
        host float/double -> double scratch
          -> 12 modules in user-defined slot order; per module:
               latency-compensated dry path, channel routing (Stereo/M/S/L/R),
               bypass + wet/dry (smoothed), optional delta solo
          -> output gain -> metering / spectrum -> host format

    Parameter IDs live in ParameterIDs.h (namespace meridian::pid) so that the
    DSP modules can share them without depending on this header; the most
    used ones are re-exported below as static constexpr string_view members.

    External component contracts used here (implemented in analysis/):
        MeteringEngine:   prepare(double, int), reset(), resetIntegrated(),
                          process(const double*, const double*, int),
                          fillMeterData(MeterData&) const
        SpectrumAnalyzer: prepare(double, int), reset(),
                          pushSamples(const double*, const double*, int),
                          getNextFrame(SpectrumFrame&) -> bool
*/
class MeridianAudioProcessor final : public juce::AudioProcessor,
                                     private juce::Timer
{
public:
    // ---- Types & constants --------------------------------------------------
    using MeterData = meridian::MeterData;
    using SpectrumFrame = meridian::SpectrumFrame;
    using ModuleId = meridian::ModuleId;

    static constexpr int kNumModules        = meridian::kNumModules;
    static constexpr int kNumSnapshots      = meridian::kNumSnapshots;
    static constexpr int kMaxModuleLatency  = 16384;   // per-module dry-path compensation limit
    static constexpr int kMaxChainLatency   = 65536;   // delta-solo downstream compensation limit
    static constexpr int kStateVersion      = 1;

    // ---- Parameter IDs (full registry in ParameterIDs.h) --------------------
    static constexpr std::string_view kInputTrim       = meridian::pid::inputTrim;
    static constexpr std::string_view kCeqPhaseMode    = meridian::pid::ceqPhaseMode;
    static constexpr std::string_view kGlueThreshold   = meridian::pid::glueThreshold;
    static constexpr std::string_view kLimCeiling      = meridian::pid::limCeiling;
    static constexpr std::string_view kClipperCeiling  = meridian::pid::clipperCeiling;
    static constexpr std::string_view kDitherEnabled   = meridian::pid::ditherEnabled;
    static constexpr std::string_view kModuleOrder     = meridian::pid::moduleOrderPrefix;
    static constexpr std::string_view kModuleBypass    = meridian::pid::moduleBypassPrefix;
    static constexpr std::string_view kModuleChannel   = meridian::pid::moduleChannelPrefix;
    static constexpr std::string_view kModuleWetDry    = meridian::pid::moduleWetDryPrefix;
    static constexpr std::string_view kModuleDelta     = meridian::pid::moduleDeltaPrefix;
    static constexpr std::string_view kAbSlot          = meridian::pid::abSlot;
    static constexpr std::string_view kOutputGain      = meridian::pid::outputGain;
    static constexpr std::string_view kLowLatencyMode  = meridian::pid::lowLatencyMode;
    static constexpr std::string_view kLufsTarget      = meridian::pid::lufsTarget;
    static constexpr std::string_view kTargetProfile   = meridian::pid::targetProfileProperty;

    static constexpr const char* kDefaultTargetProfile = "R&B Major Label";

    MeridianAudioProcessor();
    ~MeridianAudioProcessor() override;

    // ---- AudioProcessor -----------------------------------------------------
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlock (juce::AudioBuffer<double>&, juce::MidiBuffer&) override;
    bool supportsDoublePrecisionProcessing() const override { return true; }

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override;

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // ---- Parameters ---------------------------------------------------------
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    juce::AudioProcessorValueTreeState& getAPVTS() noexcept { return apvts; }

    // ---- Undo / redo (message thread) ----------------------------------------
    /** Every parameter edit made through the APVTS is recorded. Edits are
        grouped into one undo step until 400 ms pass without a new change, so
        a knob drag is a single step. Loading a preset, a snapshot or host
        state clears the history (APVTS::replaceState). */
    juce::UndoManager& getUndoManager() noexcept { return undoManager; }
    bool undo();
    bool redo();
    bool canUndo() const { return undoManager.canUndo(); }
    bool canRedo() const { return undoManager.canRedo(); }

    /** Loudness-matched bypass compensation currently applied to a module (dB). */
    double getLoudnessMatchDb (ModuleId id) const noexcept;

    // ---- Latency ------------------------------------------------------------
    /** Sums the current latency of every module in the chain (thread-safe).
        Bypassed modules are included: their dry path stays delay-compensated so
        toggling bypass never changes the latency reported to the host. */
    int computeChainLatency() const noexcept;

    /** Recomputes the chain latency and reports it to the host if it changed.
        Message thread (or prepareToPlay). */
    void updateLatency();

    /** Latency most recently reported to the host. Note: the base class
        AudioProcessor::getLatencySamples() returns the same value. */
    int getTotalLatencySamples() const noexcept { return reportedLatency.load (std::memory_order_acquire); }

    // ---- Module chain -------------------------------------------------------
    meridian::ModuleBase* getModule (ModuleId id) noexcept;
    const meridian::ModuleBase* getModule (ModuleId id) const noexcept;

    /** Slot -> ModuleId mapping as last validated by the audio thread. */
    std::array<int, kNumModules> getModuleOrder() const noexcept;

    /** Writes a new slot order into the module_order parameters (message thread).
        Returns false (and changes nothing) if the order is not a permutation of 0..11. */
    bool setModuleOrder (const std::array<int, kNumModules>& newOrder);

    /** Moves the module currently in slot fromSlot to slot toSlot, shifting the others. */
    bool moveModule (int fromSlot, int toSlot);

    static bool isValidModuleOrder (const std::array<int, kNumModules>& order) noexcept;
    static std::array<int, kNumModules> defaultModuleOrder() noexcept;

    // ---- A/B/C/D snapshots (message thread) ---------------------------------
    /** Saves the live state into the active slot, then makes slot the live state.
        An empty target slot is initialised with the current live state. */
    void selectSnapshot (int slot);
    /** Saves the live state into slot without switching. */
    void storeSnapshot (int slot);
    /** Loads slot into the live state without saving the current live state first. */
    bool recallSnapshot (int slot);
    /** Copies slot (the live state if slot is active) to the internal clipboard. */
    void copySnapshot (int slot);
    /** Pastes the clipboard into slot (into the live state if slot is active). */
    bool pasteSnapshot (int slot);
    /** Copies one slot to another (live state is used for the active slot). */
    void copySnapshotTo (int sourceSlot, int destSlot);
    void clearSnapshot (int slot);
    bool hasSnapshot (int slot) const;
    bool hasClipboard() const;
    int getActiveSnapshot() const noexcept { return activeSnapshot.load(); }

    // ---- Target profile (non-automatable state property) --------------------
    juce::String getTargetProfile() const;
    void setTargetProfile (const juce::String& profileName);

    // ---- Metering (UI thread consumers) -------------------------------------
    bool popMeterData (MeterData& out) noexcept           { return meterQueue.pop (out); }
    bool popSpectrumFrame (SpectrumFrame& out) noexcept   { return spectrumQueue.pop (out); }
    void requestIntegratedLoudnessReset() noexcept        { resetLoudnessRequested.store (true); }
    double getCurrentSampleRate() const noexcept          { return currentSampleRate; }

    meridian::LockFreeQueue<MeterData, 512> meterQueue;
    meridian::LockFreeQueue<SpectrumFrame, 8> spectrumQueue;

    // ---- Analysis services (message-thread API; see each class for threading) ----
    meridian::IntelligentAssistant& getAssistant() noexcept    { return assistant; }
    meridian::ReferencePanel& getReferencePanel() noexcept     { return referencePanel; }
    meridian::CodecPreview& getCodecPreview() noexcept         { return codecPreview; }

    /** Seconds of processed output kept in the capture ring. */
    static constexpr double kCaptureSeconds = 20.0;

    /** Message thread: copies the most recent `seconds` of processed output
        (post output gain, before delta solo / reference A/B) into dest
        (resized to 2 x available samples). Returns the number of samples copied
        (0 before any audio has been processed). */
    int copyRecentAudio (juce::AudioBuffer<float>& dest, double seconds) const;

    /** Seconds of audio currently available in the capture ring (any thread). */
    double getCapturedSeconds() const noexcept;

    /** User profile directory: <userDocuments>/Meridian Mastering Suite/Profiles */
    static juce::File getUserProfileDirectory();
    /** User preset directory: <userDocuments>/Meridian Mastering Suite/Presets */
    static juce::File getUserPresetDirectory();

private:
    // ---- Per-module runtime state (indexed by ModuleId) ---------------------
    struct ModuleRuntime
    {
        meridian::DelayLine dryDelayL, dryDelayR;
        meridian::SmoothParam wet;
        bool skipped = false;      // module was fully dry last chunk (not processed)

        // Loudness-matched bypass: output/input mean-square tracked while the
        // module is fully engaged (~3 s), frozen while bypassed and applied
        // to the bypassed signal as a smoothed gain (50 ms, clamped +/-12 dB).
        double lmInMs = 0.0, lmOutMs = 0.0, lmSeconds = 0.0;
        meridian::SmoothParam lmGain;

        std::atomic<float>* bypass  = nullptr;
        std::atomic<float>* channel = nullptr;
        std::atomic<float>* wetDry  = nullptr;
        std::atomic<float>* delta   = nullptr;
    };

    template <typename SampleType>
    void processBlockInternal (juce::AudioBuffer<SampleType>& buffer);

    void processChunk (int numSamples) noexcept;
    void processModuleInSlot (int moduleIndex, int numSamples, bool captureDelta) noexcept;
    void applyLoudnessMatch (ModuleRuntime& rt, bool bypassed, bool settledActive, int numSamples) noexcept;
    void readModuleOrder() noexcept;
    void pushMeters (int numSamples) noexcept;
    void writeCapture (int numSamples) noexcept;

    void timerCallback() override;

    void setAbSlotParameter (int slot);
    void replaceLiveState (const juce::ValueTree& source, int activeSlotAfter);
    void ensureTargetProfileProperty();
    static bool isValidSlot (int slot) noexcept { return slot >= 0 && slot < kNumSnapshots; }

    // ---- State --------------------------------------------------------------
    // Declared before apvts: the APVTS keeps a pointer to it.
    juce::UndoManager undoManager { 30000, 30 };
    int undoActionsSeen = 0;
    juce::uint32 undoLastChangeMs = 0;

    juce::AudioProcessorValueTreeState apvts;

    std::array<std::unique_ptr<meridian::ModuleBase>, kNumModules> modules;
    std::array<ModuleRuntime, kNumModules> runtime;

    meridian::MeteringEngine meteringEngine;
    meridian::SpectrumAnalyzer spectrumAnalyzer;

    meridian::IntelligentAssistant assistant;
    meridian::ReferencePanel referencePanel;
    meridian::CodecPreview codecPreview;

    // Capture ring (allocated in prepareToPlay under captureLock; the audio
    // thread never takes the lock - prepareToPlay never runs concurrently with
    // processBlock - it only writes samples and publishes atomics).
    std::vector<float> captureL, captureR;
    bool hasBeenPrepared = false;   // set by prepareToPlay; guards releaseResources
    std::atomic<int> captureWritePos { 0 };
    std::atomic<int> captureFilled { 0 };
    int captureSize = 0;
    std::atomic<double> captureRate { 48000.0 };
    mutable juce::CriticalSection captureLock;

    /** Slot -> ModuleId, owned by the audio thread (validated copy of module_order params). */
    int moduleOrder[kNumModules] {};
    std::array<std::atomic<int>, kNumModules> publishedOrder {};
    std::array<std::atomic<float>*, kNumModules> orderParams {};

    std::atomic<float>* outputGainParam = nullptr;
    std::atomic<float>* lmBypassParam = nullptr;
    std::array<std::atomic<float>, kNumModules> lmAppliedDb {};
    meridian::SmoothParam outputGain;

    // Delta solo: the first slot with delta enabled is monitored at the output,
    // delayed by the latency of the modules after it.
    meridian::DelayLine deltaDelayL, deltaDelayR;
    int lastDeltaModule = -1;

    // Scratch buffers (allocated in prepareToPlay)
    std::vector<double> bufL, bufR, dryL, dryR, deltaL, deltaR;
    SpectrumFrame spectrumScratch;

    double currentSampleRate = 48000.0;
    int maxBlockSize = 0;
    std::int64_t samplePosition = 0;

    std::atomic<int> pendingLatency { 0 };
    std::atomic<int> reportedLatency { 0 };
    std::atomic<bool> latencyDirty { false };
    std::atomic<bool> resetLoudnessRequested { false };

    // Snapshots (never touched by the audio thread)
    std::array<juce::ValueTree, kNumSnapshots> snapshots;
    juce::ValueTree clipboard;
    std::atomic<int> activeSnapshot { 0 };
    mutable juce::CriticalSection snapshotLock;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MeridianAudioProcessor)
};
