#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "ui/UITheme.h"
#include "ui/ModuleRack.h"
#include "ui/SpectrumDisplay.h"
#include "ui/MeterColumn.h"
#include "ui/PresetBrowser.h"
#include "ui/AssistantPanel.h"

#include <memory>
#include <utility>
#include <vector>

class MeridianAudioProcessor;

/**
    Meridian main editor.

        +------------------------------------------------------------------+
        | MERIDIAN [preset v][Save][A][B][C][D] [Undo][Redo][LM] latency [gear]| 48 px
        +-------------+------------------------------------+---------------+
        | ModuleRack  | SpectrumDisplay (65 %)             | MeterColumn   |
        | (Viewport,  |------------------------------------|  (180 px)     |
        |  280 px)    | AssistantPanel (35 %, collapsible) |               |
        +-------------+------------------------------------+---------------+

    Default 1280 x 768, resizable 960 x 576 .. 1920 x 1080. A 60 Hz timer
    drains the processor's meter / spectrum queues, feeds the goniometer and
    peak bars from the capture ring, and (at 10 Hz) refreshes the EQ overlay,
    the reference curve and the state-dependent widgets.
*/
class MeridianAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                           private juce::Timer
{
public:
    static constexpr int kDefaultWidth = 1280, kDefaultHeight = 768;
    static constexpr int kMinWidth = 960, kMinHeight = 576;
    static constexpr int kMaxWidth = 1920, kMaxHeight = 1080;
    static constexpr int kTopBarHeight = 48;
    static constexpr int kRackWidth = 280;
    static constexpr int kMeterWidth = 180;
    static constexpr int kTimerHz = 60;

    explicit MeridianAudioProcessorEditor (MeridianAudioProcessor& processor);
    ~MeridianAudioProcessorEditor() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

    /** Ctrl/Cmd+Z undo, Ctrl/Cmd+Shift+Z or Ctrl+Y redo. */
    bool keyPressed (const juce::KeyPress& key) override;

private:
    void timerCallback() override;
    void updateSlowState();
    std::vector<std::pair<float, float>> computeEQCurve() const;

    MeridianAudioProcessor& meridianProcessor;

    // The look-and-feel must outlive every child component.
    meridian::ui::MeridianLookAndFeel lookAndFeel;
    juce::TooltipWindow tooltipWindow { this, 600 };

    juce::Label logoLabel, latencyLabel;
    juce::TextButton gearButton;
    juce::TextButton undoButton { "Undo" }, redoButton { "Redo" };
    juce::TextButton lmBypassButton { "LM" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> lmBypassAttachment;
    meridian::ui::PresetBrowser presetBrowser;

    juce::Viewport rackViewport;
    meridian::ui::ModuleRack moduleRack;
    meridian::ui::SpectrumDisplay spectrumDisplay;
    meridian::ui::AssistantPanel assistantPanel;
    meridian::ui::MeterColumn meterColumn;

    meridian::MeterData meterScratch;
    meridian::SpectrumFrame spectrumScratch;
    juce::AudioBuffer<float> scopeBuffer;

    int slowCounter = 0;
    bool referenceShown = false;
    juce::String lastTargetProfile;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MeridianAudioProcessorEditor)
};
