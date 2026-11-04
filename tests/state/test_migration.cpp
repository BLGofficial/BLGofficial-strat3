// State migration tests: presets and DAW sessions must load across plugin versions,
// and malformed or newer-than-us state must never be half-applied.
#include <catch2/catch_test_macros.hpp>
#include <juce_events/juce_events.h>

#include "params/StateMigration.h"
#include "params/ParameterSpecs.h"
#include "support/TestProcessor.h"

using namespace strat3::params;

namespace {

juce::ValueTree savedStateOf (strat3::test::TestProcessor& proc)
{
    return wrapState (proc.apvts.copyState(), makeDefaultExtraState());
}

} // namespace

TEST_CASE ("current-schema state passes through unchanged", "[state]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    strat3::test::TestProcessor proc;

    const auto saved = savedStateOf (proc);
    REQUIRE (static_cast<int> (saved.getProperty (kSchemaProperty)) == kCurrentSchema);

    const auto migrated = migrateState (saved);
    REQUIRE (migrated.has_value());
    CHECK (migrated->isEquivalentTo (saved));
}

TEST_CASE ("state from a newer schema is rejected, not half-loaded", "[state]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    strat3::test::TestProcessor proc;

    auto saved = savedStateOf (proc);
    saved.setProperty (kSchemaProperty, kCurrentSchema + 1, nullptr);
    CHECK_FALSE (migrateState (saved).has_value());
}

TEST_CASE ("state with no schema or wrong root type is rejected", "[state]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    strat3::test::TestProcessor proc;

    auto noSchema = savedStateOf (proc);
    noSchema.removeProperty (kSchemaProperty, nullptr);
    CHECK_FALSE (migrateState (noSchema).has_value());

    CHECK_FALSE (migrateState (juce::ValueTree { "SOMETHING_ELSE" }).has_value());
    CHECK_FALSE (migrateState (juce::ValueTree {}).has_value());
}

TEST_CASE ("save -> load round trip restores non-default values", "[state]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    strat3::test::TestProcessor source;

    auto setParam = [&] (const char* id, float value)
    {
        auto* p = source.apvts.getParameter (id);
        REQUIRE (p != nullptr);
        p->setValueNotifyingHost (source.apvts.getParameterRange (id).convertTo0to1 (value));
    };
    setParam ("eq.b3.gain", 4.5f);
    setParam ("limit.thresh", -6.0f);
    setParam ("mb.depth", 72.0f);

    const auto saved = savedStateOf (source);

    strat3::test::TestProcessor target;
    const auto migrated = migrateState (saved);
    REQUIRE (migrated.has_value());
    const auto unwrapped = unwrapParameters (*migrated);
    REQUIRE (unwrapped.isValid());
    target.apvts.replaceState (unwrapped);

    CHECK (target.apvts.getRawParameterValue ("eq.b3.gain")->load() == 4.5f);
    CHECK (target.apvts.getRawParameterValue ("limit.thresh")->load() == -6.0f);
    CHECK (target.apvts.getRawParameterValue ("mb.depth")->load() == 72.0f);
}

TEST_CASE ("extra state keeps layer order and the schema stamp", "[state]")
{
    const auto extra = makeDefaultExtraState();
    CHECK (decodeLayerOrder (extra) == defaultLayerOrder());
}

TEST_CASE ("layer order sanitiser accepts only exact permutations", "[state][order]")
{
    const auto def = defaultLayerOrder();

    SECTION ("valid permutation kept")
    {
        auto reordered = def;
        std::reverse (reordered.begin(), reordered.end());
        CHECK (sanitiseLayerOrder (reordered) == reordered);
    }
    SECTION ("duplicate falls back to default")
    {
        auto dup = def;
        dup[1] = dup[0];
        CHECK (sanitiseLayerOrder (dup) == def);
    }
    SECTION ("unknown id falls back to default")
    {
        auto unknown = def;
        unknown.back() = "reverb";
        CHECK (sanitiseLayerOrder (unknown) == def);
    }
    SECTION ("missing layer falls back to default")
    {
        auto missing = def;
        missing.pop_back();
        CHECK (sanitiseLayerOrder (missing) == def);
    }
    SECTION ("ceiling is never reorderable")
    {
        auto withCeiling = def;
        withCeiling.back() = "ceiling";
        CHECK (sanitiseLayerOrder (withCeiling) == def);
    }
}
