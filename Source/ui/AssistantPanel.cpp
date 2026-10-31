#include "AssistantPanel.h"
#include "UITheme.h"

#include "PluginProcessor.h"

#include <cmath>

namespace meridian::ui
{

namespace
{
    constexpr double kAnalysisSeconds = 20.0;
    constexpr double kMinAnalysisSeconds = 3.0;
    constexpr double kCodecSeconds = 10.0;
    constexpr int kProposalRowHeight = 24;

    void styleLabel (juce::Label& l, float size, juce::Colour colour, bool bold = false)
    {
        l.setFont (makeFont (size, bold));
        l.setColour (juce::Label::textColourId, colour);
        l.setInterceptsMouseClicks (false, false);
    }

    juce::String f1 (float v) { return juce::String (v, 1); }
}

//==============================================================================
// ProposalRow
//==============================================================================
juce::String ProposalRow::moduleForParameter (const juce::String& id)
{
    struct Prefix { const char* prefix; const char* module; };
    static const Prefix table[] = {
        { "input_", "Input" }, { "ceq_", "Corrective EQ" }, { "lec_", "Low End" }, { "deq_", "Dynamic EQ" },
        { "glue_", "Glue Comp" }, { "mbd_", "Multiband" }, { "teq_", "Tone EQ" }, { "sat_", "Saturation" },
        { "img_", "Imager" }, { "clipper_", "Clipper" }, { "lim_", "Limiter" }, { "dither_", "Dither" },
        { "module_", "Routing" } };

    for (const auto& p : table)
        if (id.startsWith (p.prefix))
            return p.module;
    return "Global";
}

juce::String ProposalRow::formatValue (juce::AudioProcessorValueTreeState& state, const juce::String& id, float value)
{
    if (auto* p = state.getParameter (id))
    {
        const auto text = p->getText (p->convertTo0to1 (value), 24);
        const auto label = p->getLabel();
        // Float parameters already append their unit in the text function.
        if (label.isEmpty() || text.containsIgnoreCase (label) || dynamic_cast<juce::AudioParameterChoice*> (p) != nullptr)
            return text;
        return text + " " + label;
    }
    return juce::String (value, 2);
}

ProposalRow::ProposalRow (int i, const IntelligentAssistant::ParameterProposal& pr,
                          juce::AudioProcessorValueTreeState& state)
    : index (i)
{
    module   = moduleForParameter (pr.paramId);
    param    = IntelligentAssistant::describeParameter (state, pr.paramId);
    current  = formatValue (state, pr.paramId, pr.currentValue);
    proposed = formatValue (state, pr.paramId, pr.proposedValue);
    reason   = pr.reason;

    accept.setToggleState (pr.accepted, juce::dontSendNotification);
    accept.onClick = [this]
    {
        if (onToggle)
            onToggle (index, accept.getToggleState());
    };
    addAndMakeVisible (accept);
    accept.setTooltip (reason);
}

void ProposalRow::setAccepted (bool accepted)
{
    if (accept.getToggleState() != accepted)
        accept.setToggleState (accepted, juce::dontSendNotification);
}

void ProposalRow::resized()
{
    accept.setBounds (getLocalBounds().removeFromRight (74).reduced (2, 1));
}

void ProposalRow::paint (juce::Graphics& g)
{
    auto area = getLocalBounds().reduced (2, 1);
    g.setColour (index % 2 == 0 ? Colours::panelLight.withAlpha (0.6f) : Colours::panel);
    g.fillRoundedRectangle (area.toFloat(), Radius::small);

    area.removeFromRight (76);
    area.removeFromLeft (4);
    const int w = area.getWidth();

    g.setFont (makeFont (FontSize::small));
    g.setColour (Colours::accentBright);
    g.drawText (module, area.removeFromLeft ((int) (w * 0.13f)), juce::Justification::centredLeft, true);
    g.setColour (Colours::textPrimary);
    g.drawText (param, area.removeFromLeft ((int) (w * 0.20f)), juce::Justification::centredLeft, true);
    g.setColour (Colours::textSecondary);
    g.drawText (current, area.removeFromLeft ((int) (w * 0.14f)), juce::Justification::centredLeft, true);
    g.setColour (Colours::meterGreen);
    g.drawText (juce::String (juce::CharPointer_UTF8 ("\xe2\x86\x92 ")) + proposed,
                area.removeFromLeft ((int) (w * 0.16f)), juce::Justification::centredLeft, true);
    g.setColour (Colours::textSecondary);
    g.drawFittedText (reason, area, juce::Justification::centredLeft, 1, 0.8f);
}

//==============================================================================
// AssistantPanel
//==============================================================================
AssistantPanel::AssistantPanel (MeridianAudioProcessor& p)
    : processor (p), assistant (p.getAssistant())
{
    title.setText ("INTELLIGENT ASSISTANT", juce::dontSendNotification);
    styleLabel (title, FontSize::normal, Colours::accentBright, true);
    addAndMakeVisible (title);

    for (int i = 0; i < (int) assistant.getProfiles().size(); ++i)
        profileBox.addItem (assistant.getProfiles()[(size_t) i].name, i + 1);
    profileBox.setTooltip ("Target profile used for the analysis and proposals");
    profileBox.onChange = [this]
    {
        const auto name = profileBox.getText();
        if (name.isNotEmpty())
        {
            processor.setTargetProfile (name);
            assistant.setActiveProfile (name);
            updateStats();
        }
    };
    addAndMakeVisible (profileBox);

    analyzeButton.setTooltip ("Analyse the last 20 s of processed output");
    analyzeButton.setColour (juce::TextButton::buttonColourId, Colours::accent);
    analyzeButton.onClick = [this] { startAnalysis(); };
    addAndMakeVisible (analyzeButton);

    styleLabel (statusLabel, FontSize::small, Colours::textSecondary);
    addAndMakeVisible (statusLabel);

    styleLabel (grWarning, FontSize::small, Colours::meterRed, true);
    grWarning.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (grWarning);

    collapseButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xbe")));
    collapseButton.setTooltip ("Collapse / expand the assistant");
    collapseButton.onClick = [this]
    {
        collapsed = ! collapsed;
        collapseButton.setButtonText (juce::String (juce::CharPointer_UTF8 (collapsed ? "\xe2\x96\xb4" : "\xe2\x96\xbe")));
        for (auto* c : std::initializer_list<juce::Component*> { &statsLabel, &proposalViewport, &acceptAllButton, &rejectAllButton,
                                                                 &codecTitle, &codecBox, &codecButton, &codecResult,
                                                                 &refTitle, &refLoadButton, &refABButton, &refStatus })
            c->setVisible (! collapsed);
        if (onCollapseToggled)
            onCollapseToggled();
    };
    addAndMakeVisible (collapseButton);

    // Stats
    styleLabel (statsLabel, FontSize::small, Colours::textPrimary);
    statsLabel.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (statsLabel);

    // Proposals
    proposalViewport.setViewedComponent (&proposalList, false);
    proposalViewport.setScrollBarsShown (true, false);
    proposalViewport.setScrollBarThickness (8);
    addAndMakeVisible (proposalViewport);

    emptyLabel.setText ("Play your mix for 20 s, then press Analyze.", juce::dontSendNotification);
    styleLabel (emptyLabel, FontSize::small, Colours::textSecondary);
    emptyLabel.setJustificationType (juce::Justification::centred);
    proposalList.addAndMakeVisible (emptyLabel);

    acceptAllButton.onClick = [this]
    {
        assistant.acceptAllProposals (processor.getAPVTS());
        refreshAcceptedStates();
    };
    rejectAllButton.onClick = [this]
    {
        assistant.rejectAllProposals (processor.getAPVTS());
        refreshAcceptedStates();
    };
    addAndMakeVisible (acceptAllButton);
    addAndMakeVisible (rejectAllButton);

    // Codec preview
    codecTitle.setText ("CODEC PREVIEW", juce::dontSendNotification);
    styleLabel (codecTitle, FontSize::small, Colours::textSecondary, true);
    addAndMakeVisible (codecTitle);
    for (int c = 0; c < CodecPreview::NumCodecs; ++c)
        codecBox.addItem (CodecPreview::getCodecDisplayName (c), c + 1);
    codecBox.setSelectedId (1, juce::dontSendNotification);
    addAndMakeVisible (codecBox);
    codecButton.setTooltip ("Encode/decode the last 10 s and measure the post-codec true peak");
    codecButton.onClick = [this] { startCodecPreview(); };
    addAndMakeVisible (codecButton);
    styleLabel (codecResult, FontSize::small, Colours::textPrimary);
    codecResult.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (codecResult);

    // Reference
    refTitle.setText ("REFERENCE", juce::dontSendNotification);
    styleLabel (refTitle, FontSize::small, Colours::textSecondary, true);
    addAndMakeVisible (refTitle);
    refLoadButton.setTooltip ("Load a reference track (WAV / AIFF / FLAC / Ogg / MP3)");
    refLoadButton.onClick = [this] { chooseReference(); };
    addAndMakeVisible (refLoadButton);
    refABButton.setClickingTogglesState (true);
    refABButton.setTooltip ("Listen to the loudness-matched reference instead of the master");
    refABButton.onClick = [this] { processor.getReferencePanel().setListeningToReference (refABButton.getToggleState()); };
    refABButton.setToggleState (processor.getReferencePanel().isListeningToReference(), juce::dontSendNotification);
    addAndMakeVisible (refABButton);
    styleLabel (refStatus, FontSize::small, Colours::textPrimary);
    refStatus.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (refStatus);

    syncProfileFromProcessor();
    wasAnalyzing = assistant.isAnalyzing();
    if (assistant.getAnalysisResult().valid)
        rebuildProposalRows();
    updateStats();
}

AssistantPanel::~AssistantPanel()
{
    proposalViewport.setViewedComponent (nullptr, false);
}

void AssistantPanel::syncProfileFromProcessor()
{
    const auto target = processor.getTargetProfile();
    assistant.setActiveProfile (target);

    const auto* active = assistant.getActiveProfile();
    const auto name = active != nullptr ? active->name : target;

    for (int i = 0; i < profileBox.getNumItems(); ++i)
        if (profileBox.getItemText (i) == name)
            profileBox.setSelectedItemIndex (i, juce::dontSendNotification);
}

void AssistantPanel::startAnalysis()
{
    if (assistant.isAnalyzing())
        return;

    juce::AudioBuffer<float> capture;
    const int n = processor.copyRecentAudio (capture, kAnalysisSeconds);
    const double sr = processor.getCurrentSampleRate();

    if (n < (int) (kMinAnalysisSeconds * sr))
    {
        statusLabel.setText ("Need at least 3 s of audio - press play first.", juce::dontSendNotification);
        return;
    }

    assistant.analyzeBuffer (capture, sr, profileBox.getText());
    statusLabel.setText ("Analysing " + f1 ((float) (n / sr)) + " s...", juce::dontSendNotification);
    analyzeButton.setEnabled (false);
    wasAnalyzing = true;
    repaint (spinnerArea);
}

void AssistantPanel::rebuildProposalRows()
{
    proposalRows.clear();
    const auto proposals = assistant.getProposals();

    for (int i = 0; i < (int) proposals.size(); ++i)
    {
        auto row = std::make_unique<ProposalRow> (i, proposals[(size_t) i], processor.getAPVTS());
        row->onToggle = [this] (int index, bool accepted)
        {
            if (accepted)
                assistant.acceptProposal (index, processor.getAPVTS());
            else
                assistant.rejectProposal (index, processor.getAPVTS());
        };
        proposalList.addAndMakeVisible (*row);
        proposalRows.push_back (std::move (row));
    }

    emptyLabel.setText (assistant.getAnalysisResult().valid
                            ? juce::String ("No changes proposed - the mix already meets the profile.")
                            : juce::String ("Play your mix for 20 s, then press Analyze."),
                        juce::dontSendNotification);
    emptyLabel.setVisible (proposalRows.empty());
    acceptAllButton.setEnabled (! proposalRows.empty());
    rejectAllButton.setEnabled (! proposalRows.empty());
    resized();
}

void AssistantPanel::refreshAcceptedStates()
{
    const auto proposals = assistant.getProposals();
    for (size_t i = 0; i < proposalRows.size() && i < proposals.size(); ++i)
        proposalRows[i]->setAccepted (proposals[i].accepted);
}

void AssistantPanel::updateStats()
{
    const auto r = assistant.getAnalysisResult();
    const auto* profile = assistant.getActiveProfile();

    juce::String text;
    if (profile != nullptr)
        text << "Target: " << f1 (profile->targetLUFS_I) << " LUFS-I, TP " << f1 (profile->targetTP_dBTP)
             << " dBTP, PLR " << f1 (profile->targetPLR_min) << "-" << f1 (profile->targetPLR_max) << "\n";

    if (! r.valid)
    {
        text << "\nNo analysis yet.";
    }
    else
    {
        text << "Analysed " << f1 (r.durationSeconds) << " s\n"
             << "LUFS-I  " << f1 (r.integratedLUFS) << "   ST max " << f1 (r.shortTermPeakLUFS) << "\n"
             << "TP  " << f1 (r.truePeak_dBTP) << " dBTP   PLR " << f1 (r.PLR) << "\n"
             << "LRA  " << f1 (r.LRA) << " LU   Corr<150  " << juce::String (r.lowCorrelation, 2) << "\n"
             << "Sibilance  " << f1 (r.sibilanceEnergy_dB) << " dB\n"
             << "Mud 200-400  " << f1 (r.muddyEnergy_dB) << " dB\n"
             << "Sub 20-80  " << f1 (r.subEnergy_dB) << " dB\n"
             << "Tilt  " << (r.spectralTilt_dB >= 0.0f ? "+" : "") << f1 (r.spectralTilt_dB) << " dB "
             << (r.spectralTilt_dB < -1.5f ? "(dull)" : (r.spectralTilt_dB > 1.5f ? "(bright)" : "(balanced)"));
    }

    statsLabel.setText (text, juce::dontSendNotification);
}

void AssistantPanel::startCodecPreview()
{
    auto& codec = processor.getCodecPreview();
    if (codec.isRunning())
        return;

    juce::AudioBuffer<float> capture;
    const int n = processor.copyRecentAudio (capture, kCodecSeconds);
    const double sr = processor.getCurrentSampleRate();
    if (n < (int) (1.0 * sr))
    {
        codecResult.setText ("Need at least 1 s of audio.", juce::dontSendNotification);
        return;
    }

    codec.setInputBuffer (std::move (capture), sr);
    codec.startPreview (codecBox.getSelectedId() - 1);
    codecResult.setText ("Encoding " + codecBox.getText() + "...", juce::dontSendNotification);
    codecResult.setColour (juce::Label::textColourId, Colours::textSecondary);
    codecButton.setEnabled (false);
    wasCodecRunning = true;
}

void AssistantPanel::chooseReference()
{
    fileChooser = std::make_unique<juce::FileChooser> ("Load reference track",
                                                       juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                       "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
    juce::Component::SafePointer<AssistantPanel> safe (this);
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safe] (const juce::FileChooser& fc)
                              {
                                  if (safe == nullptr)
                                      return;
                                  const auto file = fc.getResult();
                                  if (file.existsAsFile())
                                      safe->processor.getReferencePanel().loadReference (file);
                              });
}

void AssistantPanel::update()
{
    // ---- Analysis ----------------------------------------------------------------------
    const bool analyzing = assistant.isAnalyzing();
    if (analyzing)
    {
        spinnerAngle += 0.18f;
        if (spinnerAngle > juce::MathConstants<float>::twoPi)
            spinnerAngle -= juce::MathConstants<float>::twoPi;
        repaint (spinnerArea);
    }
    else if (wasAnalyzing)
    {
        analyzeButton.setEnabled (true);
        const auto r = assistant.getAnalysisResult();
        statusLabel.setText (r.valid ? juce::String ((int) assistant.getProposals().size()) + " proposals"
                                     : juce::String ("Analysis failed (silent or too short input)"),
                             juce::dontSendNotification);
        rebuildProposalRows();
        updateStats();
        repaint (spinnerArea);
    }
    wasAnalyzing = analyzing;

    if (! analyzing && ! statusLabel.getText().containsIgnoreCase ("proposals")
        && ! statusLabel.getText().startsWith ("Need") && ! statusLabel.getText().startsWith ("Analysis failed"))
    {
        statusLabel.setText ("Captured " + f1 ((float) juce::jmin (kAnalysisSeconds, processor.getCapturedSeconds())) + " s",
                             juce::dontSendNotification);
    }

    // ---- GR budget -----------------------------------------------------------------------
    juce::String msg;
    assistant.getGRWarningMessage (msg);
    if (grWarning.getText() != msg)
        grWarning.setText (msg, juce::dontSendNotification);

    // ---- Codec preview ---------------------------------------------------------------------
    auto& codec = processor.getCodecPreview();
    const bool codecRunning = codec.isRunning();
    if (wasCodecRunning && ! codecRunning)
    {
        const auto r = codec.getResult();
        codecButton.setEnabled (true);
        if (r.success)
        {
            const bool over = r.truePeakPostEncode_dBTP > -1.0f;
            codecResult.setText (r.codecName + "\nPre  " + juce::String (r.truePeakPreEncode_dBTP, 2) + " dBTP\nPost "
                                     + juce::String (r.truePeakPostEncode_dBTP, 2) + " dBTP " + (over ? "(over -1.0!)" : "(OK)")
                                     + "\nLevel " + juce::String (r.levelDiff_dB, 2) + " dB",
                                 juce::dontSendNotification);
            codecResult.setColour (juce::Label::textColourId, over ? Colours::meterRed : Colours::meterGreen);
        }
        else
        {
            codecResult.setText ("Failed: " + r.codecName, juce::dontSendNotification);
            codecResult.setColour (juce::Label::textColourId, Colours::meterYellow);
        }
    }
    wasCodecRunning = codecRunning;

    // ---- Reference -------------------------------------------------------------------------
    auto& ref = processor.getReferencePanel();
    juce::String refText;
    switch (ref.getLoadState())
    {
        case ReferencePanel::LoadState::Empty:   refText = "No reference loaded"; break;
        case ReferencePanel::LoadState::Loading: refText = "Loading..."; break;
        case ReferencePanel::LoadState::Failed:  refText = "Failed: " + ref.getLastError(); break;
        case ReferencePanel::LoadState::Loaded:
        {
            const auto cmp = ref.getSpectralComparison();
            refText = ref.getReferenceName() + "\nRef " + f1 (cmp.refLoudness) + " LUFS, match "
                    + f1 (ref.getLevelMatchGain_dB()) + " dB\nWidth " + juce::String (juce::roundToInt (cmp.refWidth))
                    + "% vs " + juce::String (juce::roundToInt (cmp.mainWidth)) + "%";
            break;
        }
    }
    if (refStatus.getText() != refText)
        refStatus.setText (refText, juce::dontSendNotification);

    refABButton.setEnabled (ref.isLoaded());
    if (refABButton.getToggleState() != ref.isListeningToReference())
        refABButton.setToggleState (ref.isListeningToReference(), juce::dontSendNotification);
}

void AssistantPanel::paint (juce::Graphics& g)
{
    drawPanel (g, getLocalBounds().toFloat().reduced (1.0f));

    if (! collapsed)
    {
        g.setColour (Colours::border);
        g.drawHorizontalLine (kHeaderHeight, 8.0f, (float) getWidth() - 8.0f);
        if (! statsArea.isEmpty())
            g.drawVerticalLine (statsArea.getRight() + 4, (float) kHeaderHeight + 6.0f, (float) getHeight() - 6.0f);
        if (! qcArea.isEmpty())
            g.drawVerticalLine (qcArea.getX() - 5, (float) kHeaderHeight + 6.0f, (float) getHeight() - 6.0f);
    }

    if (assistant.isAnalyzing() && ! spinnerArea.isEmpty())
    {
        const auto c = spinnerArea.toFloat().getCentre();
        const float r = (float) spinnerArea.getHeight() * 0.35f;
        juce::Path arc;
        arc.addCentredArc (c.x, c.y, r, r, 0.0f, spinnerAngle, spinnerAngle + juce::MathConstants<float>::pi * 1.4f, true);
        g.setColour (Colours::accentBright);
        g.strokePath (arc, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
}

void AssistantPanel::resized()
{
    auto area = getLocalBounds().reduced (8, 0);

    auto header = area.removeFromTop (kHeaderHeight).reduced (0, 4);
    title.setBounds (header.removeFromLeft (170));
    collapseButton.setBounds (header.removeFromRight (28));
    header.removeFromRight (6);
    profileBox.setBounds (header.removeFromLeft (juce::jmin (230, header.getWidth() / 3)));
    header.removeFromLeft (6);
    analyzeButton.setBounds (header.removeFromLeft (76));
    spinnerArea = header.removeFromLeft (24);
    statusLabel.setBounds (header.removeFromLeft (juce::jmin (150, juce::jmax (0, header.getWidth() / 2))));
    grWarning.setBounds (header);

    if (collapsed)
    {
        statsArea = proposalsArea = qcArea = {};
        return;
    }

    area.removeFromTop (6);
    area.removeFromBottom (6);

    statsArea = area.removeFromLeft (juce::jmin (190, area.getWidth() / 4));
    statsLabel.setBounds (statsArea);
    area.removeFromLeft (10);

    qcArea = area.removeFromRight (juce::jmin (190, area.getWidth() / 3));
    area.removeFromRight (10);
    {
        auto qc = qcArea;
        codecTitle.setBounds (qc.removeFromTop (16));
        auto codecRow = qc.removeFromTop (24);
        codecButton.setBounds (codecRow.removeFromRight (66));
        codecRow.removeFromRight (4);
        codecBox.setBounds (codecRow);
        codecResult.setBounds (qc.removeFromTop (juce::jmin (62, qc.getHeight() / 2)));
        qc.removeFromTop (2);
        refTitle.setBounds (qc.removeFromTop (16));
        auto refRow = qc.removeFromTop (24);
        refLoadButton.setBounds (refRow.removeFromLeft (refRow.getWidth() / 2 - 2));
        refRow.removeFromLeft (4);
        refABButton.setBounds (refRow);
        refStatus.setBounds (qc);
    }

    proposalsArea = area;
    auto buttons = area.removeFromBottom (26);
    acceptAllButton.setBounds (buttons.removeFromLeft (96));
    buttons.removeFromLeft (6);
    rejectAllButton.setBounds (buttons.removeFromLeft (96));
    area.removeFromBottom (4);
    proposalViewport.setBounds (area);

    const int listW = juce::jmax (100, area.getWidth() - proposalViewport.getScrollBarThickness());
    const int listH = juce::jmax (area.getHeight(), (int) proposalRows.size() * kProposalRowHeight);
    proposalList.setSize (listW, listH);
    for (size_t i = 0; i < proposalRows.size(); ++i)
        proposalRows[i]->setBounds (0, (int) i * kProposalRowHeight, listW, kProposalRowHeight);
    emptyLabel.setBounds (0, 0, listW, juce::jmax (24, area.getHeight()));
}

} // namespace meridian::ui
