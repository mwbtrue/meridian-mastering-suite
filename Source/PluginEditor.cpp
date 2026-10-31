#include "PluginEditor.h"
#include "PluginProcessor.h"

MeridianAudioProcessorEditor::MeridianAudioProcessorEditor (MeridianAudioProcessor& p)
    : juce::AudioProcessorEditor (p), meridianProcessor (p)
{
    titleLabel.setText ("Meridian Mastering Suite", juce::dontSendNotification);
    titleLabel.setJustificationType (juce::Justification::centred);
    titleLabel.setFont (juce::FontOptions (32.0f, juce::Font::bold));
    titleLabel.setColour (juce::Label::textColourId, juce::Colour (0xffe8e6e3));
    addAndMakeVisible (titleLabel);

    statusLabel.setJustificationType (juce::Justification::centred);
    statusLabel.setFont (juce::FontOptions (15.0f));
    statusLabel.setColour (juce::Label::textColourId, juce::Colour (0xff9aa3ad));
    addAndMakeVisible (statusLabel);

    timerCallback();
    startTimerHz (4);
    setSize (800, 600);
}

MeridianAudioProcessorEditor::~MeridianAudioProcessorEditor()
{
    stopTimer();
}

void MeridianAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff15181c));
}

void MeridianAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (40);
    titleLabel.setBounds (area.removeFromTop (area.getHeight() / 2).withTrimmedTop (area.getHeight() / 4));
    statusLabel.setBounds (area.removeFromTop (60));
}

void MeridianAudioProcessorEditor::timerCallback()
{
    const int latency = meridianProcessor.getTotalLatencySamples();
    const double sr = meridianProcessor.getCurrentSampleRate();
    const double ms = sr > 0.0 ? 1000.0 * latency / sr : 0.0;

    statusLabel.setText ("Version " + juce::String (MERIDIAN_VERSION_STRING)
                             + "   |   Latency: " + juce::String (latency) + " samples ("
                             + juce::String (ms, 2) + " ms)",
                         juce::dontSendNotification);
}
