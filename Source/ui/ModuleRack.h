#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "common/AnalysisTypes.h"

#include <array>
#include <functional>
#include <memory>
#include <vector>

class MeridianAudioProcessor;

namespace meridian::ui
{

/** Small horizontal gain-reduction bar (0..12 dB). */
class MiniGRMeter final : public juce::Component
{
public:
    void setGainReduction (float db);
    void paint (juce::Graphics&) override;

private:
    float value = 0.0f;
};

/** One labelled APVTS-attached control (rotary slider, toggle or combo box). */
class QuickControl final : public juce::Component
{
public:
    QuickControl (juce::AudioProcessorValueTreeState& state, juce::RangedAudioParameter& param);

    void resized() override;

private:
    juce::Label caption;
    std::unique_ptr<juce::Slider> slider;
    std::unique_ptr<juce::ToggleButton> toggle;
    std::unique_ptr<juce::ComboBox> combo;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sliderAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> buttonAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> comboAttachment;
};

class ModuleRack;

/**
    One module strip: drag grip + name, bypass, delta solo, settings expander,
    wet/dry slider, channel-routing combo, GR meter. Expanded rows show the
    module's quick controls. Rows are drag sources (via the grip / name) and
    drop targets; a drop moves the dragged module into this row's slot.
*/
class ModuleRow final : public juce::Component,
                        public juce::DragAndDropTarget
{
public:
    static constexpr int kCollapsedHeight = 58;
    static constexpr int kControlCellHeight = 74;
    static constexpr int kControlsPerRow = 3;

    ModuleRow (ModuleRack& owner, juce::AudioProcessorValueTreeState& state, int moduleId);

    int getModuleId() const noexcept { return moduleId; }
    void setSlot (int s);
    int getSlot() const noexcept { return slot; }

    void setActive (bool shouldBeActive);
    bool isExpanded() const noexcept { return expanded; }
    int getIdealHeight() const noexcept;
    void setGainReduction (float db) { grMeter.setGainReduction (db); }

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;

    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragEnter (const SourceDetails&) override;
    void itemDragMove (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;

private:
    void buildQuickControls (juce::AudioProcessorValueTreeState& state);
    static std::vector<juce::String> quickControlIds (int moduleId);

    ModuleRack& rack;
    const int moduleId;
    int slot = 0;
    bool active = false, expanded = false, dropHighlight = false, dropAbove = true;

    juce::Label nameLabel, slotLabel;
    juce::TextButton bypassButton { "BYP" }, deltaButton { juce::String (juce::CharPointer_UTF8 ("\xce\x94")) },
                     settingsButton { "+" };
    juce::Slider wetDry;
    juce::ComboBox channel;
    MiniGRMeter grMeter;
    juce::Rectangle<int> gripArea;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> bypassAttachment, deltaAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> wetDryAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> channelAttachment;

    std::vector<std::unique_ptr<QuickControl>> quickControls;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModuleRow)
};

/**
    The 12-module signal-chain rack (left column, hosted in a Viewport).
    Row order follows the module_order parameters (slot -> ModuleId); drag a
    row's grip/name onto another row to reorder (MeridianAudioProcessor::moveModule).
*/
class ModuleRack final : public juce::Component,
                         public juce::DragAndDropContainer
{
public:
    explicit ModuleRack (MeridianAudioProcessor& processor);
    ~ModuleRack() override;

    /** Per-frame update from the editor timer: order changes + GR meters. */
    void setMeterData (const MeterData& data);
    void refreshOrder();

    void setActiveModule (int moduleId);
    int getActiveModule() const noexcept { return activeModule; }

    int getIdealHeight() const;
    /** Called whenever the rack's ideal height changes (row expanded / collapsed). */
    std::function<void()> onHeightChanged;
    /** Called when the user selects a row. */
    std::function<void (int moduleId)> onModuleSelected;

    void paint (juce::Graphics&) override;
    void resized() override;

    // Used by ModuleRow
    void moveModule (int fromSlot, int toSlot);
    void rowLayoutChanged();

private:
    std::array<int, MeterData::numModules> readOrder() const;

    MeridianAudioProcessor& processor;
    std::array<std::unique_ptr<ModuleRow>, MeterData::numModules> rows;   // indexed by ModuleId
    std::array<int, MeterData::numModules> order {};
    int activeModule = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModuleRack)
};

} // namespace meridian::ui
