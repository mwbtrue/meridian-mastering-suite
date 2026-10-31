#include "CodecPreview.h"

#include "common/TruePeakDetector.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace meridian
{

namespace
{
    struct CodecSpec
    {
        const char* displayName;
        const char* encoder;
        const char* bitrate;
        const char* extension;
    };

    constexpr CodecSpec kCodecs[CodecPreview::NumCodecs] {
        { "AAC 256 kb/s", "aac",        "256k", ".m4a"  },
        { "MP3 320 kb/s", "libmp3lame", "320k", ".mp3"  },
        { "Opus 160 kb/s", "libopus",   "160k", ".opus" },
    };

    constexpr int kProcessPollMs = 50;
    constexpr int kProcessTimeoutMs = 120000;

    float toDb (double linear) noexcept
    {
        return linear > 1.0e-15 ? static_cast<float> (20.0 * std::log10 (linear)) : -150.0f;
    }

    struct TempFileSet
    {
        juce::File in, encoded, decoded;

        ~TempFileSet()
        {
            for (auto* f : { &in, &encoded, &decoded })
                if (f->existsAsFile())
                    f->deleteFile();
        }
    };
}

//==============================================================================
class CodecPreview::Worker final : public juce::Thread
{
public:
    Worker (CodecPreview& o, int codec) : juce::Thread ("Meridian Codec Preview"), owner (o), codecIndex (codec) {}

    void run() override
    {
        owner.runRoundTrip (codecIndex, *this);
        owner.running.store (false, std::memory_order_release);
    }

private:
    CodecPreview& owner;
    const int codecIndex;
};

//==============================================================================
CodecPreview::CodecPreview() = default;

CodecPreview::~CodecPreview()
{
    cancel();
}

void CodecPreview::setInputBuffer (juce::AudioBuffer<float> buf, double sampleRate)
{
    const std::lock_guard<std::mutex> lock (inputMutex);
    input = std::move (buf);
    inputSampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
}

void CodecPreview::startPreview (int codecIndex)
{
    if (isRunning())
        return;

    if (worker != nullptr)
    {
        worker->stopThread (2000);
        worker.reset();
    }

    codecIndex = juce::jlimit (0, NumCodecs - 1, codecIndex);

    CodecResult pending;
    pending.codecName = getCodecDisplayName (codecIndex);
    pending.done = false;
    publish (pending);

    running.store (true, std::memory_order_release);
    worker = std::make_unique<Worker> (*this, codecIndex);
    worker->startThread (juce::Thread::Priority::low);
}

CodecPreview::CodecResult CodecPreview::getResult() const
{
    const std::lock_guard<std::mutex> lock (resultMutex);
    return result;
}

void CodecPreview::cancel()
{
    if (worker != nullptr)
    {
        worker->signalThreadShouldExit();
        worker->stopThread (10000);
        worker.reset();
    }

    running.store (false, std::memory_order_release);
}

juce::String CodecPreview::getCodecDisplayName (int codecIndex)
{
    return kCodecs[juce::jlimit (0, NumCodecs - 1, codecIndex)].displayName;
}

void CodecPreview::publish (const CodecResult& r)
{
    const std::lock_guard<std::mutex> lock (resultMutex);
    result = r;
}

//==============================================================================
float CodecPreview::measureTruePeakDb (const juce::AudioBuffer<float>& buffer)
{
    TruePeakDetector detector;
    detector.prepare (4, 12, 8.0);
    detector.setFactor (4);

    const int channels = std::min (2, buffer.getNumChannels());
    const int n = buffer.getNumSamples();
    const int tail = detector.getLatencySamples();
    double peak = 0.0;

    for (int ch = 0; ch < channels; ++ch)
    {
        const float* x = buffer.getReadPointer (ch);
        for (int i = 0; i < n; ++i)
            peak = std::max (peak, detector.process (ch, static_cast<double> (x[i])));

        // Flush the interpolator so the last samples are evaluated too.
        for (int i = 0; i < tail; ++i)
            peak = std::max (peak, detector.process (ch, 0.0));
    }

    return toDb (peak);
}

float CodecPreview::measureRmsDb (const juce::AudioBuffer<float>& buffer)
{
    const int channels = std::min (2, buffer.getNumChannels());
    const int n = buffer.getNumSamples();
    if (channels == 0 || n == 0)
        return -150.0f;

    double sum = 0.0;
    for (int ch = 0; ch < channels; ++ch)
    {
        const float* x = buffer.getReadPointer (ch);
        for (int i = 0; i < n; ++i)
            sum += static_cast<double> (x[i]) * static_cast<double> (x[i]);
    }

    return toDb (std::sqrt (sum / (static_cast<double> (n) * channels)));
}

//==============================================================================
juce::String CodecPreview::findFfmpeg()
{
    const juce::StringArray candidates {
       #if JUCE_WINDOWS
        "ffmpeg.exe", "C:\\ffmpeg\\bin\\ffmpeg.exe"
       #else
        "ffmpeg", "/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg", "/usr/bin/ffmpeg"
       #endif
    };

    for (const auto& c : candidates)
    {
        juce::ChildProcess probe;
        if (probe.start (juce::StringArray { c, "-hide_banner", "-version" }, juce::ChildProcess::wantStdOut))
        {
            const auto out = probe.readAllProcessOutput();
            probe.waitForProcessToFinish (5000);
            if (out.containsIgnoreCase ("ffmpeg"))
                return c;
        }
    }

    return {};
}

bool CodecPreview::runProcess (const juce::StringArray& args, juce::Thread& thread, juce::String& output)
{
    juce::ChildProcess proc;
    if (! proc.start (args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
        return false;

    int waited = 0;
    while (! proc.waitForProcessToFinish (kProcessPollMs))
    {
        waited += kProcessPollMs;
        if (thread.threadShouldExit() || waited > kProcessTimeoutMs)
        {
            proc.kill();
            output = thread.threadShouldExit() ? "cancelled" : "timed out";
            return false;
        }
    }

    // ffmpeg runs with -loglevel error, so the remaining output is tiny.
    output = proc.readAllProcessOutput();
    return proc.getExitCode() == 0;
}

void CodecPreview::runRoundTrip (int codecIndex, juce::Thread& thread)
{
    const auto& spec = kCodecs[codecIndex];

    CodecResult r;
    r.codecName = spec.displayName;

    juce::AudioBuffer<float> source;
    double sampleRate = 48000.0;
    {
        const std::lock_guard<std::mutex> lock (inputMutex);
        source.makeCopyOf (input);
        sampleRate = inputSampleRate;
    }

    auto fail = [&] (const juce::String& why)
    {
        r.codecName = why;
        r.done = true;
        r.success = false;
        publish (r);
    };

    if (source.getNumSamples() == 0 || source.getNumChannels() == 0)
        return fail ("No audio captured - play the mix first");

    if (source.getNumChannels() == 1)
    {
        juce::AudioBuffer<float> stereo (2, source.getNumSamples());
        stereo.copyFrom (0, 0, source, 0, 0, source.getNumSamples());
        stereo.copyFrom (1, 0, source, 0, 0, source.getNumSamples());
        source = std::move (stereo);
    }
    else if (source.getNumChannels() > 2)
    {
        source.setSize (2, source.getNumSamples(), true, false, false);
    }

    r.truePeakPreEncode_dBTP = measureTruePeakDb (source);

    const auto ffmpeg = findFfmpeg();
    if (ffmpeg.isEmpty())
        return fail ("ffmpeg not found");

    if (thread.threadShouldExit())
        return fail ("Cancelled");

    const auto tmpDir = juce::File::getSpecialLocation (juce::File::tempDirectory);
    const auto id = juce::String::toHexString (juce::Random::getSystemRandom().nextInt64());

    TempFileSet files;
    files.in      = tmpDir.getChildFile ("meridian_codec_" + id + "_in.wav");
    files.encoded = tmpDir.getChildFile ("meridian_codec_" + id + "_enc" + spec.extension);
    files.decoded = tmpDir.getChildFile ("meridian_codec_" + id + "_dec.wav");

    // ---- Write the snapshot ---------------------------------------------------
    {
        juce::WavAudioFormat wav;
        auto stream = files.in.createOutputStream();
        if (stream == nullptr)
            return fail ("Cannot write temp file");

        // JUCE 8.0.4 API: the writer takes ownership of the stream on success.
        std::unique_ptr<juce::AudioFormatWriter> writer (
            wav.createWriterFor (stream.get(), sampleRate, 2, 32, {}, 0));
        if (writer == nullptr)
            return fail ("Cannot create WAV writer");
        stream.release();

        if (! writer->writeFromAudioSampleBuffer (source, 0, source.getNumSamples()))
            return fail ("Cannot write temp file");
    }

    // ---- Encode ------------------------------------------------------------------
    juce::String log;
    juce::StringArray encodeArgs { ffmpeg, "-hide_banner", "-nostdin", "-loglevel", "error", "-y",
                                   "-i", files.in.getFullPathName(),
                                   "-c:a", spec.encoder, "-b:a", spec.bitrate };
    if (codecIndex == Opus)
        encodeArgs.addArray (juce::StringArray { "-ar", "48000" });
    encodeArgs.add (files.encoded.getFullPathName());

    if (! runProcess (encodeArgs, thread, log))
    {
        if (thread.threadShouldExit())
            return fail ("Cancelled");
        return fail (juce::String (spec.displayName) + ": encoder unavailable ("
                     + log.upToFirstOccurrenceOf ("\n", false, false).substring (0, 80) + ")");
    }

    // ---- Decode ------------------------------------------------------------------
    const juce::StringArray decodeArgs { ffmpeg, "-hide_banner", "-nostdin", "-loglevel", "error", "-y",
                                         "-i", files.encoded.getFullPathName(),
                                         "-c:a", "pcm_f32le", "-ar", juce::String (juce::roundToInt (sampleRate)),
                                         "-ac", "2", files.decoded.getFullPathName() };

    if (! runProcess (decodeArgs, thread, log))
        return fail (thread.threadShouldExit() ? juce::String ("Cancelled")
                                               : juce::String (spec.displayName) + ": decode failed");

    // ---- Read back ---------------------------------------------------------------
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (files.decoded));
    if (reader == nullptr || reader->lengthInSamples <= 0)
        return fail ("Cannot read decoded audio");

    const int decodedLength = static_cast<int> (std::min<juce::int64> (reader->lengthInSamples,
                                                                       std::numeric_limits<int>::max() / 2));
    juce::AudioBuffer<float> decoded (2, decodedLength);
    reader->read (&decoded, 0, decodedLength, 0, true, true);
    reader.reset();

    r.truePeakPostEncode_dBTP = measureTruePeakDb (decoded);
    r.levelDiff_dB = measureRmsDb (decoded) - measureRmsDb (source);
    r.done = true;
    r.success = true;
    publish (r);
}

} // namespace meridian
