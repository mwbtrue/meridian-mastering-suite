#include "ModuleRack.h"
#include "UITheme.h"

#include "PluginProcessor.h"
#include "ParameterIDs.h"
#include "dsp/ModuleBase.h"

#include <cstring>

namespace meridian::ui
{

using APVTS = juce::AudioProcessorValueTreeState;

//==============================================================================
// MiniGRMeter
//==============================================================================
void MiniGRMeter::setGainReduction (float db)
{
    const float v = juce::jlimit (0.0f, 24.0f, std::abs (db));
    if (std::abs (v - value) > 0.01f)
    {
        value = v;
        repaint();
    }
}

void MiniGRMeter::paint (juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat();
    auto bar = area.removeFromTop (area.getHeight() * 0.45f).reduced (0.0f, 1.0f);

    g.setColour (Colours::knobTrack);
    g.fillRoundedRectangle (bar, 2.0f);
    g.setColour (Colours::gainReduction);
    g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * juce::jmin (1.0f, value / 12.0f)), 2.0f);

    g.setColour (value >= 0.05f ? Colours::textPrimary : Colours::textSecondary);
    g.setFont (makeFont (FontSize::small - 2.0f));
    g.drawText ("GR " + juce::String (value, 1), area, juce::Justification::centred, false);
}

//==============================================================================
// QuickControl
//==============================================================================
QuickControl::QuickControl (APVTS& state, juce::RangedAudioParameter& param)
{
    const auto id = param.getParameterID();

    juce::String name = param.getName (40);
    // Drop the module prefix ("Glue Threshold" -> "Threshold") where the row already names the module.
    for (const char* prefix : { "CEQ ", "DEQ ", "MBD ", "Glue ", "Imager ", "Saturation ", "Clipper ",
                                "Limiter ", "Dither ", "Input ", "Low End ", "LEC ", "Tone EQ ", "TEQ " })
        if (name.startsWith (prefix))
            name = name.substring ((int) std::strlen (prefix));

    caption.setText (name, juce::dontSendNotification);
    caption.setJustificationType (juce::Justification::centred);
    caption.setFont (makeFont (FontSize::small - 2.0f));
    caption.setColour (juce::Label::textColourId, Colours::textSecondary);
    caption.setMinimumHorizontalScale (0.6f);
    caption.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (caption);

    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (&param))
    {
        combo = std::make_unique<juce::ComboBox>();
        combo->addItemList (choice->choices, 1);
        addAndMakeVisible (*combo);
        comboAttachment = std::make_unique<APVTS::ComboBoxAttachment> (state, id, *combo);
    }
    else if (dynamic_cast<juce::AudioParameterBool*> (&param) != nullptr)
    {
        toggle = std::make_unique<juce::ToggleButton> ("On");
        addAndMakeVisible (*toggle);
        buttonAttachment = std::make_unique<APVTS::ButtonAttachment> (state, id, *toggle);
    }
    else
    {
        slider = std::make_unique<juce::Slider> (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow);
        slider->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 72, 14);
        slider->setPopupDisplayEnabled (false, false, nullptr);
        addAndMakeVisible (*slider);
        sliderAttachment = std::make_unique<APVTS::SliderAttachment> (state, id, *slider);
        slider->setDoubleClickReturnValue (true, param.convertFrom0to1 (param.getDefaultValue()));
    }

    const auto tip = param.getName (80);
    if (slider != nullptr) slider->setTooltip (tip);
    if (combo != nullptr)  combo->setTooltip (tip);
    if (toggle != nullptr) toggle->setTooltip (tip);
}

void QuickControl::resized()
{
    auto area = getLocalBounds();
    caption.setBounds (area.removeFromTop (14));

    if (slider != nullptr)
        slider->setBounds (area);
    else if (combo != nullptr)
        combo->setBounds (area.withSizeKeepingCentre (area.getWidth() - 2, 22));
    else if (toggle != nullptr)
        toggle->setBounds (area.withSizeKeepingCentre (juce::jmin (area.getWidth(), 50), 22));
}

//==============================================================================
// ModuleRow
//==============================================================================
std::vector<juce::String> ModuleRow::quickControlIds (int moduleId)
{
    using namespace pid;
    std::vector<juce::String> ids;

    switch (static_cast<ModuleId> (moduleId))
    {
        case ModuleId::Input:
            ids = { str (inputTrim), str (inputPolarity), str (inputDcBlock) };
            break;
        case ModuleId::CorrectiveEQ:
            ids = { str (ceqPhaseMode),
                    bandParam (ceqBandPrefix, 0, field::freq), bandParam (ceqBandPrefix, 0, field::gain),
                    bandParam (ceqBandPrefix, 1, field::freq), bandParam (ceqBandPrefix, 1, field::gain),
                    bandParam (ceqBandPrefix, 2, field::freq), bandParam (ceqBandPrefix, 2, field::gain) };
            break;
        case ModuleId::LowEndControl:
            ids = { str (lecMonoEnabled), str (lecMonoFreq), str (lecDynEnabled), str (lecDynThreshold), str (lecDynMaxGr) };
            break;
        case ModuleId::DynamicEQ:
            ids = { bandParam (deqBandPrefix, 2, field::freq), bandParam (deqBandPrefix, 2, field::threshold),
                    bandParam (deqBandPrefix, 2, field::maxGain), str (deqResonEnabled) };
            break;
        case ModuleId::GlueCompressor:
            ids = { str (glueThreshold), str (glueRatio), str (glueAttack), str (glueRelease),
                    str (glueMakeup), str (glueMix), str (glueAutoRelease), str (glueScHpf) };
            break;
        case ModuleId::MultibandDynamics:
            for (int x = 0; x < kNumMbdXovers; ++x)
                ids.push_back (xoverParam (mbdXoverPrefix, x));
            for (int b = 0; b < kNumMbdBands; ++b)
                ids.push_back (bandParam (mbdBandPrefix, b, field::threshold));
            break;
        case ModuleId::ToneEQ:
            ids = { str (teqBaxandallBass), str (teqBaxandallTreble), str (teqPultecBassBoost),
                    str (teqPultecTrebleBoost), str (teqSideAirGain), str (teqColour) };
            break;
        case ModuleId::Saturation:
            ids = { str (satModel), str (satDrive), str (satBias), str (satMix), str (satOversampling), str (satAutoGain) };
            break;
        case ModuleId::StereoImager:
            for (int b = 0; b < kNumImgBands; ++b)
                ids.push_back (bandParam (imgBandPrefix, b, field::width));
            ids.push_back (str (imgCorrGuard));
            break;
        case ModuleId::Clipper:
            ids = { str (clipperEnabled), str (clipperMode), str (clipperCeiling), str (clipperDrive) };
            break;
        case ModuleId::TruePeakLimiter:
            ids = { str (limCeiling), str (limRelease), str (limStyle), str (limOversampling), str (limLookahead), str (limAutoRelease) };
            break;
        case ModuleId::Dither:
            ids = { str (ditherEnabled), str (ditherBits), str (ditherShape) };
            break;
        case ModuleId::Count:
            break;
    }

    return ids;
}

ModuleRow::ModuleRow (ModuleRack& owner, APVTS& state, int id)
    : rack (owner), moduleId (id)
{
    const auto name = kModuleNames[(size_t) moduleId];
    nameLabel.setText (juce::String (name.data(), name.size()), juce::dontSendNotification);
    nameLabel.setFont (makeFont (FontSize::normal, true));
    nameLabel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (nameLabel);

    slotLabel.setFont (makeFont (FontSize::small - 1.0f));
    slotLabel.setColour (juce::Label::textColourId, Colours::textSecondary);
    slotLabel.setJustificationType (juce::Justification::centred);
    slotLabel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (slotLabel);

    bypassButton.setClickingTogglesState (true);
    bypassButton.setColour (juce::TextButton::buttonOnColourId, Colours::meterYellow.darker (0.3f));
    bypassButton.setTooltip ("Bypass (latency-compensated)");
    addAndMakeVisible (bypassButton);

    deltaButton.setClickingTogglesState (true);
    deltaButton.setTooltip ("Delta solo: hear only what this module adds/removes");
    addAndMakeVisible (deltaButton);

    settingsButton.setTooltip ("Settings: show quick controls");
    settingsButton.onClick = [this]
    {
        expanded = ! expanded;
        settingsButton.setButtonText (expanded ? juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92")) : juce::String ("+"));
        for (auto& qc : quickControls)
            qc->setVisible (expanded);
        rack.rowLayoutChanged();
    };
    addAndMakeVisible (settingsButton);

    wetDry.setSliderStyle (juce::Slider::LinearHorizontal);
    wetDry.setTextBoxStyle (juce::Slider::TextBoxRight, false, 44, 18);
    wetDry.setTooltip ("Wet/dry mix");
    addAndMakeVisible (wetDry);

    const auto channelId = pid::slotParam (pid::moduleChannelPrefix, moduleId);
    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (state.getParameter (channelId)))
        channel.addItemList (choice->choices, 1);
    channel.setTooltip ("Channel routing");
    addAndMakeVisible (channel);

    addAndMakeVisible (grMeter);

    bypassAttachment  = std::make_unique<APVTS::ButtonAttachment> (state, pid::slotParam (pid::moduleBypassPrefix, moduleId), bypassButton);
    deltaAttachment   = std::make_unique<APVTS::ButtonAttachment> (state, pid::slotParam (pid::moduleDeltaPrefix, moduleId), deltaButton);
    wetDryAttachment  = std::make_unique<APVTS::SliderAttachment> (state, pid::slotParam (pid::moduleWetDryPrefix, moduleId), wetDry);
    channelAttachment = std::make_unique<APVTS::ComboBoxAttachment> (state, channelId, channel);
    wetDry.setTextValueSuffix (" %");

    bypassButton.onStateChange = [this] { repaint(); };

    buildQuickControls (state);
}

void ModuleRow::buildQuickControls (APVTS& state)
{
    for (const auto& id : quickControlIds (moduleId))
    {
        if (auto* param = dynamic_cast<juce::RangedAudioParameter*> (state.getParameter (id)))
        {
            auto qc = std::make_unique<QuickControl> (state, *param);
            addChildComponent (*qc);
            quickControls.push_back (std::move (qc));
        }
    }

    settingsButton.setEnabled (! quickControls.empty());
}

void ModuleRow::setSlot (int s)
{
    slot = s;
    slotLabel.setText (juce::String (s + 1).paddedLeft ('0', 2), juce::dontSendNotification);
}

int ModuleRow::getIdealHeight() const noexcept
{
    if (! expanded || quickControls.empty())
        return kCollapsedHeight;

    const int rowsOfControls = ((int) quickControls.size() + kControlsPerRow - 1) / kControlsPerRow;
    return kCollapsedHeight + rowsOfControls * kControlCellHeight + 6;
}

void ModuleRow::setActive (bool shouldBeActive)
{
    if (active != shouldBeActive)
    {
        active = shouldBeActive;
        repaint();
    }
}

void ModuleRow::resized()
{
    auto area = getLocalBounds().reduced (6, 4);

    auto line1 = area.removeFromTop (24);
    gripArea = line1.removeFromLeft (12);
    slotLabel.setBounds (line1.removeFromLeft (20));
    settingsButton.setBounds (line1.removeFromRight (24).reduced (0, 1));
    line1.removeFromRight (3);
    bypassButton.setBounds (line1.removeFromRight (40).reduced (0, 1));
    line1.removeFromRight (3);
    deltaButton.setBounds (line1.removeFromRight (26).reduced (0, 1));
    nameLabel.setBounds (line1);

    area.removeFromTop (3);
    auto line2 = area.removeFromTop (22);
    grMeter.setBounds (line2.removeFromRight (48));
    line2.removeFromRight (4);
    channel.setBounds (line2.removeFromRight (72));
    line2.removeFromRight (4);
    wetDry.setBounds (line2);

    if (expanded && ! quickControls.empty())
    {
        area.removeFromTop (6);
        const int cellW = area.getWidth() / kControlsPerRow;
        for (size_t i = 0; i < quickControls.size(); ++i)
        {
            const int col = (int) i % kControlsPerRow;
            const int row = (int) i / kControlsPerRow;
            quickControls[i]->setBounds (area.getX() + col * cellW, area.getY() + row * kControlCellHeight,
                                         cellW, kControlCellHeight - 4);
        }
    }
}

void ModuleRow::paint (juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat().reduced (2.0f, 1.5f);
    const bool bypassed = bypassButton.getToggleState();

    g.setColour (active ? Colours::panelLight : Colours::panel);
    g.fillRoundedRectangle (area, Radius::normal);
    g.setColour (active ? Colours::accent : Colours::border);
    g.drawRoundedRectangle (area, Radius::normal, active ? 1.5f : 1.0f);

    if (active)
    {
        g.setColour (Colours::accent);
        g.fillRoundedRectangle (area.withWidth (3.0f), 1.5f);
    }

    // Grip: three short lines.
    g.setColour (Colours::textSecondary.withAlpha (0.7f));
    const auto grip = gripArea.toFloat();
    for (int i = 0; i < 3; ++i)
        g.fillRect (grip.getX() + 1.0f, grip.getCentreY() - 4.0f + 4.0f * (float) i, grip.getWidth() - 3.0f, 1.2f);

    nameLabel.setColour (juce::Label::textColourId, bypassed ? Colours::textSecondary : Colours::textPrimary);
}

void ModuleRow::paintOverChildren (juce::Graphics& g)
{
    if (bypassButton.getToggleState())
    {
        g.setColour (Colours::background.withAlpha (0.35f));
        g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (2.0f, 1.5f).withTrimmedTop (30.0f), Radius::normal);
    }

    if (dropHighlight)
    {
        g.setColour (Colours::accentBright);
        const float y = dropAbove ? 1.0f : (float) getHeight() - 3.0f;
        g.fillRoundedRectangle (4.0f, y, (float) getWidth() - 8.0f, 2.5f, 1.0f);
    }
}

void ModuleRow::mouseDown (const juce::MouseEvent&)
{
    rack.setActiveModule (moduleId);
}

void ModuleRow::mouseDrag (const juce::MouseEvent& e)
{
    if (e.getDistanceFromDragStart() < 5 || e.getMouseDownY() > 30)
        return;

    if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor (this))
        if (! container->isDragAndDropActive())
            container->startDragging (juce::var (slot), this);
}

bool ModuleRow::isInterestedInDragSource (const SourceDetails& details)
{
    return dynamic_cast<ModuleRow*> (details.sourceComponent.get()) != nullptr && details.description.isInt();
}

void ModuleRow::itemDragEnter (const SourceDetails& details)
{
    itemDragMove (details);
}

void ModuleRow::itemDragMove (const SourceDetails& details)
{
    const int from = (int) details.description;
    const bool highlight = from != slot;
    const bool above = from > slot;
    if (highlight != dropHighlight || above != dropAbove)
    {
        dropHighlight = highlight;
        dropAbove = above;
        repaint();
    }
}

void ModuleRow::itemDragExit (const SourceDetails&)
{
    dropHighlight = false;
    repaint();
}

void ModuleRow::itemDropped (const SourceDetails& details)
{
    dropHighlight = false;
    repaint();
    rack.moveModule ((int) details.description, slot);
}

//==============================================================================
// ModuleRack
//==============================================================================
ModuleRack::ModuleRack (MeridianAudioProcessor& p)
    : processor (p)
{
    for (int m = 0; m < MeterData::numModules; ++m)
    {
        rows[(size_t) m] = std::make_unique<ModuleRow> (*this, processor.getAPVTS(), m);
        addAndMakeVisible (*rows[(size_t) m]);
    }

    order = readOrder();
    for (int s = 0; s < MeterData::numModules; ++s)
        rows[(size_t) order[(size_t) s]]->setSlot (s);

    setSize (272, getIdealHeight());
}

ModuleRack::~ModuleRack() = default;

std::array<int, MeterData::numModules> ModuleRack::readOrder() const
{
    // The parameters are the source of truth (they change on preset / snapshot
    // recall even when no audio is running); fall back to the audio thread's
    // published order while a multi-parameter rewrite is in flight.
    std::array<int, MeterData::numModules> candidate {};
    auto& state = processor.getAPVTS();
    for (int s = 0; s < MeterData::numModules; ++s)
    {
        auto* raw = state.getRawParameterValue (pid::slotParam (pid::moduleOrderPrefix, s));
        candidate[(size_t) s] = raw != nullptr ? juce::roundToInt (raw->load()) : s;
    }

    return MeridianAudioProcessor::isValidModuleOrder (candidate) ? candidate : processor.getModuleOrder();
}

void ModuleRack::refreshOrder()
{
    const auto newOrder = readOrder();
    if (newOrder == order)
        return;

    order = newOrder;
    for (int s = 0; s < MeterData::numModules; ++s)
        rows[(size_t) order[(size_t) s]]->setSlot (s);
    resized();
}

void ModuleRack::setMeterData (const MeterData& data)
{
    refreshOrder();
    for (int m = 0; m < MeterData::numModules; ++m)
        rows[(size_t) m]->setGainReduction (data.gainReductionPerModule[m]);
}

void ModuleRack::setActiveModule (int moduleId)
{
    activeModule = moduleId;
    for (auto& r : rows)
        r->setActive (r->getModuleId() == moduleId);

    if (onModuleSelected)
        onModuleSelected (moduleId);
}

int ModuleRack::getIdealHeight() const
{
    int h = 4;
    for (const auto& r : rows)
        h += r->getIdealHeight();
    return h + 4;
}

void ModuleRack::moveModule (int fromSlot, int toSlot)
{
    if (processor.moveModule (fromSlot, toSlot))
        refreshOrder();
}

void ModuleRack::rowLayoutChanged()
{
    const int h = getIdealHeight();
    if (h != getHeight())
        setSize (getWidth(), h);
    else
        resized();

    if (onHeightChanged)
        onHeightChanged();
}

void ModuleRack::paint (juce::Graphics& g)
{
    g.fillAll (Colours::background);
}

void ModuleRack::resized()
{
    int y = 4;
    for (int s = 0; s < MeterData::numModules; ++s)
    {
        auto& row = *rows[(size_t) order[(size_t) s]];
        const int h = row.getIdealHeight();
        row.setBounds (0, y, getWidth(), h);
        y += h;
    }
}

} // namespace meridian::ui
