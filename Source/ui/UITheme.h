#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace meridian::ui
{

/** Central palette, type scale and geometry for the Meridian UI. */
namespace Colours
{
    inline const juce::Colour background   { 0xff1a1a1f };
    inline const juce::Colour panel        { 0xff242430 };
    inline const juce::Colour border       { 0xff383848 };
    inline const juce::Colour accent       { 0xff5c6bc0 };
    inline const juce::Colour accentBright { 0xff7986cb };
    inline const juce::Colour meterGreen   { 0xff4caf50 };
    inline const juce::Colour meterYellow  { 0xffffc107 };
    inline const juce::Colour meterRed     { 0xfff44336 };
    inline const juce::Colour gainReduction{ 0xff2196f3 };
    inline const juce::Colour textPrimary  { 0xffe0e0e0 };
    inline const juce::Colour textSecondary{ 0xff9e9e9e };
    inline const juce::Colour knobArc      { 0xff5c6bc0 };
    inline const juce::Colour knobTrack    { 0xff30303c };
    inline const juce::Colour panelLight   { 0xff2c2c3a };
}

namespace FontSize
{
    inline constexpr float small  = 12.0f;
    inline constexpr float normal = 14.0f;
    inline constexpr float large  = 16.0f;
    inline constexpr float title  = 18.0f;
}

namespace Radius
{
    inline constexpr float small  = 3.0f;
    inline constexpr float normal = 5.0f;
    inline constexpr float large  = 8.0f;
}

inline juce::Font makeFont (float size, bool bold = false)
{
    return juce::Font (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain));
}

/** Fills a rounded panel with the standard background + border. */
void drawPanel (juce::Graphics& g, juce::Rectangle<float> area, float radius = Radius::large);

/**
    Flat dark look-and-feel: indigo arcs on rotary sliders, rounded flat
    buttons and combo boxes, secondary-coloured group outlines.
*/
class MeridianLookAndFeel : public juce::LookAndFeel_V4
{
public:
    MeridianLookAndFeel();

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPosProportional, float rotaryStartAngle,
                           float rotaryEndAngle, juce::Slider&) override;

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown,
                       int buttonX, int buttonY, int buttonW, int buttonH, juce::ComboBox&) override;

    void drawLabel (juce::Graphics&, juce::Label&) override;

    void drawGroupComponentOutline (juce::Graphics&, int width, int height, const juce::String& text,
                                    const juce::Justification&, juce::GroupComponent&) override;

    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool shouldDrawButtonAsHighlighted,
                           bool shouldDrawButtonAsDown) override;

    juce::Font getComboBoxFont (juce::ComboBox&) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    juce::Font getPopupMenuFont() override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;
};

} // namespace meridian::ui
