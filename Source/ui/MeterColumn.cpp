#include "MeterColumn.h"
#include "UITheme.h"

#include <cmath>

namespace meridian::ui
{

namespace
{
    constexpr const char* kGrLabels[MeterData::numModules] =
        { "IN", "CEQ", "LOW", "DEQ", "GLU", "MBD", "TEQ", "SAT", "IMG", "CLP", "LIM", "DTH" };

    constexpr float kPeakMinDb = -60.0f;
    constexpr float kGrMaxDb = 12.0f;
    constexpr float kPeakHoldSeconds = 1.5f;
    constexpr float kPeakFallDbPerSec = 24.0f;
    constexpr float kGrFallDbPerSec = 18.0f;

    juce::Colour peakColour (float db)
    {
        if (db > -1.0f) return Colours::meterRed;
        if (db > -6.0f) return Colours::meterYellow;
        return Colours::meterGreen;
    }
}

MeterColumn::MeterColumn()
{
    setOpaque (true);
}

juce::Colour MeterColumn::lufsColour (float lufs, float target)
{
    if (lufs <= -70.0f)
        return Colours::textSecondary;

    const float diff = std::abs (lufs - target);
    if (diff <= 1.0f) return Colours::meterGreen;
    if (diff <= 3.0f) return Colours::meterYellow;
    return Colours::meterRed;
}

juce::String MeterColumn::fmt (float v, int decimals, float silenceBelow)
{
    if (! std::isfinite (v) || v <= silenceBelow)
        return juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x94"));   // em dash
    return juce::String (v, decimals);
}

void MeterColumn::setMeterData (const MeterData& data)
{
    meters = data;
    for (int m = 0; m < MeterData::numModules; ++m)
        grDisplay[(size_t) m] = juce::jmax (grDisplay[(size_t) m], std::abs (data.gainReductionPerModule[m]));
}

void MeterColumn::setTargetLUFS (float target)
{
    targetLUFS = target;
}

void MeterColumn::pushAudio (const float* left, const float* right, int numSamples)
{
    if (numSamples <= 0 || left == nullptr || right == nullptr)
        return;

    const float* chans[2] = { left, right };
    for (int ch = 0; ch < 2; ++ch)
    {
        float peak = 0.0f;
        for (int i = 0; i < numSamples; ++i)
            peak = juce::jmax (peak, std::abs (chans[ch][i]));

        const float db = juce::Decibels::gainToDecibels (peak, -100.0f);
        if (db >= peakDb[(size_t) ch])
            peakDb[(size_t) ch] = db;
        if (db >= holdDb[(size_t) ch])
        {
            holdDb[(size_t) ch] = db;
            holdTime[(size_t) ch] = kPeakHoldSeconds;
        }
    }

    const int start = juce::jmax (0, numSamples - kGonioPoints);
    for (int i = start; i < numSamples; ++i)
    {
        // Rotate 45 degrees: Mid vertical, Side horizontal.
        const float mid  = 0.70710678f * (left[i] + right[i]);
        const float side = 0.70710678f * (left[i] - right[i]);
        gonio[(size_t) gonioWrite] = { side, mid };
        gonioWrite = (gonioWrite + 1) % kGonioPoints;
    }
}

void MeterColumn::tick (float seconds)
{
    for (size_t ch = 0; ch < 2; ++ch)
    {
        peakDb[ch] = juce::jmax (-100.0f, peakDb[ch] - kPeakFallDbPerSec * seconds);
        if (holdTime[ch] > 0.0f)
            holdTime[ch] -= seconds;
        else
            holdDb[ch] = juce::jmax (-100.0f, holdDb[ch] - kPeakFallDbPerSec * seconds);
    }

    for (int m = 0; m < MeterData::numModules; ++m)
    {
        const float target = std::abs (meters.gainReductionPerModule[m]);
        auto& d = grDisplay[(size_t) m];
        d = juce::jmax (target, d - kGrFallDbPerSec * seconds);
    }

    repaint();
}

void MeterColumn::resized()
{
    auto area = getLocalBounds().reduced (8);

    lufsArea    = area.removeFromTop (66);
    msArea      = area.removeFromTop (38);
    numbersArea = area.removeFromTop (40);
    area.removeFromTop (4);
    corrArea    = area.removeFromTop (38);
    area.removeFromTop (6);

    const int gonioSize = juce::jmin (area.getWidth(), juce::jmax (80, area.getHeight() / 2));
    gonioArea = area.removeFromBottom (gonioSize).withSizeKeepingCentre (gonioSize, gonioSize);
    area.removeFromBottom (6);
    barsArea = area;
}

void MeterColumn::mouseDown (const juce::MouseEvent& e)
{
    if (lufsArea.contains (e.getPosition()) && onResetIntegrated)
        onResetIntegrated();
}

void MeterColumn::drawValue (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& caption,
                             const juce::String& value, juce::Colour colour, float valueSize) const
{
    g.setColour (Colours::textSecondary);
    g.setFont (makeFont (FontSize::small - 1.0f));
    g.drawText (caption, area.removeFromTop (14), juce::Justification::centred, false);
    g.setColour (colour);
    g.setFont (makeFont (valueSize, true));
    g.drawFittedText (value, area, juce::Justification::centred, 1, 0.7f);
}

void MeterColumn::drawCorrelation (juce::Graphics& g, juce::Rectangle<int> area) const
{
    g.setColour (Colours::textSecondary);
    g.setFont (makeFont (FontSize::small - 1.0f));
    auto caption = area.removeFromTop (14);
    g.drawText ("Correlation", caption, juce::Justification::centredLeft, false);
    g.setColour (meters.correlationLow >= 0.9f ? Colours::meterGreen : Colours::meterYellow);
    g.drawText ("<150 Hz " + juce::String (meters.correlationLow, 2), caption, juce::Justification::centredRight, false);

    auto bar = area.removeFromTop (12).toFloat();
    g.setColour (Colours::knobTrack);
    g.fillRoundedRectangle (bar, Radius::small);

    const float c = juce::jlimit (-1.0f, 1.0f, meters.correlation);
    const float centre = bar.getCentreX();
    const float x = centre + c * bar.getWidth() * 0.5f;
    const auto colour = c < 0.0f ? Colours::meterRed : (c < 0.3f ? Colours::meterYellow : Colours::meterGreen);
    g.setColour (colour);
    g.fillRect (juce::Rectangle<float>::leftTopRightBottom (juce::jmin (centre, x), bar.getY() + 2.0f,
                                                            juce::jmax (centre, x), bar.getBottom() - 2.0f));
    g.setColour (Colours::textSecondary);
    g.drawVerticalLine ((int) centre, bar.getY(), bar.getBottom());

    g.setFont (makeFont (FontSize::small - 2.0f));
    auto scale = area.removeFromTop (12);
    g.drawText ("-1", scale, juce::Justification::centredLeft, false);
    g.drawText (juce::String (meters.correlation, 2), scale, juce::Justification::centred, false);
    g.drawText ("+1", scale, juce::Justification::centredRight, false);
}

void MeterColumn::drawPeakBars (juce::Graphics& g, juce::Rectangle<int> area) const
{
    g.setFont (makeFont (FontSize::small - 2.0f));
    auto labels = area.removeFromBottom (14);
    const int barW = (area.getWidth() - 4) / 2;

    for (int ch = 0; ch < 2; ++ch)
    {
        auto bar = area.withX (area.getX() + ch * (barW + 4)).withWidth (barW).toFloat();
        g.setColour (Colours::knobTrack);
        g.fillRoundedRectangle (bar, Radius::small);

        const float norm = juce::jlimit (0.0f, 1.0f, (peakDb[(size_t) ch] - kPeakMinDb) / -kPeakMinDb);
        const auto fill = bar.withTop (bar.getBottom() - norm * bar.getHeight());
        juce::ColourGradient gradient (Colours::meterRed, 0.0f, bar.getY(),
                                       Colours::meterGreen, 0.0f, bar.getBottom(), false);
        gradient.addColour (1.0 / 60.0, Colours::meterYellow);                                         // -1 dBFS
        gradient.addColour (6.0 / 60.0, Colours::meterYellow.interpolatedWith (Colours::meterGreen, 0.3f)); // -6 dBFS
        g.setGradientFill (gradient);
        g.fillRect (fill);

        const float holdNorm = juce::jlimit (0.0f, 1.0f, (holdDb[(size_t) ch] - kPeakMinDb) / -kPeakMinDb);
        if (holdNorm > 0.0f)
        {
            g.setColour (peakColour (holdDb[(size_t) ch]));
            g.fillRect (bar.getX(), bar.getBottom() - holdNorm * bar.getHeight() - 1.0f, bar.getWidth(), 2.0f);
        }

        g.setColour (Colours::textSecondary);
        g.drawText (ch == 0 ? "L" : "R", labels.withX (labels.getX() + ch * (barW + 4)).withWidth (barW),
                    juce::Justification::centred, false);
    }
}

void MeterColumn::drawGRBars (juce::Graphics& g, juce::Rectangle<int> area) const
{
    g.setColour (Colours::textSecondary);
    g.setFont (makeFont (FontSize::small - 1.0f));
    g.drawText ("GR (dB)", area.removeFromTop (14), juce::Justification::centredLeft, false);

    const float rowH = (float) area.getHeight() / (float) MeterData::numModules;
    g.setFont (makeFont (juce::jlimit (8.0f, FontSize::small - 2.0f, rowH - 2.0f)));

    for (int m = 0; m < MeterData::numModules; ++m)
    {
        auto row = juce::Rectangle<float> ((float) area.getX(), (float) area.getY() + rowH * (float) m,
                                           (float) area.getWidth(), rowH);
        auto label = row.removeFromLeft (26.0f);
        auto value = row.removeFromRight (26.0f);
        auto bar = row.reduced (1.0f, juce::jmax (1.0f, rowH * 0.2f));

        g.setColour (Colours::textSecondary);
        g.drawText (kGrLabels[m], label, juce::Justification::centredLeft, false);

        g.setColour (Colours::knobTrack);
        g.fillRect (bar);

        const float gr = grDisplay[(size_t) m];
        const float norm = juce::jlimit (0.0f, 1.0f, gr / kGrMaxDb);
        g.setColour (Colours::gainReduction);
        g.fillRect (bar.withWidth (norm * bar.getWidth()));

        g.setColour (gr >= 0.05f ? Colours::textPrimary : Colours::textSecondary);
        g.drawText (gr >= 0.05f ? juce::String (gr, 1) : juce::String ("0"), value, juce::Justification::centredRight, false);
    }
}

void MeterColumn::drawGoniometer (juce::Graphics& g, juce::Rectangle<int> area) const
{
    const auto box = area.toFloat();
    g.setColour (Colours::background);
    g.fillRoundedRectangle (box, Radius::normal);
    g.setColour (Colours::border);
    g.drawRoundedRectangle (box, Radius::normal, 1.0f);

    const auto c = box.getCentre();
    const float r = box.getWidth() * 0.45f;

    g.setColour (Colours::border.withAlpha (0.7f));
    g.drawLine (c.x, box.getY() + 4.0f, c.x, box.getBottom() - 4.0f, 1.0f);
    g.drawLine (box.getX() + 4.0f, c.y, box.getRight() - 4.0f, c.y, 1.0f);
    g.drawLine (c.x - r * 0.7071f, c.y - r * 0.7071f, c.x + r * 0.7071f, c.y + r * 0.7071f, 0.6f);
    g.drawLine (c.x + r * 0.7071f, c.y - r * 0.7071f, c.x - r * 0.7071f, c.y + r * 0.7071f, 0.6f);

    g.setColour (Colours::textSecondary);
    g.setFont (makeFont (FontSize::small - 2.0f));
    g.drawText ("M", juce::Rectangle<float> (c.x + 3.0f, box.getY() + 2.0f, 12.0f, 12.0f), juce::Justification::centredLeft, false);
    g.drawText ("L", juce::Rectangle<float> (box.getX() + 4.0f, box.getY() + 2.0f, 12.0f, 12.0f), juce::Justification::centredLeft, false);
    g.drawText ("R", juce::Rectangle<float> (box.getRight() - 16.0f, box.getY() + 2.0f, 12.0f, 12.0f), juce::Justification::centredRight, false);

    g.saveState();
    g.reduceClipRegion (area.reduced (2));
    for (int i = 0; i < kGonioPoints; ++i)
    {
        // Oldest first; age 0 = newest.
        const int idx = (gonioWrite + i) % kGonioPoints;
        const float alpha = 0.08f + 0.8f * (float) i / (float) kGonioPoints;
        const auto& p = gonio[(size_t) idx];
        const float x = c.x + juce::jlimit (-1.2f, 1.2f, p.x) * r;
        const float y = c.y - juce::jlimit (-1.2f, 1.2f, p.y) * r;
        g.setColour (Colours::accentBright.withAlpha (alpha));
        g.fillRect (x - 0.75f, y - 0.75f, 1.5f, 1.5f);
    }
    g.restoreState();
}

void MeterColumn::paint (juce::Graphics& g)
{
    g.fillAll (Colours::background);
    drawPanel (g, getLocalBounds().toFloat().reduced (1.0f));

    // LUFS-I
    {
        auto a = lufsArea;
        g.setColour (Colours::textSecondary);
        g.setFont (makeFont (FontSize::small - 1.0f));
        g.drawText ("LUFS-I  (target " + juce::String (targetLUFS, 1) + ")", a.removeFromTop (14),
                    juce::Justification::centred, false);
        g.setColour (lufsColour (meters.integratedLUFS, targetLUFS));
        g.setFont (makeFont (34.0f, true));
        g.drawFittedText (fmt (meters.integratedLUFS, 1, -70.0f), a, juce::Justification::centred, 1, 0.7f);
    }

    // LUFS-M / LUFS-S
    {
        auto a = msArea;
        const int w = a.getWidth() / 2;
        drawValue (g, a.removeFromLeft (w), "LUFS-M", fmt (meters.momentaryLUFS, 1, -70.0f), Colours::textPrimary, FontSize::title);
        drawValue (g, a, "LUFS-S", fmt (meters.shortTermLUFS, 1, -70.0f), Colours::textPrimary, FontSize::title);
    }

    // dBTP / LRA / PLR
    {
        auto a = numbersArea;
        const int w = a.getWidth() / 3;
        const auto tpColour = meters.truePeak > -1.0f ? Colours::meterRed : Colours::textPrimary;
        drawValue (g, a.removeFromLeft (w), "dBTP", fmt (meters.truePeak, 1), tpColour, FontSize::large);
        drawValue (g, a.removeFromLeft (w), "LRA", juce::String (meters.LRA, 1), Colours::textPrimary, FontSize::large);
        drawValue (g, a, "PLR", meters.integratedLUFS > -70.0f ? juce::String (meters.PLR, 1) : fmt (-150.0f, 1),
                   Colours::textPrimary, FontSize::large);
    }

    drawCorrelation (g, corrArea);

    {
        auto a = barsArea;
        drawPeakBars (g, a.removeFromLeft (34));
        a.removeFromLeft (8);
        drawGRBars (g, a);
    }

    drawGoniometer (g, gonioArea);
}

} // namespace meridian::ui
