#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

class MeridianAudioProcessor;

/**
    Minimal editor used until the full UI (ModuleRack, SpectrumDisplay,
    MeterColumn, PresetBrowser, AssistantPanel) is generated. Shows the product
    name, version and the current chain latency in an 800 x 600 window.
*/
class MeridianAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                           private juce::Timer
{
public:
    explicit MeridianAudioProcessorEditor (MeridianAudioProcessor& processor);
    ~MeridianAudioProcessorEditor() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;

    MeridianAudioProcessor& meridianProcessor;
    juce::Label titleLabel;
    juce::Label statusLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MeridianAudioProcessorEditor)
};
