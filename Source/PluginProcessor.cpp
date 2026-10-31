#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <algorithm>
#include <cmath>

using namespace meridian;

namespace
{
    using APVTS = juce::AudioProcessorValueTreeState;
    using FloatRange = juce::NormalisableRange<float>;

    constexpr int kParamVersion = 1;

    // ---- Value formatting -------------------------------------------------------
    juce::String formatHz (float v, int)
    {
        return v >= 1000.0f ? juce::String (v / 1000.0f, v >= 10000.0f ? 1 : 2) + " kHz"
                            : juce::String (v, v < 100.0f ? 1 : 0) + " Hz";
    }

    juce::String formatDb (float v, int)        { return juce::String (v, 1) + " dB"; }
    juce::String formatDbTp (float v, int)      { return juce::String (v, 2) + " dBTP"; }
    juce::String formatDbFs (float v, int)      { return juce::String (v, 1) + " dBFS"; }
    juce::String formatLufs (float v, int)      { return juce::String (v, 1) + " LUFS"; }
    juce::String formatMs (float v, int)        { return juce::String (v, v < 10.0f ? 2 : 1) + " ms"; }
    juce::String formatPercent (float v, int)   { return juce::String (juce::roundToInt (v)) + " %"; }
    juce::String formatRatio (float v, int)     { return juce::String (v, 2) + ":1"; }
    juce::String formatOct (float v, int)       { return juce::String (v, 2) + " oct"; }
    juce::String formatPlain (float v, int)     { return juce::String (v, 2); }

    float parseLeadingNumber (const juce::String& text)
    {
        const auto t = text.trim();
        float value = t.getFloatValue();

        if (t.endsWithIgnoreCase ("khz") || t.endsWithIgnoreCase ("k"))
            value *= 1000.0f;

        return value;
    }

    // ---- Ranges -------------------------------------------------------------------
    FloatRange linRange (float lo, float hi, float step = 0.0f)
    {
        return FloatRange (lo, hi, step);
    }

    /** Logarithmic-feel range: skewed so the geometric mean sits at mid-travel. */
    FloatRange logRange (float lo, float hi, float step = 0.0f)
    {
        FloatRange r (lo, hi, step);
        r.setSkewForCentre (std::sqrt (lo * hi));
        return r;
    }

    // ---- Layout builder -------------------------------------------------------------
    struct LayoutBuilder
    {
        APVTS::ParameterLayout& layout;

        void addFloat (const juce::String& id, const juce::String& name, FloatRange range, float def,
                       const juce::String& label, juce::String (*fmt) (float, int), bool automatable = true)
        {
            auto attrs = juce::AudioParameterFloatAttributes()
                             .withLabel (label)
                             .withStringFromValueFunction (fmt)
                             .withValueFromStringFunction ([] (const juce::String& t) { return parseLeadingNumber (t); })
                             .withAutomatable (automatable);

            layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { id, kParamVersion },
                                                                     name, range, def, attrs));
        }

        void addBool (const juce::String& id, const juce::String& name, bool def, bool automatable = true)
        {
            layout.add (std::make_unique<juce::AudioParameterBool> (
                juce::ParameterID { id, kParamVersion }, name, def,
                juce::AudioParameterBoolAttributes().withAutomatable (automatable)));
        }

        void addChoice (const juce::String& id, const juce::String& name, const juce::StringArray& items,
                        int defIndex, bool automatable = true)
        {
            layout.add (std::make_unique<juce::AudioParameterChoice> (
                juce::ParameterID { id, kParamVersion }, name, items, defIndex,
                juce::AudioParameterChoiceAttributes().withAutomatable (automatable)));
        }

        void addInt (const juce::String& id, const juce::String& name, int lo, int hi, int def, bool automatable = true)
        {
            layout.add (std::make_unique<juce::AudioParameterInt> (
                juce::ParameterID { id, kParamVersion }, name, lo, hi, def,
                juce::AudioParameterIntAttributes().withAutomatable (automatable)));
        }
    };

    const juce::StringArray kChannelModes5 { "Stereo", "Mid", "Side", "Left", "Right" };
    const juce::StringArray kChannelModes3 { "Stereo", "Mid", "Side" };

    juce::String moduleLabel (int index)
    {
        const auto n = kModuleNames[(size_t) index];
        return juce::String (n.data(), n.size());
    }

    template <typename SampleType>
    inline double toDouble (SampleType s) noexcept { return static_cast<double> (s); }
}

//==============================================================================
// Parameter layout
//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout MeridianAudioProcessor::createParameterLayout()
{
    APVTS::ParameterLayout layout;
    LayoutBuilder b { layout };
    using namespace meridian::pid;
    namespace f = meridian::pid::field;

    // ---- InputModule ------------------------------------------------------------
    b.addFloat (str (inputTrim), "Input Trim", linRange (-24.0f, 24.0f, 0.01f), 0.0f, "dB", formatDb);
    b.addBool  (str (inputPolarity), "Input Polarity Invert", false);
    b.addBool  (str (inputDcBlock), "Input DC Block", true);

    // ---- CorrectiveEQ -----------------------------------------------------------
    {
        constexpr float defFreqs[kNumCeqBands] { 22.0f, 200.0f, 500.0f, 1000.0f, 3000.0f, 6000.0f, 10000.0f, 16000.0f };
        const juce::StringArray types { "Bell", "Low Shelf", "High Shelf", "Notch", "Tilt",
                                        "High Pass", "Low Pass", "Elliptic High Pass" };
        juce::StringArray slopes;
        for (int s : choices::ceqSlopesDbPerOct)
            slopes.add (juce::String (s) + " dB/oct");

        for (int i = 0; i < kNumCeqBands; ++i)
        {
            const juce::String n = "CEQ Band " + juce::String (i + 1) + " ";
            b.addFloat  (bandParam (ceqBandPrefix, i, f::freq), n + "Freq", logRange (20.0f, 20000.0f, 0.01f), defFreqs[i], "Hz", formatHz);
            b.addFloat  (bandParam (ceqBandPrefix, i, f::gain), n + "Gain", linRange (-18.0f, 18.0f, 0.01f), 0.0f, "dB", formatDb);
            b.addFloat  (bandParam (ceqBandPrefix, i, f::q), n + "Q", logRange (0.1f, 10.0f, 0.001f), 0.7f, "", formatPlain);
            b.addChoice (bandParam (ceqBandPrefix, i, f::type), n + "Type", types, 0);
            b.addChoice (bandParam (ceqBandPrefix, i, f::slope), n + "Slope", slopes, choices::ceqDefaultSlopeIndex);
            b.addBool   (bandParam (ceqBandPrefix, i, f::enabled), n + "Enabled", true);
            b.addChoice (bandParam (ceqBandPrefix, i, f::channel), n + "Channel", kChannelModes5, 0);
        }

        b.addChoice (str (ceqPhaseMode), "CEQ Phase Mode", { "Minimum Phase", "Linear Phase", "Natural Phase" }, 0);
    }

    // ---- LowEndControl ----------------------------------------------------------
    b.addFloat (str (lecMonoFreq), "Low End Mono Freq", logRange (40.0f, 200.0f, 0.1f), 120.0f, "Hz", formatHz);
    b.addBool  (str (lecMonoEnabled), "Low End Mono Enabled", true);
    b.addBool  (str (lecDynEnabled), "Low End Dynamic Sub Enabled", true);
    b.addFloat (str (lecDynThreshold), "Low End Dynamic Threshold", linRange (-30.0f, 0.0f, 0.1f), -12.0f, "dBFS", formatDbFs);
    b.addFloat (str (lecDynMaxGr), "Low End Dynamic Max GR", linRange (-6.0f, 0.0f, 0.1f), -3.0f, "dB", formatDb);

    // ---- DynamicEQ --------------------------------------------------------------
    {
        constexpr float defFreqs[kNumDeqBands] { 275.0f, 3500.0f, 7000.0f, 120.0f, 5000.0f, 8000.0f };

        for (int i = 0; i < kNumDeqBands; ++i)
        {
            const juce::String n = "DEQ Band " + juce::String (i + 1) + " ";
            b.addFloat  (bandParam (deqBandPrefix, i, f::freq), n + "Freq", logRange (20.0f, 20000.0f, 0.01f), defFreqs[i], "Hz", formatHz);
            b.addFloat  (bandParam (deqBandPrefix, i, f::q), n + "Q", logRange (0.3f, 8.0f, 0.001f), 1.0f, "", formatPlain);
            b.addFloat  (bandParam (deqBandPrefix, i, f::threshold), n + "Threshold", linRange (-60.0f, 0.0f, 0.1f), -24.0f, "dBFS", formatDbFs);
            b.addFloat  (bandParam (deqBandPrefix, i, f::maxGain), n + "Max Gain", linRange (-12.0f, 6.0f, 0.01f), -2.0f, "dB", formatDb);
            b.addFloat  (bandParam (deqBandPrefix, i, f::attack), n + "Attack", logRange (0.1f, 200.0f, 0.01f), 5.0f, "ms", formatMs);
            b.addFloat  (bandParam (deqBandPrefix, i, f::release), n + "Release", logRange (1.0f, 2000.0f, 0.1f), 60.0f, "ms", formatMs);
            b.addChoice (bandParam (deqBandPrefix, i, f::channel), n + "Channel", kChannelModes3, 0);
            b.addBool   (bandParam (deqBandPrefix, i, f::enabled), n + "Enabled", true);
        }

        b.addBool (str (deqResonEnabled), "DEQ Resonance Suppressor", true);
    }

    // ---- GlueCompressor ---------------------------------------------------------
    b.addFloat  (str (glueRatio), "Glue Ratio", logRange (1.1f, 10.0f, 0.01f), 2.0f, ":1", formatRatio);
    b.addFloat  (str (glueThreshold), "Glue Threshold", linRange (-40.0f, 0.0f, 0.1f), -18.0f, "dBFS", formatDbFs);
    b.addFloat  (str (glueAttack), "Glue Attack", logRange (0.1f, 100.0f, 0.01f), 30.0f, "ms", formatMs);
    b.addFloat  (str (glueRelease), "Glue Release", logRange (10.0f, 2000.0f, 0.1f), 200.0f, "ms", formatMs);
    b.addBool   (str (glueAutoRelease), "Glue Auto Release", true);
    b.addFloat  (str (glueKnee), "Glue Knee", linRange (0.0f, 12.0f, 0.1f), 4.0f, "dB", formatDb);
    b.addFloat  (str (glueMakeup), "Glue Makeup", linRange (-12.0f, 12.0f, 0.01f), 0.0f, "dB", formatDb);
    b.addFloat  (str (glueMix), "Glue Mix", linRange (0.0f, 100.0f, 0.1f), 100.0f, "%", formatPercent);
    b.addFloat  (str (glueScHpf), "Glue Sidechain HPF", logRange (20.0f, 300.0f, 0.1f), 100.0f, "Hz", formatHz);
    b.addFloat  (str (glueLookahead), "Glue Lookahead", linRange (0.0f, 10.0f, 0.01f), 0.0f, "ms", formatMs);
    b.addChoice (str (glueScChannel), "Glue Sidechain Source", { "L+R", "Mid" }, 0);

    // ---- MultibandDynamics ------------------------------------------------------
    {
        constexpr float defXover[kNumMbdXovers]   { 120.0f, 800.0f, 5000.0f };
        constexpr float defRatio[kNumMbdBands]    { 2.0f, 1.5f, 1.5f, 2.0f };
        constexpr float defAttack[kNumMbdBands]   { 30.0f, 20.0f, 10.0f, 5.0f };
        constexpr float defRelease[kNumMbdBands]  { 150.0f, 120.0f, 80.0f, 50.0f };

        for (int i = 0; i < kNumMbdXovers; ++i)
            b.addFloat (xoverParam (mbdXoverPrefix, i), "MBD Crossover " + juce::String (i + 1),
                        logRange (60.0f, 16000.0f, 0.1f), defXover[i], "Hz", formatHz);

        for (int i = 0; i < kNumMbdBands; ++i)
        {
            const juce::String n = "MBD Band " + juce::String (i + 1) + " ";
            b.addFloat (bandParam (mbdBandPrefix, i, f::ratio), n + "Ratio", logRange (1.0f, 10.0f, 0.01f), defRatio[i], ":1", formatRatio);
            b.addFloat (bandParam (mbdBandPrefix, i, f::threshold), n + "Threshold", linRange (-40.0f, 0.0f, 0.1f), -18.0f, "dBFS", formatDbFs);
            b.addFloat (bandParam (mbdBandPrefix, i, f::attack), n + "Attack", logRange (0.1f, 200.0f, 0.01f), defAttack[i], "ms", formatMs);
            b.addFloat (bandParam (mbdBandPrefix, i, f::release), n + "Release", logRange (5.0f, 2000.0f, 0.1f), defRelease[i], "ms", formatMs);
            b.addFloat (bandParam (mbdBandPrefix, i, f::knee), n + "Knee", linRange (0.0f, 12.0f, 0.1f), 4.0f, "dB", formatDb);
            b.addFloat (bandParam (mbdBandPrefix, i, f::makeup), n + "Makeup", linRange (-12.0f, 12.0f, 0.01f), 0.0f, "dB", formatDb);
            b.addFloat (bandParam (mbdBandPrefix, i, f::upwardThresh), n + "Upward Threshold", linRange (-60.0f, -20.0f, 0.1f), -50.0f, "dBFS", formatDbFs);
            b.addFloat (bandParam (mbdBandPrefix, i, f::upwardRatio), n + "Upward Ratio", logRange (1.0f, 4.0f, 0.01f), 1.0f, ":1", formatRatio);
            b.addBool  (bandParam (mbdBandPrefix, i, f::enabled), n + "Enabled", true);
        }
    }

    // ---- ToneEQ -----------------------------------------------------------------
    {
        const juce::StringArray bassFreqs   { "20 Hz", "30 Hz", "60 Hz", "100 Hz" };
        const juce::StringArray attenFreqs  { "100 Hz", "200 Hz", "300 Hz", "400 Hz" };
        const juce::StringArray midFreqs    { "3 kHz", "4 kHz", "5 kHz", "8 kHz", "10 kHz", "12 kHz", "16 kHz" };
        const juce::StringArray trebleFreqs { "8 kHz", "10 kHz", "12 kHz", "14 kHz", "16 kHz" };

        b.addChoice (str (teqPultecBassFreq), "Pultec Bass Freq", bassFreqs, 2);
        b.addFloat  (str (teqPultecBassBoost), "Pultec Bass Boost", linRange (0.0f, 10.0f, 0.01f), 1.0f, "dB", formatDb);
        b.addFloat  (str (teqPultecBassAtten), "Pultec Bass Atten", linRange (0.0f, 10.0f, 0.01f), 0.0f, "dB", formatDb);
        b.addChoice (str (teqPultecAttenFreq), "Pultec Atten Freq", attenFreqs, 1);
        b.addFloat  (str (teqPultecAttenBw), "Pultec Atten Bandwidth", linRange (0.5f, 2.0f, 0.01f), 1.0f, "oct", formatOct);
        b.addFloat  (str (teqPultecMidBoost), "Pultec Mid Boost", linRange (0.0f, 10.0f, 0.01f), 0.75f, "dB", formatDb);
        b.addChoice (str (teqPultecMidFreq), "Pultec Mid Freq", midFreqs, 0);
        b.addFloat  (str (teqPultecMidBw), "Pultec Mid Bandwidth", linRange (0.5f, 2.0f, 0.01f), 1.92f, "oct", formatOct);
        b.addFloat  (str (teqPultecTrebleBoost), "Pultec Treble Boost", linRange (0.0f, 10.0f, 0.01f), 1.5f, "dB", formatDb);
        b.addChoice (str (teqPultecTrebleFreq), "Pultec Treble Freq", trebleFreqs, 3);
        b.addFloat  (str (teqPultecTrebleBw), "Pultec Treble Bandwidth", linRange (0.5f, 2.0f, 0.01f), 1.0f, "oct", formatOct);
        b.addFloat  (str (teqBaxandallBass), "Baxandall Bass", linRange (-6.0f, 6.0f, 0.01f), 0.0f, "dB", formatDb);
        b.addFloat  (str (teqBaxandallBassFreq), "Baxandall Bass Freq", logRange (50.0f, 500.0f, 0.1f), 100.0f, "Hz", formatHz);
        b.addFloat  (str (teqBaxandallTreble), "Baxandall Treble", linRange (-6.0f, 6.0f, 0.01f), 0.0f, "dB", formatDb);
        b.addFloat  (str (teqBaxandallTrebleFreq), "Baxandall Treble Freq", logRange (2000.0f, 15000.0f, 1.0f), 10000.0f, "Hz", formatHz);
        b.addFloat  (str (teqPassiveMidGain), "Passive Mid Gain", linRange (-6.0f, 6.0f, 0.01f), 0.5f, "dB", formatDb);
        b.addFloat  (str (teqPassiveMidFreq), "Passive Mid Freq", logRange (200.0f, 8000.0f, 0.1f), 200.0f, "Hz", formatHz);
        b.addFloat  (str (teqPassiveMidQ), "Passive Mid Q", linRange (0.3f, 1.5f, 0.001f), 0.5f, "", formatPlain);
        b.addFloat  (str (teqSideAirGain), "Side Air Gain", linRange (-6.0f, 6.0f, 0.01f), 1.0f, "dB", formatDb);
        b.addFloat  (str (teqSideAirFreq), "Side Air Freq", logRange (8000.0f, 20000.0f, 1.0f), 10000.0f, "Hz", formatHz);
        b.addFloat  (str (teqColour), "Tone EQ Analog Colour", linRange (0.0f, 100.0f, 0.1f), 0.0f, "%", formatPercent);
    }

    // ---- Saturation -------------------------------------------------------------
    {
        b.addChoice (str (satModel), "Saturation Model", { "Tape", "Tube", "Transformer" }, 0);
        b.addFloat  (str (satDrive), "Saturation Drive", linRange (0.0f, 24.0f, 0.01f), 2.0f, "dB", formatDb);
        b.addFloat  (str (satBias), "Saturation Bias", linRange (-1.0f, 1.0f, 0.001f), 0.0f, "", formatPlain);
        b.addFloat  (str (satMix), "Saturation Mix", linRange (0.0f, 100.0f, 0.1f), 40.0f, "%", formatPercent);
        b.addChoice (str (satOversampling), "Saturation Oversampling", { "1x", "2x", "4x", "8x", "16x" }, 2);
        b.addBool   (str (satMultibandEnabled), "Saturation Multiband", false);

        constexpr float defXover[kNumSatXovers] { 200.0f, 2000.0f };
        for (int i = 0; i < kNumSatXovers; ++i)
            b.addFloat (bandParam (satBandPrefix, i, f::freq), "Saturation Crossover " + juce::String (i + 1),
                        logRange (80.0f, 8000.0f, 0.1f), defXover[i], "Hz", formatHz);

        for (int i = 0; i < kNumSatBands; ++i)
            b.addFloat (bandParam (satBandPrefix, i, f::drive), "Saturation Band " + juce::String (i + 1) + " Drive",
                        linRange (0.0f, 24.0f, 0.01f), 2.0f, "dB", formatDb);

        b.addBool (str (satAutoGain), "Saturation Auto Gain", true);
    }

    // ---- StereoImager -----------------------------------------------------------
    {
        constexpr float defWidth[kNumImgBands] { 0.0f, 100.0f, 110.0f, 120.0f };
        constexpr float defXover[kNumImgXovers] { 120.0f, 1000.0f, 8000.0f };

        for (int i = 0; i < kNumImgXovers; ++i)
            b.addFloat (xoverParam (imgXoverPrefix, i), "Imager Crossover " + juce::String (i + 1),
                        logRange (60.0f, 16000.0f, 0.1f), defXover[i], "Hz", formatHz);

        for (int i = 0; i < kNumImgBands; ++i)
        {
            const juce::String n = "Imager Band " + juce::String (i + 1) + " ";
            b.addFloat (bandParam (imgBandPrefix, i, f::width), n + "Width", linRange (0.0f, 200.0f, 0.1f), defWidth[i], "%", formatPercent);
            b.addBool  (bandParam (imgBandPrefix, i, f::enabled), n + "Enabled", true);
        }

        b.addBool (str (imgCorrGuard), "Imager Correlation Guard", true);
    }

    // ---- Clipper ----------------------------------------------------------------
    b.addChoice (str (clipperMode), "Clipper Mode", { "Soft", "Hard", "Knee" }, 0);
    b.addFloat  (str (clipperCeiling), "Clipper Ceiling", linRange (-3.0f, 0.0f, 0.01f), -1.0f, "dBTP", formatDbTp);
    b.addFloat  (str (clipperDrive), "Clipper Drive", linRange (0.0f, 3.0f, 0.01f), 1.0f, "dB", formatDb);
    b.addBool   (str (clipperEnabled), "Clipper Enabled", true);
    b.addBool   (str (clipperLinkCeiling), "Clipper Link Ceiling To Limiter", true);

    // ---- TruePeakLimiter --------------------------------------------------------
    b.addFloat  (str (limCeiling), "Limiter Ceiling", linRange (-3.0f, 0.0f, 0.01f), -1.0f, "dBTP", formatDbTp);
    b.addFloat  (str (limLookahead), "Limiter Lookahead", linRange (0.0f, 5.0f, 0.01f), 1.5f, "ms", formatMs);
    b.addFloat  (str (limRelease), "Limiter Release", logRange (10.0f, 500.0f, 0.1f), 100.0f, "ms", formatMs);
    b.addBool   (str (limAutoRelease), "Limiter Auto Release", true);
    b.addChoice (str (limStyle), "Limiter Style", { "Transparent", "Punchy", "Warm", "Aggressive" }, 0);
    b.addFloat  (str (limLink), "Limiter Stereo Link", linRange (0.0f, 100.0f, 0.1f), 100.0f, "%", formatPercent);
    b.addChoice (str (limOversampling), "Limiter True-Peak Oversampling", { "4x", "8x", "16x" }, 0);
    b.addBool   (str (limStage1Enabled), "Limiter Transient Stage", true);

    // ---- Dither -----------------------------------------------------------------
    b.addBool   (str (ditherEnabled), "Dither Enabled", true);
    b.addChoice (str (ditherBits), "Dither Bit Depth", { "16 bit", "24 bit", "32 bit float (dither off)" }, 1);
    b.addChoice (str (ditherShape), "Dither Noise Shaping", { "TPDF", "High-Shibata", "E-Weighted" }, 1);

    // ---- Global per-module routing ----------------------------------------------
    for (int m = 0; m < kNumModules; ++m)
    {
        const juce::String n = moduleLabel (m) + " ";
        b.addInt    (slotParam (moduleOrderPrefix, m), "Slot " + juce::String (m + 1) + " Module", 0, kNumModules - 1, m, false);
        b.addBool   (slotParam (moduleBypassPrefix, m), n + "Bypass", false);
        b.addChoice (slotParam (moduleChannelPrefix, m), n + "Channel", kChannelModes5, 0);
        b.addFloat  (slotParam (moduleWetDryPrefix, m), n + "Wet/Dry", linRange (0.0f, 100.0f, 0.1f), 100.0f, "%", formatPercent);
        b.addBool   (slotParam (moduleDeltaPrefix, m), n + "Delta Solo", false);
    }

    // ---- Global -----------------------------------------------------------------
    b.addChoice (str (abSlot), "Snapshot Slot", { "A", "B", "C", "D" }, 0, false);
    b.addFloat  (str (pid::outputGain), "Output Gain", linRange (-24.0f, 24.0f, 0.01f), 0.0f, "dB", formatDb);
    b.addBool   (str (lowLatencyMode), "Low Latency Mode", false);
    b.addFloat  (str (lufsTarget), "LUFS Target", linRange (-20.0f, -6.0f, 0.1f), -9.0f, "LUFS", formatLufs, false);

    return layout;
}

//==============================================================================
// Construction
//==============================================================================
MeridianAudioProcessor::MeridianAudioProcessor()
    : juce::AudioProcessor (BusesProperties()
                                .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                                .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "MeridianParameters", createParameterLayout())
{
    modules[(size_t) ModuleId::Input]             = std::make_unique<InputModule> (apvts);
    modules[(size_t) ModuleId::CorrectiveEQ]      = std::make_unique<CorrectiveEQ> (apvts);
    modules[(size_t) ModuleId::LowEndControl]     = std::make_unique<LowEndControl> (apvts);
    modules[(size_t) ModuleId::DynamicEQ]         = std::make_unique<DynamicEQ> (apvts);
    modules[(size_t) ModuleId::GlueCompressor]    = std::make_unique<GlueCompressor> (apvts);
    modules[(size_t) ModuleId::MultibandDynamics] = std::make_unique<MultibandDynamics> (apvts);
    modules[(size_t) ModuleId::ToneEQ]            = std::make_unique<ToneEQ> (apvts);
    modules[(size_t) ModuleId::Saturation]        = std::make_unique<Saturation> (apvts);
    modules[(size_t) ModuleId::StereoImager]      = std::make_unique<StereoImager> (apvts);
    modules[(size_t) ModuleId::Clipper]           = std::make_unique<Clipper> (apvts);
    modules[(size_t) ModuleId::TruePeakLimiter]   = std::make_unique<TruePeakLimiter> (apvts);
    modules[(size_t) ModuleId::Dither]            = std::make_unique<Dither> (apvts);

    for (int m = 0; m < kNumModules; ++m)
    {
        jassert (modules[(size_t) m] != nullptr && static_cast<int> (modules[(size_t) m]->getId()) == m);

        auto& rt = runtime[(size_t) m];
        rt.bypass  = apvts.getRawParameterValue (pid::slotParam (pid::moduleBypassPrefix, m));
        rt.channel = apvts.getRawParameterValue (pid::slotParam (pid::moduleChannelPrefix, m));
        rt.wetDry  = apvts.getRawParameterValue (pid::slotParam (pid::moduleWetDryPrefix, m));
        rt.delta   = apvts.getRawParameterValue (pid::slotParam (pid::moduleDeltaPrefix, m));

        orderParams[(size_t) m] = apvts.getRawParameterValue (pid::slotParam (pid::moduleOrderPrefix, m));
        moduleOrder[m] = m;
        publishedOrder[(size_t) m].store (m);
    }

    outputGainParam = apvts.getRawParameterValue (pid::str (pid::outputGain));

    ensureTargetProfileProperty();

    assistant.attach (apvts);
    assistant.loadProfilesFromDirectory (getUserProfileDirectory());
    assistant.setActiveProfile (getTargetProfile());

    startTimerHz (15);
}

MeridianAudioProcessor::~MeridianAudioProcessor()
{
    stopTimer();
}

//==============================================================================
// Preparation
//==============================================================================
void MeridianAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    maxBlockSize = std::max (1, samplesPerBlock);

    for (auto* v : { &bufL, &bufR, &dryL, &dryR, &deltaL, &deltaR })
        v->assign ((size_t) maxBlockSize, 0.0);

    readModuleOrder();

    for (int m = 0; m < kNumModules; ++m)
    {
        auto& module = *modules[(size_t) m];
        module.prepare (currentSampleRate, maxBlockSize);
        module.updateParameters();
        module.reset();

        auto& rt = runtime[(size_t) m];
        rt.dryDelayL.prepare (kMaxModuleLatency);
        rt.dryDelayR.prepare (kMaxModuleLatency);
        rt.wet.prepare (currentSampleRate, 10.0);

        const bool bypassed = rt.bypass->load() >= 0.5f;
        rt.wet.reset (bypassed ? 0.0 : static_cast<double> (rt.wetDry->load()) * 0.01);
        rt.skipped = false;
    }

    deltaDelayL.prepare (kMaxChainLatency);
    deltaDelayR.prepare (kMaxChainLatency);
    lastDeltaModule = -1;

    outputGain.prepare (currentSampleRate, 20.0);
    outputGain.reset (juce::Decibels::decibelsToGain (static_cast<double> (outputGainParam->load())));

    meteringEngine.prepare (currentSampleRate, maxBlockSize);
    spectrumAnalyzer.prepare (currentSampleRate, maxBlockSize);
    meterQueue.clear();
    spectrumQueue.clear();

    {
        const juce::ScopedLock sl (captureLock);
        captureSize = std::max (1, (int) std::ceil (kCaptureSeconds * currentSampleRate));
        captureL.assign ((size_t) captureSize, 0.0f);
        captureR.assign ((size_t) captureSize, 0.0f);
        captureWritePos.store (0);
        captureFilled.store (0);
        captureRate.store (currentSampleRate);
    }

    referencePanel.setPlaybackSampleRate (currentSampleRate);
    assistant.prepare (currentSampleRate, maxBlockSize);

    samplePosition = 0;

    // Audio is stopped here, so the module latencies are stable: report directly.
    const int total = computeChainLatency();
    pendingLatency.store (total);
    latencyDirty.store (false);
    updateLatency();
}

void MeridianAudioProcessor::releaseResources()
{
    for (auto& m : modules)
        m->reset();

    for (auto& rt : runtime)
    {
        rt.dryDelayL.reset();
        rt.dryDelayR.reset();
    }

    deltaDelayL.reset();
    deltaDelayR.reset();
    meteringEngine.reset();
    spectrumAnalyzer.reset();
}

bool MeridianAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
        && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

double MeridianAudioProcessor::getTailLengthSeconds() const
{
    // Longest release ballistics (2 s) plus the chain latency.
    return 2.0 + static_cast<double> (getTotalLatencySamples()) / currentSampleRate;
}

//==============================================================================
// Latency
//==============================================================================
int MeridianAudioProcessor::computeChainLatency() const noexcept
{
    int total = 0;

    for (const auto& m : modules)
        total += std::min (m->getLatencySamples(), kMaxModuleLatency);

    return total;
}

void MeridianAudioProcessor::updateLatency()
{
    const int total = computeChainLatency();
    pendingLatency.store (total);
    reportedLatency.store (total, std::memory_order_release);

    if (total != getLatencySamples())
        setLatencySamples (total);
}

void MeridianAudioProcessor::timerCallback()
{
    if (latencyDirty.exchange (false))
        updateLatency();
}

//==============================================================================
// Module order
//==============================================================================
bool MeridianAudioProcessor::isValidModuleOrder (const std::array<int, kNumModules>& order) noexcept
{
    unsigned int seen = 0;

    for (int id : order)
    {
        if (id < 0 || id >= kNumModules)
            return false;

        const unsigned int bit = 1u << static_cast<unsigned int> (id);
        if ((seen & bit) != 0)
            return false;

        seen |= bit;
    }

    return seen == (1u << kNumModules) - 1u;
}

std::array<int, MeridianAudioProcessor::kNumModules> MeridianAudioProcessor::defaultModuleOrder() noexcept
{
    std::array<int, kNumModules> order {};
    for (int i = 0; i < kNumModules; ++i)
        order[(size_t) i] = i;
    return order;
}

void MeridianAudioProcessor::readModuleOrder() noexcept
{
    std::array<int, kNumModules> candidate {};

    for (int s = 0; s < kNumModules; ++s)
        candidate[(size_t) s] = juce::roundToInt (orderParams[(size_t) s]->load (std::memory_order_relaxed));

    // While the UI rewrites the 12 slot parameters one by one the intermediate
    // states are not permutations; keep the last valid order until it settles.
    if (! isValidModuleOrder (candidate))
        return;

    for (int s = 0; s < kNumModules; ++s)
    {
        moduleOrder[s] = candidate[(size_t) s];
        publishedOrder[(size_t) s].store (candidate[(size_t) s], std::memory_order_relaxed);
    }
}

std::array<int, MeridianAudioProcessor::kNumModules> MeridianAudioProcessor::getModuleOrder() const noexcept
{
    std::array<int, kNumModules> order {};
    for (int s = 0; s < kNumModules; ++s)
        order[(size_t) s] = publishedOrder[(size_t) s].load (std::memory_order_relaxed);

    return isValidModuleOrder (order) ? order : defaultModuleOrder();
}

bool MeridianAudioProcessor::setModuleOrder (const std::array<int, kNumModules>& newOrder)
{
    if (! isValidModuleOrder (newOrder))
        return false;

    for (int s = 0; s < kNumModules; ++s)
    {
        if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (apvts.getParameter (pid::slotParam (pid::moduleOrderPrefix, s))))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->convertTo0to1 (static_cast<float> (newOrder[(size_t) s])));
            p->endChangeGesture();
        }
    }

    // Keep the published order in sync immediately so the UI does not flicker
    // back before the audio thread picks the change up.
    for (int s = 0; s < kNumModules; ++s)
        publishedOrder[(size_t) s].store (newOrder[(size_t) s], std::memory_order_relaxed);

    return true;
}

bool MeridianAudioProcessor::moveModule (int fromSlot, int toSlot)
{
    if (fromSlot < 0 || fromSlot >= kNumModules || toSlot < 0 || toSlot >= kNumModules)
        return false;

    if (fromSlot == toSlot)
        return true;

    auto order = getModuleOrder();
    const int moving = order[(size_t) fromSlot];

    if (fromSlot < toSlot)
        std::rotate (order.begin() + fromSlot, order.begin() + fromSlot + 1, order.begin() + toSlot + 1);
    else
        std::rotate (order.begin() + toSlot, order.begin() + fromSlot, order.begin() + fromSlot + 1);

    jassert (order[(size_t) toSlot] == moving);
    juce::ignoreUnused (moving);
    return setModuleOrder (order);
}

meridian::ModuleBase* MeridianAudioProcessor::getModule (ModuleId id) noexcept
{
    const int i = static_cast<int> (id);
    return (i >= 0 && i < kNumModules) ? modules[(size_t) i].get() : nullptr;
}

const meridian::ModuleBase* MeridianAudioProcessor::getModule (ModuleId id) const noexcept
{
    const int i = static_cast<int> (id);
    return (i >= 0 && i < kNumModules) ? modules[(size_t) i].get() : nullptr;
}

//==============================================================================
// Processing
//==============================================================================
void MeridianAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    processBlockInternal (buffer);
}

void MeridianAudioProcessor::processBlock (juce::AudioBuffer<double>& buffer, juce::MidiBuffer&)
{
    processBlockInternal (buffer);
}

template <typename SampleType>
void MeridianAudioProcessor::processBlockInternal (juce::AudioBuffer<SampleType>& buffer)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const int numInputs  = getTotalNumInputChannels();
    const int numOutputs = getTotalNumOutputChannels();

    for (int ch = numInputs; ch < numOutputs; ++ch)
        buffer.clear (ch, 0, numSamples);

    if (numSamples == 0 || numInputs == 0 || maxBlockSize == 0 || buffer.getNumChannels() == 0)
        return;

    const bool stereo = buffer.getNumChannels() > 1;
    SampleType* left  = buffer.getWritePointer (0);
    SampleType* right = stereo ? buffer.getWritePointer (1) : nullptr;

    // Hosts may exceed the prepared block size; split into prepared-size chunks.
    for (int offset = 0; offset < numSamples; offset += maxBlockSize)
    {
        const int n = std::min (maxBlockSize, numSamples - offset);

        for (int i = 0; i < n; ++i)
        {
            bufL[(size_t) i] = toDouble (left[offset + i]);
            bufR[(size_t) i] = stereo ? toDouble (right[offset + i]) : bufL[(size_t) i];
        }

        processChunk (n);

        for (int i = 0; i < n; ++i)
        {
            left[offset + i] = static_cast<SampleType> (bufL[(size_t) i]);
            if (stereo)
                right[offset + i] = static_cast<SampleType> (bufR[(size_t) i]);
        }
    }
}

void MeridianAudioProcessor::processChunk (int numSamples) noexcept
{
    if (resetLoudnessRequested.exchange (false))
        meteringEngine.resetIntegrated();

    readModuleOrder();

    for (auto& m : modules)
        m->updateParameters();

    const int chainLatency = computeChainLatency();
    if (chainLatency != pendingLatency.load (std::memory_order_relaxed))
    {
        pendingLatency.store (chainLatency, std::memory_order_relaxed);
        latencyDirty.store (true, std::memory_order_release);
    }

    // The first slot (in chain order) with delta solo enabled is monitored.
    int deltaSlot = -1;
    for (int s = 0; s < kNumModules; ++s)
    {
        if (runtime[(size_t) moduleOrder[s]].delta->load (std::memory_order_relaxed) >= 0.5f)
        {
            deltaSlot = s;
            break;
        }
    }

    const int deltaModule = deltaSlot >= 0 ? moduleOrder[deltaSlot] : -1;
    if (deltaModule != lastDeltaModule)
    {
        deltaDelayL.reset();
        deltaDelayR.reset();
        lastDeltaModule = deltaModule;
    }

    for (int s = 0; s < kNumModules; ++s)
        processModuleInSlot (moduleOrder[s], numSamples, s == deltaSlot);

    // Output trim (post-chain).
    outputGain.setTargetValue (juce::Decibels::decibelsToGain (static_cast<double> (outputGainParam->load (std::memory_order_relaxed))));

    if (outputGain.isSmoothing())
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const double g = outputGain.processSample();
            bufL[(size_t) i] *= g;
            bufR[(size_t) i] *= g;
        }
    }
    else if (const double g = outputGain.getCurrentValue(); g != 1.0)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            bufL[(size_t) i] *= g;
            bufR[(size_t) i] *= g;
        }
    }

    // The assistant / codec preview analyse the processed programme, so the
    // capture happens before delta solo or reference A/B replace the output.
    writeCapture (numSamples);

    // Delta solo replaces the output with the monitored difference signal,
    // aligned with the rest of the chain.
    if (deltaSlot >= 0)
    {
        int downstream = 0;
        for (int s = deltaSlot + 1; s < kNumModules; ++s)
            downstream += std::min (modules[(size_t) moduleOrder[s]]->getLatencySamples(), kMaxModuleLatency);

        deltaDelayL.process (deltaL.data(), bufL.data(), numSamples, downstream);
        deltaDelayR.process (deltaR.data(), bufR.data(), numSamples, downstream);
    }

    pushMeters (numSamples);

    // Reference A/B: replaces the output with the level-matched reference
    // (after metering, so the meters keep showing the processed mix).
    referencePanel.renderReference (bufL.data(), bufR.data(), numSamples);
}

void MeridianAudioProcessor::writeCapture (int numSamples) noexcept
{
    if (captureSize <= 0)
        return;

    int pos = captureWritePos.load (std::memory_order_relaxed);

    for (int i = 0; i < numSamples; ++i)
    {
        captureL[(size_t) pos] = static_cast<float> (bufL[(size_t) i]);
        captureR[(size_t) pos] = static_cast<float> (bufR[(size_t) i]);
        if (++pos >= captureSize)
            pos = 0;
    }

    captureWritePos.store (pos, std::memory_order_release);
    const int filled = captureFilled.load (std::memory_order_relaxed);
    if (filled < captureSize)
        captureFilled.store (std::min (captureSize, filled + numSamples), std::memory_order_release);
}

int MeridianAudioProcessor::copyRecentAudio (juce::AudioBuffer<float>& dest, double seconds) const
{
    const juce::ScopedLock sl (captureLock);

    if (captureSize <= 0)
    {
        dest.setSize (2, 0);
        return 0;
    }

    const int filled = captureFilled.load (std::memory_order_acquire);
    const int writePos = captureWritePos.load (std::memory_order_acquire);
    const int wanted = (int) std::floor (std::max (0.0, seconds) * captureRate.load());
    const int n = std::min (filled, std::min (wanted, captureSize));

    dest.setSize (2, n, false, true, false);

    int start = writePos - n;
    if (start < 0)
        start += captureSize;

    const int first = std::min (n, captureSize - start);
    dest.copyFrom (0, 0, captureL.data() + start, first);
    dest.copyFrom (1, 0, captureR.data() + start, first);

    if (n > first)
    {
        dest.copyFrom (0, first, captureL.data(), n - first);
        dest.copyFrom (1, first, captureR.data(), n - first);
    }

    return n;
}

double MeridianAudioProcessor::getCapturedSeconds() const noexcept
{
    const double rate = captureRate.load();
    return rate > 0.0 ? captureFilled.load (std::memory_order_relaxed) / rate : 0.0;
}

juce::File MeridianAudioProcessor::getUserProfileDirectory()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
               .getChildFile ("Meridian Mastering Suite").getChildFile ("Profiles");
}

juce::File MeridianAudioProcessor::getUserPresetDirectory()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
               .getChildFile ("Meridian Mastering Suite").getChildFile ("Presets");
}

void MeridianAudioProcessor::processModuleInSlot (int moduleIndex, int numSamples, bool captureDelta) noexcept
{
    using choices::ChannelMode;

    auto& module = *modules[(size_t) moduleIndex];
    auto& rt = runtime[(size_t) moduleIndex];

    const bool bypassed = rt.bypass->load (std::memory_order_relaxed) >= 0.5f;
    const double wetTarget = bypassed ? 0.0 : juce::jlimit (0.0, 1.0, static_cast<double> (rt.wetDry->load (std::memory_order_relaxed)) * 0.01);
    rt.wet.setTargetValue (wetTarget);

    const int latency = std::min (module.getLatencySamples(), kMaxModuleLatency);

    double* L = bufL.data();
    double* R = bufR.data();
    double* dL = dryL.data();
    double* dR = dryR.data();

    // Latency-compensated dry path (always running so bypass/mix changes are seamless).
    rt.dryDelayL.process (L, dL, numSamples, latency);
    rt.dryDelayR.process (R, dR, numSamples, latency);

    // Fully dry and settled: skip the module entirely, output the aligned dry signal.
    if (! rt.wet.isSmoothing() && rt.wet.getCurrentValue() <= 0.0)
    {
        std::copy (dL, dL + numSamples, L);
        std::copy (dR, dR + numSamples, R);

        if (captureDelta)
        {
            std::fill (deltaL.begin(), deltaL.begin() + numSamples, 0.0);
            std::fill (deltaR.begin(), deltaR.begin() + numSamples, 0.0);
        }

        rt.skipped = true;
        return;
    }

    if (rt.skipped)
    {
        module.reset();
        rt.skipped = false;
    }

    // Channel routing. Single-channel modes feed the same signal to both module
    // inputs and take channel 0; the untouched component comes from the
    // latency-aligned dry path.
    const auto mode = static_cast<ChannelMode> (juce::jlimit (0, 4, juce::roundToInt (rt.channel->load (std::memory_order_relaxed))));

    switch (mode)
    {
        case ChannelMode::Stereo:
            module.process (L, R, numSamples);
            break;

        case ChannelMode::Mid:
        case ChannelMode::Side:
        {
            const bool mid = mode == ChannelMode::Mid;

            for (int i = 0; i < numSamples; ++i)
            {
                const double v = mid ? (L[i] + R[i]) * 0.5 : (L[i] - R[i]) * 0.5;
                L[i] = v;
                R[i] = v;
            }

            module.process (L, R, numSamples);

            for (int i = 0; i < numSamples; ++i)
            {
                const double processed = L[i];
                const double other = mid ? (dL[i] - dR[i]) * 0.5 : (dL[i] + dR[i]) * 0.5;
                const double M = mid ? processed : other;
                const double S = mid ? other : processed;
                L[i] = M + S;
                R[i] = M - S;
            }
            break;
        }

        case ChannelMode::Left:
        case ChannelMode::Right:
        {
            const bool leftOnly = mode == ChannelMode::Left;
            double* src = leftOnly ? L : R;
            double* other = leftOnly ? R : L;
            std::copy (src, src + numSamples, other);

            module.process (L, R, numSamples);

            if (leftOnly)
                std::copy (dR, dR + numSamples, R);
            else
            {
                std::copy (L, L + numSamples, R);
                std::copy (dL, dL + numSamples, L);
            }
            break;
        }
    }

    // Wet/dry mix (smoothed) and optional delta capture: out = dry + w * (wet - dry).
    for (int i = 0; i < numSamples; ++i)
    {
        const double w = rt.wet.processSample();
        const double diffL = w * (L[i] - dL[i]);
        const double diffR = w * (R[i] - dR[i]);

        L[i] = dL[i] + diffL;
        R[i] = dR[i] + diffR;

        if (captureDelta)
        {
            deltaL[(size_t) i] = diffL;
            deltaR[(size_t) i] = diffR;
        }
    }
}

void MeridianAudioProcessor::pushMeters (int numSamples) noexcept
{
    meteringEngine.process (bufL.data(), bufR.data(), numSamples);
    spectrumAnalyzer.pushSamples (bufL.data(), bufR.data(), numSamples);

    samplePosition += numSamples;

    MeterData data;
    meteringEngine.fillMeterData (data);

    for (int m = 0; m < kNumModules; ++m)
    {
        const bool bypassed = runtime[(size_t) m].bypass->load (std::memory_order_relaxed) >= 0.5f;
        data.gainReductionPerModule[m] = bypassed ? 0.0f : modules[(size_t) m]->getGainReductionDb();
    }

    data.samplePosition = samplePosition;

    assistant.updateGRBudget (data.gainReductionPerModule[(int) ModuleId::TruePeakLimiter],
                              data.gainReductionPerModule);
    referencePanel.setMainLUFS (data.integratedLUFS > -70.0f ? data.integratedLUFS : data.shortTermLUFS);
    referencePanel.setMainCrest (data.PLR);

    meterQueue.push (data);   // drops silently if the UI is not draining

    while (spectrumAnalyzer.getNextFrame (spectrumScratch))
    {
        spectrumScratch.samplePosition = samplePosition;
        spectrumQueue.push (spectrumScratch);
    }
}

//==============================================================================
// State
//==============================================================================
void MeridianAudioProcessor::ensureTargetProfileProperty()
{
    const juce::Identifier prop (pid::str (kTargetProfile));

    if (! apvts.state.hasProperty (prop))
        apvts.state.setProperty (prop, kDefaultTargetProfile, nullptr);
}

juce::String MeridianAudioProcessor::getTargetProfile() const
{
    return apvts.state.getProperty (juce::Identifier (pid::str (kTargetProfile)), kDefaultTargetProfile).toString();
}

void MeridianAudioProcessor::setTargetProfile (const juce::String& profileName)
{
    apvts.state.setProperty (juce::Identifier (pid::str (kTargetProfile)), profileName, nullptr);
}

void MeridianAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::ValueTree root ("MeridianState");
    root.setProperty ("version", kStateVersion, nullptr);
    root.setProperty ("pluginVersion", MERIDIAN_VERSION_STRING, nullptr);

    root.appendChild (apvts.copyState(), nullptr);

    juce::StringArray orderTokens;
    for (int id : getModuleOrder())
        orderTokens.add (juce::String (id));

    juce::ValueTree orderTree ("ModuleOrder");
    orderTree.setProperty ("slots", orderTokens.joinIntoString (","), nullptr);
    root.appendChild (orderTree, nullptr);

    {
        const juce::ScopedLock sl (snapshotLock);

        juce::ValueTree snaps ("Snapshots");
        snaps.setProperty ("active", activeSnapshot.load(), nullptr);

        for (int i = 0; i < kNumSnapshots; ++i)
        {
            if (! snapshots[(size_t) i].isValid())
                continue;

            juce::ValueTree slot ("Snapshot");
            slot.setProperty ("index", i, nullptr);
            slot.appendChild (snapshots[(size_t) i].createCopy(), nullptr);
            snaps.appendChild (slot, nullptr);
        }

        root.appendChild (snaps, nullptr);
    }

    if (auto xml = root.createXml())
        copyXmlToBinary (*xml, destData);
}

void MeridianAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr)
        return;

    const auto root = juce::ValueTree::fromXml (*xml);
    if (! root.isValid())
        return;

    // Legacy / bare APVTS state.
    if (root.hasType (apvts.state.getType()))
    {
        apvts.replaceState (root);
        ensureTargetProfileProperty();
        return;
    }

    if (! root.hasType ("MeridianState"))
        return;

    const auto params = root.getChildWithName (apvts.state.getType());
    if (params.isValid())
        apvts.replaceState (params.createCopy());

    ensureTargetProfileProperty();

    // Module order: validate the explicit order record; fall back to the
    // parameter values already restored above, then to the default order.
    const auto orderTree = root.getChildWithName ("ModuleOrder");
    if (orderTree.isValid())
    {
        juce::StringArray tokens;
        tokens.addTokens (orderTree.getProperty ("slots").toString(), ",", "");

        std::array<int, kNumModules> order {};
        bool parsed = tokens.size() == kNumModules;

        for (int s = 0; parsed && s < kNumModules; ++s)
            order[(size_t) s] = tokens[s].trim().getIntValue();

        if (parsed && isValidModuleOrder (order))
            setModuleOrder (order);
    }

    {
        std::array<int, kNumModules> fromParams {};
        for (int s = 0; s < kNumModules; ++s)
            fromParams[(size_t) s] = juce::roundToInt (orderParams[(size_t) s]->load());

        if (! isValidModuleOrder (fromParams))
            setModuleOrder (defaultModuleOrder());
    }

    {
        const juce::ScopedLock sl (snapshotLock);

        for (auto& s : snapshots)
            s = juce::ValueTree();

        const auto snaps = root.getChildWithName ("Snapshots");
        if (snaps.isValid())
        {
            for (const auto& slot : snaps)
            {
                const int index = slot.getProperty ("index", -1);
                if (isValidSlot (index) && slot.getNumChildren() > 0)
                {
                    const auto tree = slot.getChild (0);
                    if (tree.hasType (apvts.state.getType()))
                        snapshots[(size_t) index] = tree.createCopy();
                }
            }

            const int active = snaps.getProperty ("active", 0);
            activeSnapshot.store (isValidSlot (active) ? active : 0);
        }
        else
        {
            activeSnapshot.store (0);
        }
    }
}

//==============================================================================
// Snapshots
//==============================================================================
void MeridianAudioProcessor::setAbSlotParameter (int slot)
{
    if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (apvts.getParameter (pid::str (pid::abSlot))))
        p->setValueNotifyingHost (p->convertTo0to1 (static_cast<float> (slot)));
}

void MeridianAudioProcessor::replaceLiveState (const juce::ValueTree& source, int activeSlotAfter)
{
    // Snapshots carry their own module order; it is validated here and falls
    // back to the default order if the stored one is not a permutation.
    apvts.replaceState (source.createCopy());
    ensureTargetProfileProperty();

    std::array<int, kNumModules> fromParams {};
    for (int s = 0; s < kNumModules; ++s)
        fromParams[(size_t) s] = juce::roundToInt (orderParams[(size_t) s]->load());

    if (! isValidModuleOrder (fromParams))
        setModuleOrder (defaultModuleOrder());
    else
        for (int s = 0; s < kNumModules; ++s)
            publishedOrder[(size_t) s].store (fromParams[(size_t) s]);

    activeSnapshot.store (activeSlotAfter);
    setAbSlotParameter (activeSlotAfter);
}

void MeridianAudioProcessor::selectSnapshot (int slot)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (! isValidSlot (slot))
        return;

    const juce::ScopedLock sl (snapshotLock);
    const int current = activeSnapshot.load();

    if (slot == current)
        return;

    snapshots[(size_t) current] = apvts.copyState();

    if (! snapshots[(size_t) slot].isValid())
        snapshots[(size_t) slot] = apvts.copyState();

    replaceLiveState (snapshots[(size_t) slot], slot);
}

void MeridianAudioProcessor::storeSnapshot (int slot)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (! isValidSlot (slot))
        return;

    const juce::ScopedLock sl (snapshotLock);
    snapshots[(size_t) slot] = apvts.copyState();
}

bool MeridianAudioProcessor::recallSnapshot (int slot)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (! isValidSlot (slot))
        return false;

    const juce::ScopedLock sl (snapshotLock);
    if (! snapshots[(size_t) slot].isValid())
        return false;

    replaceLiveState (snapshots[(size_t) slot], slot);
    return true;
}

void MeridianAudioProcessor::copySnapshot (int slot)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (! isValidSlot (slot))
        return;

    const juce::ScopedLock sl (snapshotLock);

    if (slot == activeSnapshot.load())
        clipboard = apvts.copyState();
    else if (snapshots[(size_t) slot].isValid())
        clipboard = snapshots[(size_t) slot].createCopy();
}

bool MeridianAudioProcessor::pasteSnapshot (int slot)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (! isValidSlot (slot))
        return false;

    const juce::ScopedLock sl (snapshotLock);
    if (! clipboard.isValid())
        return false;

    if (slot == activeSnapshot.load())
        replaceLiveState (clipboard, slot);
    else
        snapshots[(size_t) slot] = clipboard.createCopy();

    return true;
}

void MeridianAudioProcessor::copySnapshotTo (int sourceSlot, int destSlot)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (! isValidSlot (sourceSlot) || ! isValidSlot (destSlot) || sourceSlot == destSlot)
        return;

    const juce::ScopedLock sl (snapshotLock);
    const int active = activeSnapshot.load();

    const juce::ValueTree source = (sourceSlot == active) ? apvts.copyState()
                                                          : snapshots[(size_t) sourceSlot].createCopy();
    if (! source.isValid())
        return;

    if (destSlot == active)
        replaceLiveState (source, destSlot);
    else
        snapshots[(size_t) destSlot] = source;
}

void MeridianAudioProcessor::clearSnapshot (int slot)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (! isValidSlot (slot))
        return;

    const juce::ScopedLock sl (snapshotLock);
    snapshots[(size_t) slot] = juce::ValueTree();
}

bool MeridianAudioProcessor::hasSnapshot (int slot) const
{
    if (! isValidSlot (slot))
        return false;

    const juce::ScopedLock sl (snapshotLock);
    return slot == activeSnapshot.load() || snapshots[(size_t) slot].isValid();
}

bool MeridianAudioProcessor::hasClipboard() const
{
    const juce::ScopedLock sl (snapshotLock);
    return clipboard.isValid();
}

//==============================================================================
juce::AudioProcessorEditor* MeridianAudioProcessor::createEditor()
{
    return new MeridianAudioProcessorEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MeridianAudioProcessor();
}
