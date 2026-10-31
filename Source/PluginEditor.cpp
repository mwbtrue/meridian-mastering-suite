#include "PluginEditor.h"
#include "PluginProcessor.h"

#include "dsp/CorrectiveEQ.h"
#include "dsp/ToneEQ.h"

#include <cmath>

using namespace meridian;
namespace mui = meridian::ui;

namespace
{
    constexpr int kEqCurvePoints = 240;
    constexpr int kScopeSamples = 1024;
    constexpr int kSlowDivider = 6;   // 60 Hz / 6 = 10 Hz

    float loadParam (juce::AudioProcessorValueTreeState& state, const juce::String& id, float fallback)
    {
        auto* raw = state.getRawParameterValue (id);
        return raw != nullptr ? raw->load() : fallback;
    }
}

MeridianAudioProcessorEditor::MeridianAudioProcessorEditor (MeridianAudioProcessor& p)
    : juce::AudioProcessorEditor (p),
      meridianProcessor (p),
      presetBrowser (p),
      moduleRack (p),
      assistantPanel (p)
{
    setLookAndFeel (&lookAndFeel);

    logoLabel.setText ("MERIDIAN", juce::dontSendNotification);
    logoLabel.setFont (mui::makeFont (22.0f, true));
    logoLabel.setColour (juce::Label::textColourId, mui::Colours::accent);
    addAndMakeVisible (logoLabel);

    latencyLabel.setFont (mui::makeFont (mui::FontSize::small));
    latencyLabel.setColour (juce::Label::textColourId, mui::Colours::textSecondary);
    latencyLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (latencyLabel);

    gearButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xe2\x9a\x99")));
    gearButton.setTooltip ("Settings");
    gearButton.onClick = [this]
    {
        DBG ("Meridian settings: latency " << meridianProcessor.getTotalLatencySamples() << " samples @ "
             << meridianProcessor.getCurrentSampleRate() << " Hz, target profile '"
             << meridianProcessor.getTargetProfile() << "', snapshot " << meridianProcessor.getActiveSnapshot());
    };
    addAndMakeVisible (gearButton);

    addAndMakeVisible (presetBrowser);
    presetBrowser.onPresetLoaded = [this]
    {
        assistantPanel.syncProfileFromProcessor();
        moduleRack.refreshOrder();
    };

    rackViewport.setViewedComponent (&moduleRack, false);
    rackViewport.setScrollBarsShown (true, false);
    rackViewport.setScrollBarThickness (8);
    addAndMakeVisible (rackViewport);

    addAndMakeVisible (spectrumDisplay);

    assistantPanel.onCollapseToggled = [this] { resized(); };
    addAndMakeVisible (assistantPanel);

    meterColumn.onResetIntegrated = [this] { meridianProcessor.requestIntegratedLoudnessReset(); };
    addAndMakeVisible (meterColumn);

    lastTargetProfile = meridianProcessor.getTargetProfile();

    setResizable (true, true);
    setResizeLimits (kMinWidth, kMinHeight, kMaxWidth, kMaxHeight);
    setSize (kDefaultWidth, kDefaultHeight);

    updateSlowState();
    startTimerHz (kTimerHz);
}

MeridianAudioProcessorEditor::~MeridianAudioProcessorEditor()
{
    stopTimer();
    rackViewport.setViewedComponent (nullptr, false);
    setLookAndFeel (nullptr);
}

void MeridianAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (mui::Colours::background);

    g.setColour (mui::Colours::panel);
    g.fillRect (0, 0, getWidth(), kTopBarHeight);
    g.setColour (mui::Colours::border);
    g.drawHorizontalLine (kTopBarHeight - 1, 0.0f, (float) getWidth());
}

void MeridianAudioProcessorEditor::resized()
{
    auto area = getLocalBounds();

    auto top = area.removeFromTop (kTopBarHeight).reduced (12, 8);
    logoLabel.setBounds (top.removeFromLeft (140));
    gearButton.setBounds (top.removeFromRight (32));
    top.removeFromRight (8);
    latencyLabel.setBounds (top.removeFromRight (180));
    top.removeFromRight (8);
    presetBrowser.setBounds (top.removeFromLeft (juce::jmin (520, top.getWidth())));

    area.reduce (4, 4);

    rackViewport.setBounds (area.removeFromLeft (kRackWidth));
    moduleRack.setSize (kRackWidth - rackViewport.getScrollBarThickness(), moduleRack.getIdealHeight());
    area.removeFromLeft (4);

    meterColumn.setBounds (area.removeFromRight (kMeterWidth));
    area.removeFromRight (4);

    if (assistantPanel.isCollapsed())
    {
        assistantPanel.setBounds (area.removeFromBottom (mui::AssistantPanel::kHeaderHeight));
    }
    else
    {
        assistantPanel.setBounds (area.removeFromBottom (juce::roundToInt ((float) area.getHeight() * 0.35f)));
    }
    area.removeFromBottom (4);
    spectrumDisplay.setBounds (area);
}

std::vector<std::pair<float, float>> MeridianAudioProcessorEditor::computeEQCurve() const
{
    auto& state = meridianProcessor.getAPVTS();
    const double sr = meridianProcessor.getCurrentSampleRate();

    const bool ceqActive = loadParam (state, pid::slotParam (pid::moduleBypassPrefix, (int) ModuleId::CorrectiveEQ), 0.0f) < 0.5f;
    const bool teqActive = loadParam (state, pid::slotParam (pid::moduleBypassPrefix, (int) ModuleId::ToneEQ), 0.0f) < 0.5f;

    std::array<CorrectiveEQ::BandSettings, kNumCeqBands> bands {};
    int numBands = 0;
    if (ceqActive)
    {
        for (int b = 0; b < kNumCeqBands; ++b)
        {
            using namespace pid;
            if (loadParam (state, bandParam (ceqBandPrefix, b, field::enabled), 0.0f) < 0.5f)
                continue;

            CorrectiveEQ::BandSettings s;
            s.freq       = loadParam (state, bandParam (ceqBandPrefix, b, field::freq), 1000.0f);
            s.gainDb     = loadParam (state, bandParam (ceqBandPrefix, b, field::gain), 0.0f);
            s.q          = loadParam (state, bandParam (ceqBandPrefix, b, field::q), 0.7f);
            s.type       = juce::roundToInt (loadParam (state, bandParam (ceqBandPrefix, b, field::type), 0.0f));
            s.slopeIndex = juce::roundToInt (loadParam (state, bandParam (ceqBandPrefix, b, field::slope),
                                                        (float) choices::ceqDefaultSlopeIndex));
            s.channel    = juce::roundToInt (loadParam (state, bandParam (ceqBandPrefix, b, field::channel), 0.0f));
            s.enabled    = true;

            // Bell / shelf / tilt bands at 0 dB are flat: skip them.
            if (CorrectiveEQ::typeUsesGain (s.type) && std::abs (s.gainDb) < 0.01)
                continue;

            bands[(size_t) numBands++] = s;
        }
    }

    const auto* toneEq = teqActive ? dynamic_cast<const ToneEQ*> (meridianProcessor.getModule (ModuleId::ToneEQ)) : nullptr;

    std::vector<std::pair<float, float>> curve;
    curve.reserve (kEqCurvePoints);
    const double nyquistLimit = sr * 0.499;

    for (int i = 0; i < kEqCurvePoints; ++i)
    {
        const double f = 20.0 * std::pow (1000.0, (double) i / (double) (kEqCurvePoints - 1));
        if (f >= nyquistLimit)
            break;

        double db = 0.0;
        for (int b = 0; b < numBands; ++b)
        {
            const double mag = CorrectiveEQ::computeBandMagnitude (bands[(size_t) b], f, sr);
            db += 20.0 * std::log10 (juce::jmax (1.0e-6, mag));
        }

        if (toneEq != nullptr)
            db += toneEq->getStereoMagnitudeDb (f);

        curve.emplace_back ((float) f, (float) db);
    }

    return curve;
}

void MeridianAudioProcessorEditor::updateSlowState()
{
    // Latency readout
    const int latency = meridianProcessor.getTotalLatencySamples();
    const double sr = meridianProcessor.getCurrentSampleRate();
    latencyLabel.setText ("Latency " + juce::String (latency) + " smp (" + juce::String (sr > 0.0 ? 1000.0 * latency / sr : 0.0, 1)
                              + " ms)", juce::dontSendNotification);

    // LUFS target for the meter colouring
    meterColumn.setTargetLUFS (loadParam (meridianProcessor.getAPVTS(), pid::str (pid::lufsTarget), -9.0f));

    // EQ overlay
    spectrumDisplay.setEQCurve (computeEQCurve());

    // Reference curve
    auto& ref = meridianProcessor.getReferencePanel();
    if (ref.isLoaded())
    {
        const auto spectrum = ref.getReferenceSpectrum();
        spectrumDisplay.setReferenceCurve (std::vector<float> (spectrum.begin(), spectrum.end()), sr);
        referenceShown = true;
    }
    else if (referenceShown)
    {
        spectrumDisplay.setReferenceCurve ({}, sr);
        referenceShown = false;
    }

    // State-driven widgets (host state restore, snapshot recall, presets)
    const auto target = meridianProcessor.getTargetProfile();
    if (target != lastTargetProfile)
    {
        lastTargetProfile = target;
        assistantPanel.syncProfileFromProcessor();
    }

    presetBrowser.updateSnapshotButtons();
}

void MeridianAudioProcessorEditor::timerCallback()
{
    // ---- Meters: drain the queue, keep the newest ----------------------------------------------
    bool gotMeters = false;
    while (meridianProcessor.popMeterData (meterScratch))
        gotMeters = true;

    if (gotMeters)
    {
        meterColumn.setMeterData (meterScratch);
        moduleRack.setMeterData (meterScratch);
    }

    // ---- Spectrum: every frame feeds the display ballistics and the reference comparison ------
    auto& ref = meridianProcessor.getReferencePanel();
    while (meridianProcessor.popSpectrumFrame (spectrumScratch))
    {
        spectrumDisplay.setSpectrumFrame (spectrumScratch);
        ref.setMainSpectrumFrame (spectrumScratch);
    }

    // ---- Goniometer + per-channel peaks from the capture ring ---------------------------------
    const double sr = meridianProcessor.getCurrentSampleRate();
    if (sr > 0.0)
    {
        const int n = meridianProcessor.copyRecentAudio (scopeBuffer, (double) kScopeSamples / sr);
        if (n > 0 && gotMeters)
            meterColumn.pushAudio (scopeBuffer.getReadPointer (0), scopeBuffer.getReadPointer (1), n);
    }

    meterColumn.tick (1.0f / (float) kTimerHz);
    assistantPanel.update();

    if (++slowCounter >= kSlowDivider)
    {
        slowCounter = 0;
        updateSlowState();
    }
}
