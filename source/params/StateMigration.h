#pragma once
// Saved-state envelope and migration chain.
// Root: STRAT3_STATE { schema=N } -> children: APVTS tree ("STRAT3") + EXTRA (layer order,
// later: user targets, assistant log, stem-cache reference). Non-automatable state lives in
// EXTRA because hosts must never see it as parameters.
#include <juce_data_structures/juce_data_structures.h>

#include <optional>
#include <string>
#include <vector>

namespace strat3::params {

inline constexpr int kCurrentSchema = 1;
inline const juce::Identifier kStateRoot      { "STRAT3_STATE" };
inline const juce::Identifier kParamsType     { "STRAT3" };      // must equal the APVTS valueType
inline const juce::Identifier kExtraType      { "EXTRA" };
inline const juce::Identifier kSchemaProperty { "schema" };
inline const juce::Identifier kLayerOrderProp { "layerOrder" };

/** Wrap APVTS state + extra state into the versioned envelope. */
juce::ValueTree wrapState (const juce::ValueTree& apvtsState, const juce::ValueTree& extraState);

/** APVTS child of a migrated envelope, or an invalid tree if absent. */
juce::ValueTree unwrapParameters (const juce::ValueTree& envelope);

/** EXTRA child of a migrated envelope, or defaults if absent. */
juce::ValueTree unwrapExtra (const juce::ValueTree& envelope);

/** Bring a saved envelope up to kCurrentSchema.
 *  Returns nullopt when the state is unusable (wrong root, missing schema, or written by a
 *  newer build): the caller then keeps defaults instead of half-applying unknown data. */
std::optional<juce::ValueTree> migrateState (const juce::ValueTree& saved);

juce::ValueTree makeDefaultExtraState();

std::vector<std::string> defaultLayerOrder();

/** Returns `order` if it is an exact permutation of the processing layers, else the default.
 *  All-or-nothing on purpose: a partially-repaired order would silently reroute the chain. */
std::vector<std::string> sanitiseLayerOrder (const std::vector<std::string>& order);

/** Layer order stored in EXTRA, sanitised. */
std::vector<std::string> decodeLayerOrder (const juce::ValueTree& extra);

juce::ValueTree withLayerOrder (juce::ValueTree extra, const std::vector<std::string>& order);

} // namespace strat3::params
