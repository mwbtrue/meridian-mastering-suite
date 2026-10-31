#include "PresetBrowser.h"
#include "UITheme.h"

#include "PluginProcessor.h"
#include "ParameterIDs.h"

#include <algorithm>

namespace meridian::ui
{

namespace
{
    const juce::String kPresetExtension { ".xml" };
}

//==============================================================================
// SnapshotButton: left click selects, right click opens the slot menu.
//==============================================================================
class PresetBrowser::SnapshotButton final : public juce::TextButton
{
public:
    SnapshotButton (MeridianAudioProcessor& p, int s)
        : juce::TextButton (juce::String::charToString ((juce::juce_wchar) ('A' + s))), processor (p), slot (s)
    {
        setTooltip ("Snapshot " + getButtonText() + " (right-click: store / copy / paste / clear)");
        setColour (juce::TextButton::buttonOnColourId, Colours::accent);
    }

    void clicked (const juce::ModifierKeys& mods) override
    {
        if (mods.isPopupMenu())
            return;
        processor.selectSnapshot (slot);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            showMenu();
            return;
        }
        juce::TextButton::mouseDown (e);
    }

    void refresh()
    {
        const bool isActive = processor.getActiveSnapshot() == slot;
        const bool stored = processor.hasSnapshot (slot);
        if (getToggleState() != isActive)
            setToggleState (isActive, juce::dontSendNotification);
        setAlpha (isActive || stored ? 1.0f : 0.65f);
    }

private:
    void showMenu()
    {
        juce::PopupMenu menu;
        menu.addSectionHeader ("Snapshot " + getButtonText());
        menu.addItem (1, "Store current settings here");
        menu.addItem (2, "Copy");
        menu.addItem (3, "Paste", processor.hasClipboard());
        menu.addSeparator();
        menu.addItem (4, "Clear", processor.hasSnapshot (slot) && processor.getActiveSnapshot() != slot);

        juce::Component::SafePointer<SnapshotButton> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                            [safe] (int result)
                            {
                                if (safe == nullptr)
                                    return;
                                auto& p = safe->processor;
                                switch (result)
                                {
                                    case 1: p.storeSnapshot (safe->slot); break;
                                    case 2: p.copySnapshot (safe->slot); break;
                                    case 3: p.pasteSnapshot (safe->slot); break;
                                    case 4: p.clearSnapshot (safe->slot); break;
                                    default: break;
                                }
                            });
    }

    MeridianAudioProcessor& processor;
    const int slot;
};

//==============================================================================
// ListPanel: the call-out content with the preset ListBox.
//==============================================================================
class PresetBrowser::ListPanel final : public juce::Component,
                                       private juce::ListBoxModel
{
public:
    explicit ListPanel (PresetBrowser& b) : browser (b)
    {
        list.setModel (this);
        list.setRowHeight (24);
        list.setColour (juce::ListBox::backgroundColourId, Colours::panel);
        addAndMakeVisible (list);

        loadButton.onClick   = [this] { loadSelected(); };
        deleteButton.onClick = [this] { deleteSelected(); };
        folderButton.onClick = []
        {
            auto dir = MeridianAudioProcessor::getUserPresetDirectory();
            dir.createDirectory();
            dir.revealToUser();
        };
        addAndMakeVisible (loadButton);
        addAndMakeVisible (deleteButton);
        addAndMakeVisible (folderButton);

        // Pre-select the current preset.
        const auto& entries = browser.getEntries();
        for (int i = 0; i < (int) entries.size(); ++i)
            if (! entries[(size_t) i].isHeader && entries[(size_t) i].name == browser.getCurrentPresetName())
                list.selectRow (i);

        updateButtons();
        setSize (300, 360);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (6);
        auto buttons = area.removeFromBottom (26);
        loadButton.setBounds (buttons.removeFromLeft (80));
        buttons.removeFromLeft (6);
        deleteButton.setBounds (buttons.removeFromLeft (80));
        folderButton.setBounds (buttons.removeFromRight (90));
        area.removeFromBottom (6);
        list.setBounds (area);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Colours::panel);
    }

private:
    int getNumRows() override { return (int) browser.getEntries().size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        const auto& entries = browser.getEntries();
        if (row < 0 || row >= (int) entries.size())
            return;

        const auto& e = entries[(size_t) row];
        if (e.isHeader)
        {
            g.setColour (Colours::textSecondary);
            g.setFont (makeFont (FontSize::small, true));
            g.drawText (e.name.toUpperCase(), 6, 0, width - 12, height, juce::Justification::bottomLeft, false);
            g.setColour (Colours::border);
            g.drawHorizontalLine (height - 1, 6.0f, (float) width - 6.0f);
            return;
        }

        if (selected)
        {
            g.setColour (Colours::accent.withAlpha (0.6f));
            g.fillRoundedRectangle (juce::Rectangle<float> (2.0f, 1.0f, (float) width - 4.0f, (float) height - 2.0f), Radius::small);
        }

        g.setColour (Colours::textPrimary);
        g.setFont (makeFont (FontSize::normal));
        g.drawText (e.name, 14, 0, width - 20, height, juce::Justification::centredLeft, true);

        if (e.name == browser.getCurrentPresetName())
        {
            g.setColour (Colours::accentBright);
            g.fillEllipse (4.0f, (float) height * 0.5f - 2.5f, 5.0f, 5.0f);
        }
    }

    void selectedRowsChanged (int) override { updateButtons(); }
    void listBoxItemDoubleClicked (int, const juce::MouseEvent&) override { loadSelected(); }
    void returnKeyPressed (int) override { loadSelected(); }
    void deleteKeyPressed (int) override { deleteSelected(); }

    const Entry* selectedEntry() const
    {
        const int row = list.getSelectedRow();
        const auto& entries = browser.getEntries();
        if (row < 0 || row >= (int) entries.size() || entries[(size_t) row].isHeader)
            return nullptr;
        return &entries[(size_t) row];
    }

    void updateButtons()
    {
        const auto* e = selectedEntry();
        loadButton.setEnabled (e != nullptr);
        deleteButton.setEnabled (e != nullptr && ! e->isFactory);
    }

    void loadSelected()
    {
        if (const auto* e = selectedEntry())
        {
            const Entry copy = *e;
            if (browser.loadEntry (copy))
                if (auto* box = findParentComponentOfClass<juce::CallOutBox>())
                    box->dismiss();
        }
    }

    void deleteSelected()
    {
        if (const auto* e = selectedEntry(); e != nullptr && ! e->isFactory)
        {
            const Entry copy = *e;
            browser.deletePreset (copy);
            list.updateContent();
            list.deselectAllRows();
            list.repaint();
            updateButtons();
        }
    }

    PresetBrowser& browser;
    juce::ListBox list { "Presets" };
    juce::TextButton loadButton { "Load" }, deleteButton { "Delete" }, folderButton { "Open Folder" };
};

//==============================================================================
// PresetBrowser
//==============================================================================
PresetBrowser::PresetBrowser (MeridianAudioProcessor& p)
    : processor (p)
{
    presetButton.setTooltip ("Browse presets");
    presetButton.onClick = [this] { showPresetList(); };
    addAndMakeVisible (presetButton);

    saveButton.setTooltip ("Save the current settings as a user preset");
    saveButton.onClick = [this] { showSaveDialog(); };
    addAndMakeVisible (saveButton);

    for (int s = 0; s < 4; ++s)
    {
        snapshotButtons[(size_t) s] = std::make_unique<SnapshotButton> (processor, s);
        addAndMakeVisible (*snapshotButtons[(size_t) s]);
    }

    setCurrentPresetName (currentPreset);
    refreshPresetList();
    updateSnapshotButtons();
}

PresetBrowser::~PresetBrowser() = default;

void PresetBrowser::refreshPresetList()
{
    entries.clear();

    entries.push_back ({ "Factory", {}, true, true });
    for (const auto& profile : processor.getAssistant().getProfiles())
        entries.push_back ({ profile.name, {}, true, false });

    entries.push_back ({ "User", {}, false, true });

    const auto dir = MeridianAudioProcessor::getUserPresetDirectory();
    if (dir.isDirectory())
    {
        auto files = dir.findChildFiles (juce::File::findFiles, false, "*" + kPresetExtension);
        std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b)
                   { return a.getFileNameWithoutExtension().compareNatural (b.getFileNameWithoutExtension()) < 0; });

        for (const auto& f : files)
            entries.push_back ({ f.getFileNameWithoutExtension(), f, false, false });
    }
}

bool PresetBrowser::loadPresetFile (juce::AudioProcessorValueTreeState& state, const juce::File& file)
{
    const auto xml = juce::XmlDocument::parse (file);
    if (xml == nullptr)
        return false;

    // Accept both a bare APVTS tree and the plugin's full "MeridianState" wrapper.
    juce::ValueTree tree = juce::ValueTree::fromXml (*xml);
    if (! tree.hasType (state.state.getType()))
        tree = tree.getChildWithName (state.state.getType());

    if (! tree.isValid())
        return false;

    state.replaceState (tree);
    return true;
}

bool PresetBrowser::savePresetFile (juce::AudioProcessorValueTreeState& state, const juce::File& file)
{
    const auto xml = state.copyState().createXml();
    if (xml == nullptr)
        return false;

    file.getParentDirectory().createDirectory();
    return xml->writeTo (file);
}

void PresetBrowser::applyFactoryProfile (const juce::String& profileName)
{
    auto& state = processor.getAPVTS();
    auto& assistant = processor.getAssistant();

    const TargetProfile* profile = nullptr;
    for (const auto& p : assistant.getProfiles())
        if (p.name == profileName)
            profile = &p;

    if (profile == nullptr)
        return;

    const juce::String abSlotId = pid::str (pid::abSlot);
    for (auto* param : processor.getParameters())
    {
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param))
        {
            if (ranged->getParameterID() == abSlotId)
                continue;
            ranged->beginChangeGesture();
            ranged->setValueNotifyingHost (ranged->getDefaultValue());
            ranged->endChangeGesture();
        }
    }

    for (const auto& [id, value] : profile->paramSuggestions)
        IntelligentAssistant::setParameter (state, id, value);

    IntelligentAssistant::setParameter (state, pid::str (pid::lufsTarget), profile->targetLUFS_I);

    processor.setTargetProfile (profile->name);
    assistant.setActiveProfile (profile->name);
}

bool PresetBrowser::loadEntry (const Entry& entry)
{
    if (entry.isHeader)
        return false;

    if (entry.isFactory)
    {
        applyFactoryProfile (entry.name);
    }
    else
    {
        if (! loadPresetFile (processor.getAPVTS(), entry.file))
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Load Preset",
                                                    "Could not read " + entry.file.getFullPathName());
            return false;
        }
        processor.getAssistant().setActiveProfile (processor.getTargetProfile());
    }

    setCurrentPresetName (entry.name);
    if (onPresetLoaded)
        onPresetLoaded();
    return true;
}

bool PresetBrowser::savePreset (const juce::String& name)
{
    const auto legal = juce::File::createLegalFileName (name.trim());
    if (legal.isEmpty())
        return false;

    const auto file = MeridianAudioProcessor::getUserPresetDirectory().getChildFile (legal + kPresetExtension);
    if (! savePresetFile (processor.getAPVTS(), file))
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Save Preset",
                                                "Could not write " + file.getFullPathName());
        return false;
    }

    refreshPresetList();
    setCurrentPresetName (legal);
    return true;
}

bool PresetBrowser::deletePreset (const Entry& entry)
{
    if (entry.isFactory || entry.isHeader || ! entry.file.existsAsFile())
        return false;

    const bool ok = entry.file.deleteFile();
    refreshPresetList();
    return ok;
}

void PresetBrowser::setCurrentPresetName (const juce::String& name)
{
    currentPreset = name;
    presetButton.setButtonText (name + "  " + juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xbe")));
}

void PresetBrowser::showPresetList()
{
    refreshPresetList();

    auto* top = getTopLevelComponent();
    const auto target = top->getLocalArea (this, presetButton.getBounds());
    juce::CallOutBox::launchAsynchronously (std::make_unique<ListPanel> (*this), target, top);
}

void PresetBrowser::showSaveDialog()
{
    saveDialog = std::make_unique<juce::AlertWindow> ("Save Preset", "Preset name:",
                                                      juce::MessageBoxIconType::NoIcon, this);
    saveDialog->addTextEditor ("name", currentPreset == "Init" ? juce::String ("My Master") : currentPreset);
    saveDialog->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    saveDialog->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    juce::Component::SafePointer<PresetBrowser> safe (this);
    saveDialog->enterModalState (true, juce::ModalCallbackFunction::create ([safe] (int result)
    {
        if (safe == nullptr || safe->saveDialog == nullptr)
            return;

        const auto name = safe->saveDialog->getTextEditorContents ("name");
        if (result == 1)
            safe->savePreset (name);

        // Destroy the dialog after the modal callback has fully unwound.
        juce::MessageManager::callAsync ([safe]
        {
            if (safe != nullptr)
                safe->saveDialog.reset();
        });
    }), false);
}

void PresetBrowser::updateSnapshotButtons()
{
    for (auto& b : snapshotButtons)
        b->refresh();
}

void PresetBrowser::resized()
{
    auto area = getLocalBounds();
    for (int s = 3; s >= 0; --s)
    {
        snapshotButtons[(size_t) s]->setBounds (area.removeFromRight (30).reduced (2, 0));
    }
    area.removeFromRight (8);
    saveButton.setBounds (area.removeFromRight (56).reduced (2, 0));
    area.removeFromRight (4);
    presetButton.setBounds (area);
}

} // namespace meridian::ui
