#pragma once

#include <juce_core/juce_core.h>

#include <utility>
#include <vector>

namespace meridian
{

/**
    Mastering target profile: loudness / dynamics targets plus per-parameter
    suggestions (APVTS parameter ID -> value in the parameter's real units;
    for choice parameters either the choice index or the physical value shown
    by the choice, e.g. 60 for "60 Hz" - see IntelligentAssistant).

    JSON schema (all numeric fields optional; missing fields keep the defaults below):
    {
      "name": "...", "description": "...",
      "targetLUFS_I": -9, "targetLUFS_S": -8, "targetTP_dBTP": -1,
      "targetPLR_min": 8, "targetPLR_max": 10, "targetLRA_min": 4, "targetLRA_max": 7,
      "targetLowCorr": 0.9, "airBoost_dB": 1.5,
      "paramSuggestions": [ { "id": "lim_ceiling", "value": -1.0 }, ... ]
    }
*/
struct TargetProfile
{
    juce::String name { "Untitled" };
    juce::String description;
    float targetLUFS_I  = -14.0f;
    float targetLUFS_S  = -12.0f;
    float targetTP_dBTP = -1.0f;
    float targetPLR_min = 8.0f,  targetPLR_max = 12.0f;
    float targetLRA_min = 5.0f,  targetLRA_max = 9.0f;
    float targetLowCorr = 0.9f;
    float airBoost_dB   = 0.0f;
    std::vector<std::pair<juce::String, float>> paramSuggestions;

    /** Returns a default-constructed profile with an empty name if the file cannot be parsed. */
    static TargetProfile loadFromJSON (const juce::File& f);
    static TargetProfile loadFromJSONString (const juce::String& json);
    static TargetProfile loadFromVar (const juce::var& v);

    juce::var toVar() const;
    bool saveToJSON (const juce::File& f) const;

    bool isValid() const noexcept { return name.isNotEmpty(); }
};

} // namespace meridian
