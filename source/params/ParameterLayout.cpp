#include "params/ParameterLayout.h"

#include <cmath>

namespace strat3::params {
namespace {

std::unique_ptr<juce::RangedAudioParameter> makeParameter (const ParamSpec& s)
{
    const juce::ParameterID pid { juce::String (s.id), s.versionHint };
    const juce::String name { s.name };

    switch (s.kind)
    {
        case Kind::Bool:
            return std::make_unique<juce::AudioParameterBool> (
                pid, name, s.def >= 0.5f,
                juce::AudioParameterBoolAttributes().withAutomatable (s.automatable));

        case Kind::Choice:
        {
            juce::StringArray options;
            for (const auto& c : s.choices)
                options.add (juce::String (c));
            return std::make_unique<juce::AudioParameterChoice> (
                pid, name, options, static_cast<int> (s.def),
                juce::AudioParameterChoiceAttributes().withAutomatable (s.automatable));
        }

        case Kind::Float:
            return std::make_unique<juce::AudioParameterFloat> (
                pid, name, makeRange (s), s.def,
                juce::AudioParameterFloatAttributes()
                    .withLabel (juce::String (s.unit))
                    .withAutomatable (s.automatable));
    }

    jassertfalse;
    return {};
}

} // namespace

juce::NormalisableRange<float> makeRange (const ParamSpec& s)
{
    if (s.kind != Kind::Float || s.taper == Taper::Linear)
        return { s.min, s.max };

    // Captureless lambdas: NormalisableRange passes start/end in, so the range stays copyable
    // and identical for every instance.
    auto from0to1 = [] (float start, float end, float x) { return start * std::pow (end / start, x); };
    auto to0to1   = [] (float start, float end, float v) { return std::log (v / start) / std::log (end / start); };
    auto snap     = [] (float start, float end, float v) { return juce::jlimit (start, end, v); };

    return { s.min, s.max, from0to1, to0to1, snap };
}

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    for (const auto& s : allSpecs())
        layout.add (makeParameter (s));
    return layout;
}

} // namespace strat3::params
