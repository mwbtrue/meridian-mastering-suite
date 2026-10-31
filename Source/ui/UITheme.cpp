#include "UITheme.h"

namespace meridian::ui
{

void drawPanel (juce::Graphics& g, juce::Rectangle<float> area, float radius)
{
    g.setColour (Colours::panel);
    g.fillRoundedRectangle (area, radius);
    g.setColour (Colours::border);
    g.drawRoundedRectangle (area.reduced (0.5f), radius, 1.0f);
}

MeridianLookAndFeel::MeridianLookAndFeel()
{
    using C = juce::Colour;

    auto scheme = getDarkColourScheme();
    scheme.setUIColour (ColourScheme::windowBackground, Colours::background);
    scheme.setUIColour (ColourScheme::widgetBackground, Colours::panel);
    scheme.setUIColour (ColourScheme::menuBackground, Colours::panel);
    scheme.setUIColour (ColourScheme::outline, Colours::border);
    scheme.setUIColour (ColourScheme::defaultText, Colours::textPrimary);
    scheme.setUIColour (ColourScheme::defaultFill, Colours::accent);
    scheme.setUIColour (ColourScheme::highlightedText, Colours::textPrimary);
    scheme.setUIColour (ColourScheme::highlightedFill, Colours::accent);
    scheme.setUIColour (ColourScheme::menuText, Colours::textPrimary);
    setColourScheme (scheme);

    setColour (juce::ResizableWindow::backgroundColourId, Colours::background);
    setColour (juce::DocumentWindow::backgroundColourId, Colours::background);

    setColour (juce::Slider::rotarySliderFillColourId, Colours::knobArc);
    setColour (juce::Slider::rotarySliderOutlineColourId, Colours::knobTrack);
    setColour (juce::Slider::thumbColourId, Colours::accentBright);
    setColour (juce::Slider::trackColourId, Colours::accent);
    setColour (juce::Slider::backgroundColourId, Colours::knobTrack);
    setColour (juce::Slider::textBoxTextColourId, Colours::textPrimary);
    setColour (juce::Slider::textBoxBackgroundColourId, C (0x00000000));
    setColour (juce::Slider::textBoxOutlineColourId, C (0x00000000));
    setColour (juce::Slider::textBoxHighlightColourId, Colours::accent.withAlpha (0.5f));

    setColour (juce::TextButton::buttonColourId, Colours::panelLight);
    setColour (juce::TextButton::buttonOnColourId, Colours::accent);
    setColour (juce::TextButton::textColourOffId, Colours::textPrimary);
    setColour (juce::TextButton::textColourOnId, juce::Colours::white);

    setColour (juce::ToggleButton::textColourId, Colours::textPrimary);
    setColour (juce::ToggleButton::tickColourId, Colours::accentBright);
    setColour (juce::ToggleButton::tickDisabledColourId, Colours::textSecondary);

    setColour (juce::ComboBox::backgroundColourId, Colours::panelLight);
    setColour (juce::ComboBox::outlineColourId, Colours::border);
    setColour (juce::ComboBox::textColourId, Colours::textPrimary);
    setColour (juce::ComboBox::arrowColourId, Colours::textSecondary);
    setColour (juce::ComboBox::focusedOutlineColourId, Colours::accent);

    setColour (juce::PopupMenu::backgroundColourId, Colours::panel);
    setColour (juce::PopupMenu::textColourId, Colours::textPrimary);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, Colours::accent);
    setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
    setColour (juce::PopupMenu::headerTextColourId, Colours::textSecondary);

    setColour (juce::Label::textColourId, Colours::textPrimary);
    setColour (juce::Label::backgroundColourId, C (0x00000000));
    setColour (juce::Label::outlineColourId, C (0x00000000));
    setColour (juce::Label::textWhenEditingColourId, Colours::textPrimary);
    setColour (juce::Label::backgroundWhenEditingColourId, Colours::panelLight);
    setColour (juce::Label::outlineWhenEditingColourId, Colours::accent);

    setColour (juce::GroupComponent::outlineColourId, Colours::border);
    setColour (juce::GroupComponent::textColourId, Colours::textSecondary);

    setColour (juce::ListBox::backgroundColourId, Colours::panel);
    setColour (juce::ListBox::outlineColourId, Colours::border);
    setColour (juce::ListBox::textColourId, Colours::textPrimary);

    setColour (juce::TextEditor::backgroundColourId, Colours::panelLight);
    setColour (juce::TextEditor::textColourId, Colours::textPrimary);
    setColour (juce::TextEditor::outlineColourId, Colours::border);
    setColour (juce::TextEditor::focusedOutlineColourId, Colours::accent);
    setColour (juce::TextEditor::highlightColourId, Colours::accent.withAlpha (0.5f));
    setColour (juce::CaretComponent::caretColourId, Colours::accentBright);

    setColour (juce::ScrollBar::thumbColourId, Colours::border);
    setColour (juce::ScrollBar::trackColourId, Colours::background);

    setColour (juce::AlertWindow::backgroundColourId, Colours::panel);
    setColour (juce::AlertWindow::textColourId, Colours::textPrimary);
    setColour (juce::AlertWindow::outlineColourId, Colours::border);

    setColour (juce::TooltipWindow::backgroundColourId, Colours::panel);
    setColour (juce::TooltipWindow::textColourId, Colours::textPrimary);
    setColour (juce::TooltipWindow::outlineColourId, Colours::border);
}

void MeridianLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                            float pos, float startAngle, float endAngle, juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (2.0f);
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    if (radius < 2.0f)
        return;

    const auto centre = bounds.getCentre();
    const float lineW = juce::jlimit (1.5f, 4.0f, radius * 0.18f);
    const float arcRadius = radius - lineW * 0.5f;
    const float angle = startAngle + pos * (endAngle - startAngle);
    const bool enabled = slider.isEnabled();

    juce::Path track;
    track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, endAngle, true);
    g.setColour (slider.findColour (juce::Slider::rotarySliderOutlineColourId));
    g.strokePath (track, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Bipolar ranges (min < 0 < max) draw the arc from the centre (zero) position.
    const auto range = slider.getRange();
    float fromAngle = startAngle;
    if (range.getStart() < 0.0 && range.getEnd() > 0.0)
        fromAngle = startAngle + (float) slider.valueToProportionOfLength (0.0) * (endAngle - startAngle);

    if (std::abs (angle - fromAngle) > 0.001f)
    {
        juce::Path arc;
        arc.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                           juce::jmin (fromAngle, angle), juce::jmax (fromAngle, angle), true);
        const auto fill = slider.findColour (juce::Slider::rotarySliderFillColourId);
        g.setColour (enabled ? fill : fill.withSaturation (0.0f));
        g.strokePath (arc, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    const float knobR = arcRadius - lineW * 1.2f;
    if (knobR > 1.0f)
    {
        g.setColour (Colours::panelLight);
        g.fillEllipse (centre.x - knobR, centre.y - knobR, knobR * 2.0f, knobR * 2.0f);
        g.setColour (Colours::border);
        g.drawEllipse (centre.x - knobR, centre.y - knobR, knobR * 2.0f, knobR * 2.0f, 1.0f);

        const auto tip = centre.getPointOnCircumference (knobR * 0.85f, angle);
        const auto base = centre.getPointOnCircumference (knobR * 0.25f, angle);
        g.setColour (enabled ? Colours::textPrimary : Colours::textSecondary);
        g.drawLine ({ base, tip }, juce::jmax (1.5f, lineW * 0.6f));
    }
}

void MeridianLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button,
                                                const juce::Colour& backgroundColour,
                                                bool highlighted, bool down)
{
    auto area = button.getLocalBounds().toFloat().reduced (0.5f);
    const bool on = button.getToggleState();

    auto fill = on ? button.findColour (juce::TextButton::buttonOnColourId) : backgroundColour;
    if (! button.isEnabled())
        fill = fill.withMultipliedAlpha (0.5f);
    else if (down)
        fill = fill.brighter (0.25f);
    else if (highlighted)
        fill = fill.brighter (0.12f);

    g.setColour (fill);
    g.fillRoundedRectangle (area, Radius::normal);
    g.setColour (on ? Colours::accentBright : Colours::border);
    g.drawRoundedRectangle (area, Radius::normal, 1.0f);
}

void MeridianLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool,
                                        int, int, int, int, juce::ComboBox& box)
{
    const auto area = juce::Rectangle<int> (0, 0, width, height).toFloat().reduced (0.5f);

    g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
    g.fillRoundedRectangle (area, Radius::normal);
    g.setColour (box.hasKeyboardFocus (true) ? box.findColour (juce::ComboBox::focusedOutlineColourId)
                                              : box.findColour (juce::ComboBox::outlineColourId));
    g.drawRoundedRectangle (area, Radius::normal, 1.0f);

    const float arrowW = juce::jmin (10.0f, (float) height * 0.4f);
    const float cx = (float) width - arrowW - 6.0f;
    const float cy = (float) height * 0.5f;
    juce::Path arrow;
    arrow.addTriangle (cx, cy - arrowW * 0.25f, cx + arrowW, cy - arrowW * 0.25f, cx + arrowW * 0.5f, cy + arrowW * 0.3f);
    g.setColour (box.findColour (juce::ComboBox::arrowColourId).withAlpha (box.isEnabled() ? 1.0f : 0.4f));
    g.fillPath (arrow);
}

void MeridianLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (4, 1, box.getWidth() - 22, box.getHeight() - 2);
    label.setFont (getComboBoxFont (box));
}

void MeridianLookAndFeel::drawLabel (juce::Graphics& g, juce::Label& label)
{
    g.fillAll (label.findColour (juce::Label::backgroundColourId));

    if (label.isBeingEdited())
    {
        g.setColour (label.findColour (juce::Label::outlineWhenEditingColourId));
        g.drawRoundedRectangle (label.getLocalBounds().toFloat().reduced (0.5f), Radius::small, 1.0f);
        return;
    }

    const float alpha = label.isEnabled() ? 1.0f : 0.5f;
    g.setColour (label.findColour (juce::Label::textColourId).withMultipliedAlpha (alpha));
    g.setFont (getLabelFont (label));

    const auto textArea = getLabelBorderSize (label).subtractedFrom (label.getLocalBounds());
    g.drawFittedText (label.getText(), textArea, label.getJustificationType(),
                      juce::jmax (1, (int) ((float) textArea.getHeight() / getLabelFont (label).getHeight())),
                      label.getMinimumHorizontalScale());

    const auto outline = label.findColour (juce::Label::outlineColourId);
    if (! outline.isTransparent())
    {
        g.setColour (outline);
        g.drawRoundedRectangle (label.getLocalBounds().toFloat().reduced (0.5f), Radius::small, 1.0f);
    }
}

void MeridianLookAndFeel::drawGroupComponentOutline (juce::Graphics& g, int width, int height,
                                                     const juce::String& text, const juce::Justification&,
                                                     juce::GroupComponent& group)
{
    const float textH = FontSize::small + 2.0f;
    const auto font = makeFont (FontSize::small, true);
    auto area = juce::Rectangle<float> (0.5f, textH * 0.5f, (float) width - 1.0f, (float) height - textH * 0.5f - 0.5f);

    g.setColour (group.findColour (juce::GroupComponent::outlineColourId));
    g.drawRoundedRectangle (area, Radius::normal, 1.0f);

    if (text.isNotEmpty())
    {
        const float textW = juce::GlyphArrangement::getStringWidth (font, text) + 8.0f;
        const juce::Rectangle<float> textBox (Radius::large + 2.0f, 0.0f, textW, textH);
        g.setColour (Colours::panel);
        g.fillRect (textBox);
        g.setColour (group.findColour (juce::GroupComponent::textColourId));
        g.setFont (font);
        g.drawText (text, textBox, juce::Justification::centred, false);
    }
}

void MeridianLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button,
                                            bool highlighted, bool)
{
    const auto bounds = button.getLocalBounds().toFloat();
    const float box = juce::jmin (14.0f, bounds.getHeight() - 4.0f);
    const juce::Rectangle<float> tick (2.0f, (bounds.getHeight() - box) * 0.5f, box, box);

    g.setColour (button.getToggleState() ? Colours::accent : Colours::panelLight);
    g.fillRoundedRectangle (tick, Radius::small);
    g.setColour (highlighted ? Colours::accentBright : Colours::border);
    g.drawRoundedRectangle (tick, Radius::small, 1.0f);

    if (button.getToggleState())
    {
        juce::Path check;
        check.startNewSubPath (tick.getX() + box * 0.22f, tick.getCentreY());
        check.lineTo (tick.getX() + box * 0.42f, tick.getBottom() - box * 0.25f);
        check.lineTo (tick.getRight() - box * 0.2f, tick.getY() + box * 0.25f);
        g.setColour (juce::Colours::white);
        g.strokePath (check, juce::PathStrokeType (1.8f));
    }

    if (button.getButtonText().isNotEmpty())
    {
        g.setColour (button.findColour (juce::ToggleButton::textColourId).withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.5f));
        g.setFont (makeFont (FontSize::small));
        g.drawFittedText (button.getButtonText(),
                          button.getLocalBounds().withTrimmedLeft ((int) (box + 7.0f)),
                          juce::Justification::centredLeft, 1);
    }
}

juce::Font MeridianLookAndFeel::getComboBoxFont (juce::ComboBox& box)
{
    return makeFont (juce::jmin (FontSize::normal, (float) box.getHeight() * 0.6f));
}

juce::Font MeridianLookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    return makeFont (juce::jmin (FontSize::normal, (float) buttonHeight * 0.6f));
}

juce::Font MeridianLookAndFeel::getPopupMenuFont()
{
    return makeFont (FontSize::normal);
}

} // namespace meridian::ui
