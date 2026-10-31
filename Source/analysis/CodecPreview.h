#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <atomic>
#include <memory>
#include <mutex>

namespace meridian
{

/**
    Lossy-codec round-trip preview (AAC 256 kb/s, MP3 320 kb/s, Opus 160 kb/s).

    Everything here runs on the message thread (setInputBuffer / startPreview /
    getResult) or on the private worker thread (file I/O, ffmpeg, measurement);
    nothing is ever called from the audio thread.

    Round trip (worker thread):
        snapshot -> <tmp>/meridian_codec_<id>_in.wav (32-bit float)
                 -> ffmpeg encode   (aac 256k .m4a | libmp3lame 320k .mp3 | libopus 160k .opus)
                 -> ffmpeg decode   (pcm_f32le, resampled back to the source rate)
                 -> read back, measure, delete all temp files

    Measurements:
        truePeakPreEncode_dBTP   4x TruePeakDetector on the snapshot
        truePeakPostEncode_dBTP  4x TruePeakDetector on the decoded signal
        levelDiff_dB             RMS(decoded) - RMS(snapshot), both channels
    (Codec delay / priming samples do not affect any of these measures.)

    Opus is encoded at 160 kb/s, its highest commonly deployed streaming rate
    (Opus is always coded at 48 kHz internally; the decode resamples back).

    ffmpeg is looked up on PATH (plus the usual Homebrew / Linux locations).
    If it cannot be started, the result reads codecName = "ffmpeg not found",
    done = true. If ffmpeg lacks the encoder, codecName says so.
*/
class CodecPreview
{
public:
    enum Codec : int { AAC = 0, MP3 = 1, Opus = 2, NumCodecs };

    struct CodecResult
    {
        float truePeakPostEncode_dBTP = -150.0f;
        float truePeakPreEncode_dBTP  = -150.0f;
        float levelDiff_dB            = 0.0f;
        juce::String codecName;
        bool done = false;
        bool success = false;
    };

    CodecPreview();
    ~CodecPreview();

    /** Message thread: stores a copy of processed audio (only the first two channels are used). */
    void setInputBuffer (juce::AudioBuffer<float> buf, double sampleRate);

    /** Message thread: launches the round trip for codecIndex (0 = AAC, 1 = MP3, 2 = Opus).
        Ignored while a preview is running. */
    void startPreview (int codecIndex);

    bool isRunning() const noexcept { return running.load (std::memory_order_acquire); }

    CodecResult getResult() const;

    /** Stops the worker (kills a running ffmpeg) and waits for it. */
    void cancel();

    static juce::String getCodecDisplayName (int codecIndex);

    /** Measures the 4x true peak (dBTP) of up to two channels. */
    static float measureTruePeakDb (const juce::AudioBuffer<float>& buffer);

    /** RMS level over all samples of up to two channels (dBFS). */
    static float measureRmsDb (const juce::AudioBuffer<float>& buffer);

private:
    class Worker;
    friend class Worker;

    void runRoundTrip (int codecIndex, juce::Thread& thread);
    void publish (const CodecResult& r);
    static juce::String findFfmpeg();
    static bool runProcess (const juce::StringArray& args, juce::Thread& thread, juce::String& output);

    std::unique_ptr<Worker> worker;
    std::atomic<bool> running { false };

    mutable std::mutex inputMutex;
    juce::AudioBuffer<float> input;
    double inputSampleRate = 48000.0;

    mutable std::mutex resultMutex;
    CodecResult result;

    JUCE_DECLARE_NON_COPYABLE (CodecPreview)
};

} // namespace meridian
