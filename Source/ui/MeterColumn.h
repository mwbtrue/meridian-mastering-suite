#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "common/AnalysisTypes.h"

#include <array>
#include <functional>

namespace meridian::ui
{

/**
    Right-hand meter column (message thread only).

      LUFS-I (large; click to reset the integrated measurement)
      LUFS-M / LUFS-S
      dBTP (max) / LRA / PLR
      correlation bar -1..+1 (full band, plus the < 150 Hz value)
      L / R peak bars (-60..0 dBFS, 1.5 s peak hold)  |  12 labelled GR bars (0..12 dB)
      goniometer (Lissajous, M up / S across) with 512 fading dots

    LUFS-I colour vs. setTargetLUFS(): green within 1 LU, yellow within 3 LU,
    red outside. Per-channel peaks and the goniometer come from pushAudio()
    (the editor feeds the latest processed samples from the processor's
    capture ring); everything else comes from setMeterData().
*/
class MeterColumn final : public juce::Component
{
public:
    static constexpr int kGonioPoints = 512;

    MeterColumn();

    void setMeterData (const MeterData& data);
    void setTargetLUFS (float target);
    /** Latest processed output samples (any length; the last 512 feed the goniometer). */
    void pushAudio (const float* left, const float* right, int numSamples);
    /** Advances peak-hold / decay ballistics; call once per UI frame with the frame time. */
    void tick (float seconds);

    std::function<void()> onResetIntegrated;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

    /** Colour rule for the integrated loudness readout. */
    static juce::Colour lufsColour (float lufs, float target);

private:
    void drawValue (juce::Graphics&, juce::Rectangle<int> area, const juce::String& caption,
                    const juce::String& value, juce::Colour colour, float valueSize) const;
    void drawCorrelation (juce::Graphics&, juce::Rectangle<int> area) const;
    void drawPeakBars (juce::Graphics&, juce::Rectangle<int> area) const;
    void drawGRBars (juce::Graphics&, juce::Rectangle<int> area) const;
    void drawGoniometer (juce::Graphics&, juce::Rectangle<int> area) const;

    static juce::String fmt (float v, int decimals, float silenceBelow = -100.0f);

    MeterData meters;
    float targetLUFS = -9.0f;

    std::array<float, 2> peakDb { -100.0f, -100.0f }, holdDb { -100.0f, -100.0f };
    std::array<float, 2> holdTime { 0.0f, 0.0f };
    std::array<float, MeterData::numModules> grDisplay {};

    std::array<juce::Point<float>, kGonioPoints> gonio {};
    int gonioWrite = 0;

    juce::Rectangle<int> lufsArea, msArea, numbersArea, corrArea, barsArea, gonioArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MeterColumn)
};

} // namespace meridian::ui
