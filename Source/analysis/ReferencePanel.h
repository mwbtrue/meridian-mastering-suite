#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include "common/AnalysisTypes.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace meridian
{

/**
    Reference-track comparison: loading, LUFS level matching, A/B playback,
    spectral difference and a "Match EQ" target curve.

    Threads
      loadReference()          message thread; decoding + analysis run on a
                               private std::thread (allocation is fine there).
      setMainLUFS/Crest        any thread (atomics) - the processor calls them
                               from the audio thread after metering.
      setMainFFT /
      setMainSpectrumFrame     message thread (editor timer); mutex-protected.
      renderReference()        AUDIO thread. Lock-free w.r.t. the loader: it
                               uses a SpinLock *try*-lock and simply leaves the
                               main mix untouched if the buffer is being
                               swapped (a few microseconds, once per load).
      everything else          message thread.

    Analysis of the reference (background thread)
      - decoded with juce::AudioFormatManager (WAV / AIFF / FLAC / Ogg / MP3),
        resampled (Lagrange) to the playback rate for A/B
      - loudness: MeteringEngine offline pass -> integrated LUFS, true peak
      - spectrum: 2048-pt Hann, hop 1024, power-averaged over the whole file,
        stereo power average, same dB scale as SpectrumAnalyzer
        (20 log10(4 |X| / N), full-scale sine = 0 dB)
      - width = 100 * RMS(S) / RMS(M) (%), crest = true peak - integrated (PLR)

    Main mix spectrum: exponential power average (tau ~ 3 s) of the
    SpectrumAnalyzer frames fed through setMainSpectrumFrame().

    diffCurve[k] = refDb[k] + levelMatch - mainDb[k], where levelMatch =
    mainLUFS - refLUFS once the main loudness is known (so the curve shows the
    tonal difference at matched loudness, which is what the A/B hears).

    getMatchEQ(amount): diffCurve minus its mean over 100 Hz - 10 kHz (pure
    tonal correction, no broadband gain), scaled by amount / 100, clamped to
    +/-4 dB per bin, then Gaussian-smoothed (sigma 2.5 bins, ~10-bin kernel).
*/
class ReferencePanel
{
public:
    static constexpr int kFftOrder = SpectrumFrame::fftOrder;
    static constexpr int kFftSize  = SpectrumFrame::fftSize;
    static constexpr int kNumBins  = SpectrumFrame::numBins;   // 1025
    static constexpr float kMaxMatchDb = 4.0f;

    struct SpectralComparison
    {
        float diffCurve[kNumBins] {};
        float mainLoudness = -150.0f, refLoudness = -150.0f;
        float mainWidth = 0.0f, refWidth = 0.0f;
        float mainCrest = 0.0f, refCrest = 0.0f;
    };

    struct MatchEQCurve
    {
        float gainPerBin[kNumBins] {};
        float amount_pct = 0.0f;
    };

    enum class LoadState : int { Empty = 0, Loading, Loaded, Failed };

    ReferencePanel();
    ~ReferencePanel();

    /** Playback / analysis rate of the host (call from prepareToPlay or the message thread
        before loading; a later change re-uses the already-resampled audio at its old rate). */
    void setPlaybackSampleRate (double sampleRate) noexcept { playbackRate.store (sampleRate > 0.0 ? sampleRate : 48000.0); }

    /** Starts decoding + analysis on a background thread. */
    void loadReference (const juce::File& file);

    bool isLoaded() const noexcept { return state.load() == static_cast<int> (LoadState::Loaded); }
    LoadState getLoadState() const noexcept { return static_cast<LoadState> (state.load()); }
    juce::String getReferenceName() const;
    juce::String getLastError() const;

    void setMainLUFS (float lufs) noexcept   { mainLufs.store (lufs, std::memory_order_relaxed); }
    void setMainCrest (float plr) noexcept   { mainCrest.store (plr, std::memory_order_relaxed); }

    /** refLUFS - mainLUFS (dB). Reference playback is attenuated by this amount. */
    float getLevelMatchGain_dB() const noexcept;

    /** A/B: when true, renderReference() replaces the output with the level-matched reference. */
    void setListeningToReference (bool shouldListen) noexcept { listening.store (shouldListen); }
    bool isListeningToReference() const noexcept { return listening.load(); }

    /** Audio thread. If listening and loaded, writes the level-matched reference
        (looping) into L/R and returns true; otherwise leaves L/R untouched and
        returns false. */
    bool renderReference (double* L, double* R, int numSamples) noexcept;

    /** Same as renderReference() but for float buffers; writes silence when not listening. */
    void getOutputBuffer (float* L, float* R, int numSamples) noexcept;

    /** Message thread: one main-mix magnitude curve in dB (SpectrumAnalyzer scale). */
    void setMainFFT (const float* binsDb, int numBins);

    /** Message thread: feeds a full frame (also updates the main width estimate). */
    void setMainSpectrumFrame (const SpectrumFrame& frame);

    SpectralComparison getSpectralComparison() const;
    MatchEQCurve getMatchEQ (float amountPct) const;

    /** Reference spectrum (dB, level-matched when the main loudness is known). */
    std::array<float, kNumBins> getReferenceSpectrum() const;

private:
    void loaderThread (juce::File file);
    void joinLoader();

    std::thread loader;
    std::atomic<bool> cancelLoad { false };
    std::atomic<int> state { static_cast<int> (LoadState::Empty) };
    std::atomic<double> playbackRate { 48000.0 };

    // Playback buffer (swapped under spin lock; audio thread only try-locks).
    juce::SpinLock playbackLock;
    std::unique_ptr<juce::AudioBuffer<float>> playback;
    int playhead = 0;
    std::atomic<bool> listening { false };
    float appliedGain = 1.0f;

    std::atomic<float> mainLufs { -150.0f }, mainCrest { 0.0f };
    std::atomic<float> refLufs { -150.0f };

    mutable std::mutex dataMutex;
    std::array<double, kNumBins> mainPower {};
    bool mainPowerValid = false;
    double mainSidePower = 0.0, mainMidPower = 0.0;
    std::array<float, kNumBins> refDb {};
    float refWidth = 0.0f, refCrest = 0.0f;
    juce::String refName, lastError;
};

} // namespace meridian
