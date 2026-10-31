#include "SpectrumDisplay.h"
#include "UITheme.h"

#include <cmath>

namespace meridian::ui
{

namespace
{
    constexpr float kGridFreqs[] = { 31.5f, 63.0f, 125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f };
    constexpr float kGridDbs[]   = { -60.0f, -48.0f, -36.0f, -24.0f, -12.0f, 0.0f };
    constexpr float kReleaseCoeff = 0.25f;   // fraction of the fall applied per frame

    const juce::Colour kSumColour  { 0xff9e9e9e };
    const juce::Colour kMidColour  { 0xff2196f3 };
    const juce::Colour kRefColour  { 0xffff9800 };
    const juce::Colour kEqColour   { 0xff4caf50 };

    juce::String formatFreq (float f)
    {
        return f >= 1000.0f ? juce::String (f / 1000.0f, f >= 10000.0f ? 1 : 2) + " kHz"
                            : juce::String (juce::roundToInt (f)) + " Hz";
    }
}

SpectrumDisplay::SpectrumDisplay()
{
    setOpaque (true);
    for (auto& d : display)
        d.clear();
}

float SpectrumDisplay::freqToX (float freq) const noexcept
{
    const float norm = std::log (juce::jlimit (kMinFreq, kMaxFreq, freq) / kMinFreq) / std::log (kMaxFreq / kMinFreq);
    return plot.getX() + norm * plot.getWidth();
}

float SpectrumDisplay::xToFreq (float x) const noexcept
{
    const float norm = juce::jlimit (0.0f, 1.0f, (x - plot.getX()) / juce::jmax (1.0f, plot.getWidth()));
    return kMinFreq * std::pow (kMaxFreq / kMinFreq, norm);
}

float SpectrumDisplay::dbToY (float db) const noexcept
{
    return juce::jmap (juce::jlimit (kMinDb, kMaxDb, db), kMinDb, kMaxDb, plot.getBottom(), plot.getY());
}

float SpectrumDisplay::yToDb (float y) const noexcept
{
    return juce::jmap (y, plot.getBottom(), plot.getY(), kMinDb, kMaxDb);
}

void SpectrumDisplay::resized()
{
    plot = getLocalBounds().toFloat().reduced (8.0f).withTrimmedLeft (28.0f).withTrimmedBottom (16.0f);
    rebuildColumns();
}

void SpectrumDisplay::rebuildColumns()
{
    const int columns = juce::jmax (1, (int) plot.getWidth());
    columnLowBin.assign ((size_t) columns, 0.0f);
    columnHighBin.assign ((size_t) columns, 0.0f);

    const float binHz = (float) frameRate / (float) SpectrumFrame::fftSize;
    for (int c = 0; c < columns; ++c)
    {
        const float f0 = xToFreq (plot.getX() + (float) c);
        const float f1 = xToFreq (plot.getX() + (float) c + 1.0f);
        columnLowBin[(size_t) c]  = juce::jlimit (0.0f, (float) (SpectrumFrame::numBins - 1), f0 / binHz);
        columnHighBin[(size_t) c] = juce::jlimit (0.0f, (float) (SpectrumFrame::numBins - 1), f1 / binHz);
    }

    for (auto& d : display)
        d.assign ((size_t) columns, kMinDb - 10.0f);

    hasFrame = false;
}

float SpectrumDisplay::sampleBins (const float* bins, int column) const noexcept
{
    const float lo = columnLowBin[(size_t) column];
    const float hi = columnHighBin[(size_t) column];

    if (hi - lo < 1.0f)
    {
        // Several pixels per bin: linear interpolation at the column centre.
        const float pos = 0.5f * (lo + hi);
        const int i0 = (int) pos;
        const int i1 = juce::jmin (i0 + 1, SpectrumFrame::numBins - 1);
        const float frac = pos - (float) i0;
        return bins[i0] + frac * (bins[i1] - bins[i0]);
    }

    // Several bins per pixel: the maximum keeps narrow peaks visible.
    float v = bins[(int) lo];
    for (int k = (int) lo + 1; k <= (int) std::ceil (hi) && k < SpectrumFrame::numBins; ++k)
        v = juce::jmax (v, bins[k]);
    return v;
}

void SpectrumDisplay::setSpectrumFrame (const SpectrumFrame& frame)
{
    if (std::abs (frame.sampleRate - frameRate) > 0.5 && frame.sampleRate > 0.0)
    {
        frameRate = frame.sampleRate;
        rebuildColumns();
    }

    const float* sources[NumCurves] = { frame.magnitude, frame.magnitudeMid, frame.magnitudeSide, frame.peakHold };
    const int columns = (int) columnLowBin.size();

    for (int curve = 0; curve < NumCurves; ++curve)
    {
        auto& d = display[(size_t) curve];
        for (int c = 0; c < columns; ++c)
        {
            const float v = sampleBins (sources[curve], c);
            float& cur = d[(size_t) c];
            if (! hasFrame || curve == Peak || v > cur)
                cur = v;
            else
                cur += (v - cur) * kReleaseCoeff;
        }
    }

    hasFrame = true;
    repaint();
}

void SpectrumDisplay::setEQCurve (std::vector<std::pair<float, float>> curve)
{
    eqCurve = std::move (curve);
    repaint();
}

void SpectrumDisplay::setReferenceCurve (std::vector<float> binsDb, double sampleRate)
{
    referenceDb = std::move (binsDb);
    referenceRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    repaint();
}

juce::Path SpectrumDisplay::buildPath (const std::vector<float>& values, bool closeToBottom) const
{
    juce::Path p;
    const int columns = (int) values.size();
    if (columns == 0)
        return p;

    p.startNewSubPath (plot.getX(), dbToY (values[0]));
    for (int c = 1; c < columns; ++c)
        p.lineTo (plot.getX() + (float) c + 0.5f, dbToY (values[(size_t) c]));

    if (closeToBottom)
    {
        p.lineTo (plot.getRight(), plot.getBottom());
        p.lineTo (plot.getX(), plot.getBottom());
        p.closeSubPath();
    }

    return p;
}

juce::String SpectrumDisplay::noteName (float freq)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const int midi = juce::roundToInt (69.0f + 12.0f * std::log2 (freq / 440.0f));
    if (midi < 0 || midi > 135)
        return {};
    return juce::String (names[midi % 12]) + juce::String (midi / 12 - 1);
}

void SpectrumDisplay::paint (juce::Graphics& g)
{
    g.fillAll (Colours::background);
    drawPanel (g, getLocalBounds().toFloat().reduced (1.0f));

    // ---- Grid --------------------------------------------------------------------------
    g.setFont (makeFont (FontSize::small - 1.0f));
    for (float f : kGridFreqs)
    {
        const float x = freqToX (f);
        g.setColour (Colours::border.withAlpha (0.6f));
        g.drawVerticalLine (juce::roundToInt (x), plot.getY(), plot.getBottom());
        g.setColour (Colours::textSecondary);
        const juce::String label = f >= 1000.0f ? juce::String ((int) (f / 1000.0f)) + "k"
                                                : (f < 40.0f ? juce::String ("31.5") : juce::String ((int) f));
        g.drawText (label, juce::Rectangle<float> (x - 20.0f, plot.getBottom() + 2.0f, 40.0f, 14.0f),
                    juce::Justification::centred, false);
    }

    for (float db : kGridDbs)
    {
        const float y = dbToY (db);
        g.setColour (db == 0.0f ? Colours::border.brighter (0.3f) : Colours::border.withAlpha (0.6f));
        g.drawHorizontalLine (juce::roundToInt (y), plot.getX(), plot.getRight());
        g.setColour (Colours::textSecondary);
        g.drawText (juce::String ((int) db), juce::Rectangle<float> (plot.getX() - 30.0f, y - 7.0f, 26.0f, 14.0f),
                    juce::Justification::centredRight, false);
    }

    g.saveState();
    g.reduceClipRegion (plot.toNearestInt());

    // ---- Reference -----------------------------------------------------------------------
    if (referenceDb.size() >= 2)
    {
        juce::Path ref;
        const float binHz = (float) referenceRate / (float) SpectrumFrame::fftSize;
        bool started = false;
        for (int c = 0; c < (int) plot.getWidth(); c += 2)
        {
            const float f = xToFreq (plot.getX() + (float) c);
            const float pos = juce::jlimit (0.0f, (float) referenceDb.size() - 1.001f, f / binHz);
            const int i0 = (int) pos;
            const float v = referenceDb[(size_t) i0] + (pos - (float) i0) * (referenceDb[(size_t) i0 + 1] - referenceDb[(size_t) i0]);
            const juce::Point<float> pt (plot.getX() + (float) c, dbToY (v));
            if (! started) { ref.startNewSubPath (pt); started = true; }
            else           ref.lineTo (pt);
        }
        g.setColour (kRefColour.withAlpha (0.85f));
        g.strokePath (ref, juce::PathStrokeType (1.3f));
    }

    if (hasFrame)
    {
        // ---- L+R -------------------------------------------------------------------------
        const auto sumFill = buildPath (display[Sum], true);
        g.setGradientFill (juce::ColourGradient (kSumColour.withAlpha (0.35f), 0.0f, plot.getY(),
                                                 kSumColour.withAlpha (0.05f), 0.0f, plot.getBottom(), false));
        g.fillPath (sumFill);
        g.setColour (kSumColour);
        g.strokePath (buildPath (display[Sum], false), juce::PathStrokeType (1.2f));

        // ---- Mid ---------------------------------------------------------------------------
        if (showMid)
        {
            g.setColour (kMidColour.withAlpha (0.9f));
            g.strokePath (buildPath (display[Mid], false), juce::PathStrokeType (1.0f));
        }

        // ---- Side (dashed) -----------------------------------------------------------------
        if (showSide)
        {
            juce::Path dashed;
            const float dashes[] = { 4.0f, 3.0f };
            juce::PathStrokeType (1.0f).createDashedStroke (dashed, buildPath (display[Side], false), dashes, 2);
            g.setColour (Colours::accentBright);
            g.fillPath (dashed);
        }

        // ---- Peak hold (dots) --------------------------------------------------------------
        g.setColour (juce::Colours::white.withAlpha (0.8f));
        const auto& peak = display[Peak];
        for (int c = 0; c < (int) peak.size(); c += 3)
            if (peak[(size_t) c] > kMinDb)
                g.fillEllipse (plot.getX() + (float) c - 0.9f, dbToY (peak[(size_t) c]) - 0.9f, 1.8f, 1.8f);
    }

    // ---- EQ overlay ------------------------------------------------------------------------
    if (eqCurve.size() >= 2)
    {
        const float mid = plot.getCentreY();
        const float scale = plot.getHeight() * 0.5f / kEqRangeDb;
        juce::Path eq;
        bool started = false;
        for (const auto& [freq, gainDb] : eqCurve)
        {
            if (freq < kMinFreq * 0.9f || freq > kMaxFreq * 1.1f)
                continue;
            const juce::Point<float> pt (freqToX (freq), mid - juce::jlimit (-kEqRangeDb, kEqRangeDb, gainDb) * scale);
            if (! started) { eq.startNewSubPath (pt); started = true; }
            else           eq.lineTo (pt);
        }
        g.setColour (kEqColour.withAlpha (0.25f));
        g.drawHorizontalLine (juce::roundToInt (mid), plot.getX(), plot.getRight());
        g.setColour (kEqColour);
        g.strokePath (eq, juce::PathStrokeType (1.8f));
    }

    g.restoreState();

    // ---- Legend --------------------------------------------------------------------------------
    {
        auto legend = juce::Rectangle<float> (plot.getRight() - 250.0f, plot.getY() + 4.0f, 246.0f, 14.0f);
        struct Item { const char* text; juce::Colour colour; };
        const Item items[] = { { "L+R", kSumColour }, { "Mid", kMidColour }, { "Side", Colours::accentBright },
                               { "Ref", kRefColour }, { "EQ", kEqColour } };
        g.setFont (makeFont (FontSize::small - 1.0f));
        float x = legend.getX();
        for (const auto& it : items)
        {
            g.setColour (it.colour);
            g.fillRect (x, legend.getCentreY() - 1.0f, 10.0f, 2.0f);
            g.setColour (Colours::textSecondary);
            g.drawText (it.text, juce::Rectangle<float> (x + 13.0f, legend.getY(), 34.0f, legend.getHeight()),
                        juce::Justification::centredLeft, false);
            x += 49.0f;
        }
    }

    // ---- Hover tooltip -------------------------------------------------------------------------
    if (hovering && plot.contains (mouse))
    {
        const float freq = xToFreq (mouse.x);
        const int c = juce::jlimit (0, juce::jmax (0, (int) display[Sum].size() - 1), (int) (mouse.x - plot.getX()));

        g.setColour (Colours::textSecondary.withAlpha (0.5f));
        g.drawVerticalLine (juce::roundToInt (mouse.x), plot.getY(), plot.getBottom());

        juce::StringArray lines;
        lines.add (formatFreq (freq) + "  " + noteName (freq));
        lines.add ("Cursor " + juce::String (yToDb (mouse.y), 1) + " dB");
        if (hasFrame && ! display[Sum].empty())
        {
            lines.add ("L+R " + juce::String (display[Sum][(size_t) c], 1) + " dB");
            lines.add ("Mid " + juce::String (display[Mid][(size_t) c], 1) + "  Side " + juce::String (display[Side][(size_t) c], 1));
        }

        const float w = 150.0f, h = 6.0f + 14.0f * (float) lines.size();
        auto box = juce::Rectangle<float> (mouse.x + 12.0f, mouse.y - h - 6.0f, w, h);
        if (box.getRight() > plot.getRight()) box.setX (mouse.x - w - 12.0f);
        if (box.getY() < plot.getY())         box.setY (mouse.y + 12.0f);

        g.setColour (Colours::panel.withAlpha (0.95f));
        g.fillRoundedRectangle (box, Radius::small);
        g.setColour (Colours::border);
        g.drawRoundedRectangle (box, Radius::small, 1.0f);
        g.setColour (Colours::textPrimary);
        g.setFont (makeFont (FontSize::small));
        for (int i = 0; i < lines.size(); ++i)
            g.drawText (lines[i], box.withTrimmedLeft (6.0f).withY (box.getY() + 3.0f + 14.0f * (float) i).withHeight (14.0f),
                        juce::Justification::centredLeft, false);
    }
}

void SpectrumDisplay::mouseMove (const juce::MouseEvent& e)
{
    mouse = e.position;
    hovering = true;
    repaint();
}

void SpectrumDisplay::mouseExit (const juce::MouseEvent&)
{
    hovering = false;
    repaint();
}

} // namespace meridian::ui
