#include "params/StateMigration.h"
#include "params/ParameterSpecs.h"

#include <algorithm>
#include <array>
#include <functional>

namespace strat3::params {
namespace {

using Migration = std::function<juce::ValueTree (juce::ValueTree)>;

// migrations[n] upgrades schema n+1 -> n+2. Empty at schema 1; append, never edit, entries.
const std::array<Migration, kCurrentSchema - 1>& migrations()
{
    static const std::array<Migration, kCurrentSchema - 1> steps {};
    return steps;
}

std::string joinOrder (const std::vector<std::string>& order)
{
    std::string out;
    for (size_t i = 0; i < order.size(); ++i)
        out += (i ? "," : "") + order[i];
    return out;
}

std::vector<std::string> splitOrder (const juce::String& text)
{
    std::vector<std::string> out;
    for (const auto& token : juce::StringArray::fromTokens (text, ",", {}))
        if (token.isNotEmpty())
            out.push_back (token.trim().toStdString());
    return out;
}

} // namespace

juce::ValueTree wrapState (const juce::ValueTree& apvtsState, const juce::ValueTree& extraState)
{
    juce::ValueTree root { kStateRoot };
    root.setProperty (kSchemaProperty, kCurrentSchema, nullptr);
    root.appendChild (apvtsState.createCopy(), nullptr);
    root.appendChild (extraState.createCopy(), nullptr);
    return root;
}

juce::ValueTree unwrapParameters (const juce::ValueTree& envelope)
{
    return envelope.getChildWithName (kParamsType);
}

juce::ValueTree unwrapExtra (const juce::ValueTree& envelope)
{
    const auto extra = envelope.getChildWithName (kExtraType);
    return extra.isValid() ? extra : makeDefaultExtraState();
}

std::optional<juce::ValueTree> migrateState (const juce::ValueTree& saved)
{
    if (! saved.isValid() || saved.getType() != kStateRoot || ! saved.hasProperty (kSchemaProperty))
        return std::nullopt;

    const int schema = static_cast<int> (saved.getProperty (kSchemaProperty));
    if (schema < 1 || schema > kCurrentSchema)
        return std::nullopt;   // newer build wrote this: refusing beats misreading it

    auto tree = saved.createCopy();
    for (int from = schema; from < kCurrentSchema; ++from)
    {
        tree = migrations()[static_cast<size_t> (from - 1)] (std::move (tree));
        tree.setProperty (kSchemaProperty, from + 1, nullptr);
    }
    return tree;
}

juce::ValueTree makeDefaultExtraState()
{
    return withLayerOrder (juce::ValueTree { kExtraType }, defaultLayerOrder());
}

std::vector<std::string> defaultLayerOrder()
{
    return processingLayerIds();
}

std::vector<std::string> sanitiseLayerOrder (const std::vector<std::string>& order)
{
    auto expected = defaultLayerOrder();
    auto candidate = order;
    std::sort (expected.begin(), expected.end());
    std::sort (candidate.begin(), candidate.end());
    return candidate == expected ? order : defaultLayerOrder();
}

std::vector<std::string> decodeLayerOrder (const juce::ValueTree& extra)
{
    return sanitiseLayerOrder (splitOrder (extra.getProperty (kLayerOrderProp).toString()));
}

juce::ValueTree withLayerOrder (juce::ValueTree extra, const std::vector<std::string>& order)
{
    extra.setProperty (kLayerOrderProp, juce::String (joinOrder (sanitiseLayerOrder (order))), nullptr);
    return extra;
}

} // namespace strat3::params
