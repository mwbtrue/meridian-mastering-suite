#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "assistant/IntelligentAssistant.h"

#include <functional>
#include <memory>
#include <vector>

class MeridianAudioProcessor;

namespace meridian::ui
{

/** One proposal line: module | parameter | current -> proposed | reason | Accept. */
class ProposalRow final : public juce::Component
{
public:
    ProposalRow (int index, const IntelligentAssistant::ParameterProposal& proposal,
                 juce::AudioProcessorValueTreeState& state);

    void setAccepted (bool accepted);
    std::function<void (int index, bool accepted)> onToggle;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Module name for a parameter ID (by prefix). */
    static juce::String moduleForParameter (const juce::String& paramId);
    /** Parameter value formatted with the parameter's own text function (real units in). */
    static juce::String formatValue (juce::AudioProcessorValueTreeState& state, const juce::String& id, float value);

private:
    const int index;
    juce::String module, param, current, proposed, reason;
    juce::ToggleButton accept { "Accept" };
};

/**
    Intelligent Assistant panel (centre-bottom, collapsible).

    Header: title, target-profile combo, Analyze (captures the last 20 s of
            processed output from the processor's capture ring; spinner while
            the background analysis runs), GR-budget warning, collapse button.
    Body:   analysis stats | proposal list (Accept per row, Accept All /
            Reject All) | QC tools: codec round-trip preview (AAC 256 /
            MP3 320 / Opus 160) and reference track load + level-matched A/B.
*/
class AssistantPanel final : public juce::Component
{
public:
    static constexpr int kHeaderHeight = 32;

    explicit AssistantPanel (MeridianAudioProcessor& processor);
    ~AssistantPanel() override;

    /** Editor timer: spinner, analysis completion, codec / reference status, GR warning. */
    void update();

    /** Re-reads the target profile from the processor state (after preset / state load). */
    void syncProfileFromProcessor();

    bool isCollapsed() const noexcept { return collapsed; }
    std::function<void()> onCollapseToggled;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void startAnalysis();
    void rebuildProposalRows();
    void refreshAcceptedStates();
    void updateStats();
    void startCodecPreview();
    void chooseReference();

    MeridianAudioProcessor& processor;
    IntelligentAssistant& assistant;

    juce::Label title, statusLabel, grWarning;
    juce::ComboBox profileBox;
    juce::TextButton analyzeButton { "Analyze" }, collapseButton;
    juce::TextButton acceptAllButton { "Accept All" }, rejectAllButton { "Reject All" };

    juce::Label statsLabel;
    juce::Viewport proposalViewport;
    juce::Component proposalList;
    std::vector<std::unique_ptr<ProposalRow>> proposalRows;
    juce::Label emptyLabel;

    juce::Label codecTitle, codecResult;
    juce::ComboBox codecBox;
    juce::TextButton codecButton { "Preview" };

    juce::Label refTitle, refStatus;
    juce::TextButton refLoadButton { "Load..." }, refABButton { "A/B Ref" };
    std::unique_ptr<juce::FileChooser> fileChooser;

    juce::Rectangle<int> spinnerArea, statsArea, proposalsArea, qcArea;

    bool collapsed = false;
    bool wasAnalyzing = false;
    bool wasCodecRunning = false;
    float spinnerAngle = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AssistantPanel)
};

} // namespace meridian::ui
