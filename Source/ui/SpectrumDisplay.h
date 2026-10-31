#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "common/AnalysisTypes.h"

#include <array>
#include <utility>
#include <vector>

namespace meridian::ui
{

/**
    Real-time spectrum display (message thread only).

    Axes: logarithmic frequency 20 Hz - 20 kHz, level -90 .. +6 dB
    (SpectrumAnalyzer scale: full-scale sine = 0 dB).

    Curves
      L+R        grey, filled         SpectrumFrame::magnitude
      Mid        blue                 SpectrumFrame::magnitudeMid
      Side       indigo, dashed       SpectrumFrame::magnitudeSide
      Peak hold  white dots           SpectrumFrame::peakHold
      Reference  orange (optional)    setReferenceCurve()
      EQ overlay green                setEQCurve(): {frequency Hz, gain dB} pairs,
                                      drawn on its own +/-18 dB scale centred
                                      on the display's vertical middle.

    Each displayed curve is smoothed per pixel column (attack immediate,
    release ~ 300 ms at 60 fps) so the display is calm without lagging peaks.
    Hovering shows frequency, note name and the L+R / Mid / Side levels.
*/
class SpectrumDisplay final : public juce::Component
{
public:
    static constexpr float kMinFreq = 20.0f, kMaxFreq = 20000.0f;
    static constexpr float kMinDb = -90.0f, kMaxDb = 6.0f;
    static constexpr float kEqRangeDb = 18.0f;

    SpectrumDisplay();

    void setSpectrumFrame (const SpectrumFrame& frame);
    void setEQCurve (std::vector<std::pair<float, float>> curve);
    /** Reference spectrum in dB per FFT bin (SpectrumFrame::numBins values); empty hides it. */
    void setReferenceCurve (std::vector<float> binsDb, double sampleRate);

    void setShowMid (bool b)  { showMid = b; repaint(); }
    void setShowSide (bool b) { showSide = b; repaint(); }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    float freqToX (float freq) const noexcept;
    float xToFreq (float x) const noexcept;
    float dbToY (float db) const noexcept;
    float yToDb (float y) const noexcept;

private:
    enum Curve { Sum = 0, Mid, Side, Peak, NumCurves };

    void rebuildColumns();
    float sampleBins (const float* bins, int column) const noexcept;
    juce::Path buildPath (const std::vector<float>& values, bool closeToBottom) const;
    static juce::String noteName (float freq);

    juce::Rectangle<float> plot;
    std::array<std::vector<float>, NumCurves> display;   // per pixel column, dB
    std::vector<float> columnLowBin, columnHighBin;
    double frameRate = 48000.0;
    bool hasFrame = false;
    bool showMid = true, showSide = true;

    std::vector<std::pair<float, float>> eqCurve;
    std::vector<float> referenceDb;
    double referenceRate = 48000.0;

    juce::Point<float> mouse;
    bool hovering = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumDisplay)
};

} // namespace meridian::ui
