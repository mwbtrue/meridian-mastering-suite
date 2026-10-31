#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <memory>
#include <vector>

class MeridianAudioProcessor;

namespace meridian::ui
{

/**
    Preset + snapshot strip for the top bar:

        [ preset name  v ] [Save] [A] [B] [C] [D]

    - The preset button opens a call-out with a ListBox: "Factory" section
      (the five embedded target profiles) followed by "User" presets
      (*.xml in <userDocuments>/Meridian Mastering Suite/Presets).
      Double-click / Enter / Load button loads; Delete removes a user preset.
    - Save asks for a name (AlertWindow) and writes the APVTS state as XML
      (an existing preset with the same name is overwritten).
    - A/B/C/D call MeridianAudioProcessor::selectSnapshot(); right-click a
      slot for Store / Copy / Paste / Clear.

    Factory presets reset every automatable parameter to its default, apply the
    profile's parameter suggestions, set lufs_target to the profile's LUFS-I
    target and select the profile as the assistant's target.
*/
class PresetBrowser final : public juce::Component
{
public:
    struct Entry
    {
        juce::String name;
        juce::File file;          // user presets only
        bool isFactory = false;
        bool isHeader = false;
    };

    explicit PresetBrowser (MeridianAudioProcessor& processor);
    ~PresetBrowser() override;

    /** Rescans the preset directory and the factory list. */
    void refreshPresetList();
    const std::vector<Entry>& getEntries() const noexcept { return entries; }

    bool loadEntry (const Entry& entry);
    bool savePreset (const juce::String& name);
    bool deletePreset (const Entry& entry);

    static bool loadPresetFile (juce::AudioProcessorValueTreeState& state, const juce::File& file);
    static bool savePresetFile (juce::AudioProcessorValueTreeState& state, const juce::File& file);

    /** Per-frame refresh of snapshot button states (editor timer). */
    void updateSnapshotButtons();

    juce::String getCurrentPresetName() const { return currentPreset; }

    /** Called after any preset load (factory or user). */
    std::function<void()> onPresetLoaded;

    void resized() override;

private:
    class SnapshotButton;
    class ListPanel;

    void showPresetList();
    void showSaveDialog();
    void setCurrentPresetName (const juce::String& name);
    void applyFactoryProfile (const juce::String& profileName);

    MeridianAudioProcessor& processor;
    juce::TextButton presetButton, saveButton { "Save" };
    std::array<std::unique_ptr<SnapshotButton>, 4> snapshotButtons;
    std::vector<Entry> entries;
    std::unique_ptr<juce::AlertWindow> saveDialog;
    juce::String currentPreset { "Init" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetBrowser)
};

} // namespace meridian::ui
