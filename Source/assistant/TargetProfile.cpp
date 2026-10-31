#include "TargetProfile.h"

namespace meridian
{

namespace
{
    float readFloat (const juce::var& v, const char* key, float fallback)
    {
        const auto& p = v.getProperty (key, juce::var());
        return (p.isDouble() || p.isInt() || p.isInt64() || p.isBool()) ? static_cast<float> (static_cast<double> (p)) : fallback;
    }
}

TargetProfile TargetProfile::loadFromJSON (const juce::File& f)
{
    if (! f.existsAsFile())
    {
        TargetProfile p;
        p.name.clear();
        return p;
    }

    return loadFromJSONString (f.loadFileAsString());
}

TargetProfile TargetProfile::loadFromJSONString (const juce::String& json)
{
    juce::var parsed;
    if (juce::JSON::parse (json, parsed).failed() || ! parsed.isObject())
    {
        TargetProfile p;
        p.name.clear();
        return p;
    }

    return loadFromVar (parsed);
}

TargetProfile TargetProfile::loadFromVar (const juce::var& v)
{
    TargetProfile p;

    if (! v.isObject())
    {
        p.name.clear();
        return p;
    }

    p.name          = v.getProperty ("name", "").toString().trim();
    p.description   = v.getProperty ("description", "").toString();
    p.targetLUFS_I  = readFloat (v, "targetLUFS_I",  p.targetLUFS_I);
    p.targetLUFS_S  = readFloat (v, "targetLUFS_S",  p.targetLUFS_S);
    p.targetTP_dBTP = readFloat (v, "targetTP_dBTP", p.targetTP_dBTP);
    p.targetPLR_min = readFloat (v, "targetPLR_min", p.targetPLR_min);
    p.targetPLR_max = readFloat (v, "targetPLR_max", p.targetPLR_max);
    p.targetLRA_min = readFloat (v, "targetLRA_min", p.targetLRA_min);
    p.targetLRA_max = readFloat (v, "targetLRA_max", p.targetLRA_max);
    p.targetLowCorr = readFloat (v, "targetLowCorr", p.targetLowCorr);
    p.airBoost_dB   = readFloat (v, "airBoost_dB",   p.airBoost_dB);

    if (p.targetPLR_min > p.targetPLR_max) std::swap (p.targetPLR_min, p.targetPLR_max);
    if (p.targetLRA_min > p.targetLRA_max) std::swap (p.targetLRA_min, p.targetLRA_max);

    if (const auto* arr = v.getProperty ("paramSuggestions", juce::var()).getArray())
    {
        for (const auto& item : *arr)
        {
            const auto id = item.getProperty ("id", "").toString().trim();
            const auto& value = item.getProperty ("value", juce::var());
            if (id.isNotEmpty() && (value.isDouble() || value.isInt() || value.isInt64() || value.isBool()))
                p.paramSuggestions.emplace_back (id, static_cast<float> (static_cast<double> (value)));
        }
    }

    return p;
}

juce::var TargetProfile::toVar() const
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty ("name", name);
    obj->setProperty ("description", description);
    obj->setProperty ("targetLUFS_I", targetLUFS_I);
    obj->setProperty ("targetLUFS_S", targetLUFS_S);
    obj->setProperty ("targetTP_dBTP", targetTP_dBTP);
    obj->setProperty ("targetPLR_min", targetPLR_min);
    obj->setProperty ("targetPLR_max", targetPLR_max);
    obj->setProperty ("targetLRA_min", targetLRA_min);
    obj->setProperty ("targetLRA_max", targetLRA_max);
    obj->setProperty ("targetLowCorr", targetLowCorr);
    obj->setProperty ("airBoost_dB", airBoost_dB);

    juce::Array<juce::var> suggestions;
    for (const auto& [id, value] : paramSuggestions)
    {
        auto* s = new juce::DynamicObject();
        s->setProperty ("id", id);
        s->setProperty ("value", value);
        suggestions.add (juce::var (s));
    }
    obj->setProperty ("paramSuggestions", suggestions);

    return juce::var (obj);
}

bool TargetProfile::saveToJSON (const juce::File& f) const
{
    if (! f.getParentDirectory().createDirectory())
        return false;

    return f.replaceWithText (juce::JSON::toString (toVar(), false));
}

} // namespace meridian
