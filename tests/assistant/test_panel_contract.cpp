// Contract between the session protocol (C++) and the WebView panels (webui/, TypeScript).
// Both test suites read the same files under webui/contract/:
//   states/<name>.json   what stateToVar sends in each situation the panels draw. Recorded
//                        from the scenarios in tests/support/PanelScenarios.h; the page's
//                        Vitest suite renders from these files.
//   commands.json        every command the page's builders produce. The Vitest suite checks
//                        the builders against it; here the real parseCommand reads it.
// Contracts tested here:
//   - the committed state files are what the protocol sends today (numbers within 1e-4, so
//     a different libm does not fail the build; text and structure exact)
//   - there is no committed state file without a scenario behind it
//   - the scenarios cover every status, result and capture kind the protocol can send
//   - every page command parses, and means what the page meant
// To re-record after a protocol change: run this test, then copy
//   <build>/panel-contract/states/*.json  over  webui/contract/states/
// and run the page's tests (npm test in webui/).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <set>
#include <string>

#include "support/JsonDiff.h"
#include "support/PanelScenarios.h"

using namespace strat3::assistant;
using Catch::Approx;
using strat3::test::difference;
using strat3::test::isNumber;

namespace {

juce::File contractDir() { return juce::File (STRAT3_PANEL_CONTRACT_DIR); }

const char* name (CommandType t)
{
    switch (t)
    {
        case CommandType::Analyse:          return "analyse";
        case CommandType::CaptureReference: return "captureReference";
        case CommandType::Cancel:           return "cancel";
        case CommandType::Accept:           return "accept";
        case CommandType::Reject:           return "reject";
        case CommandType::Depth:            return "depth";
        case CommandType::ApplyAll:         return "applyAll";
        case CommandType::ClearTarget:      return "clearTarget";
    }
    return "";
}

} // namespace

TEST_CASE ("panel contract: the committed state files are what the protocol sends today", "[assistant][contract]")
{
    const auto candidates = juce::File (STRAT3_CANDIDATE_DIR).getChildFile ("panel-contract").getChildFile ("states");
    candidates.deleteRecursively();
    REQUIRE (candidates.createDirectory().wasOk());

    // Every candidate is written before anything is compared, so a reviewer can diff them
    // and a re-record is a plain copy even when the first comparison fails.
    const auto all = strat3::test::panel::scenarios();
    for (const auto& scenario : all)
        REQUIRE (candidates.getChildFile (scenario.name + ".json").replaceWithText (juce::JSON::toString (scenario.state) + "\n", false, false, "\n"));

    std::set<std::string> names;
    for (const auto& scenario : all)
    {
        names.insert (scenario.name);
        const auto committed = contractDir().getChildFile ("states").getChildFile (scenario.name + ".json");
        INFO ("scenario: " << scenario.name << "  candidate: " << candidates.getFullPathName());
        CHECK (committed.existsAsFile());
        juce::var parsed;
        if (juce::JSON::parse (committed.loadFileAsString(), parsed).failed())
        {
            FAIL_CHECK ("the committed file is missing or not JSON");
            continue;
        }
        CHECK (difference (parsed, scenario.state, "state") == "");
    }

    // A file nothing generates would keep the page's tests green against a retired shape.
    for (const auto& file : contractDir().getChildFile ("states").findChildFiles (juce::File::findFiles, false, "*.json"))
    {
        INFO ("committed file with no scenario: " << file.getFileName());
        CHECK (names.count (file.getFileNameWithoutExtension().toStdString()) == 1);
    }
}

TEST_CASE ("panel contract: the scenarios cover everything the protocol can send", "[assistant][contract]")
{
    std::set<std::string> kinds, results, statuses, layers;
    bool paused = false, locked = false, dynamicDip = false, partDepth = false, noTargetWithMoves = false;
    std::set<int> referenceCounts;
    for (const auto& scenario : strat3::test::panel::scenarios())
    {
        const auto& s = scenario.state;
        kinds.insert (s["capture"]["kind"].toString().toStdString());
        results.insert (s["result"].toString().toStdString());
        paused = paused || static_cast<bool> (s["capture"]["paused"]);
        referenceCounts.insert (static_cast<int> (s["target"]["references"]));
        CHECK (s["target"]["levelDb"].size() == (static_cast<bool> (s["target"]["captured"]) ? kBands : 0));
        CHECK (s["target"]["bandHz"].size() == s["target"]["levelDb"].size());
        noTargetWithMoves = noTargetWithMoves || (! static_cast<bool> (s["target"]["captured"]) && s["moves"].size() > 0);
        for (int i = 0; i < s["moves"].size(); ++i)
        {
            const auto& m = s["moves"][i];
            statuses.insert (m["status"].toString().toStdString());
            layers.insert (m["layer"].toString().toStdString());
            locked = locked || static_cast<bool> (m["locked"]);
            dynamicDip = dynamicDip || m["reason"].toString().startsWith ("dynamic dip");
            partDepth = partDepth || (static_cast<double> (m["depth"]) > 0.0 && static_cast<double> (m["depth"]) < 1.0);
            CHECK (m["changes"].size() > 0);
        }
    }
    CHECK (kinds == std::set<std::string> { "none", "analyse", "reference" });
    CHECK (results == std::set<std::string> { "none", "moves", "noMoves", "referenceAdded", "silent", "cancelled" });
    CHECK (statuses == std::set<std::string> { "pending", "accepted", "rejected" });
    CHECK (layers == std::set<std::string> { "eq", "low", "limit" });
    CHECK (paused);
    CHECK (locked);
    CHECK (dynamicDip);
    CHECK (partDepth);
    CHECK (noTargetWithMoves);
    CHECK (referenceCounts.count (0) == 1);
    CHECK (referenceCounts.count (kMaxReferences) == 1);
}

TEST_CASE ("panel contract: every command the page sends parses and means what the page meant", "[assistant][contract]")
{
    const auto file = contractDir().getChildFile ("commands.json");
    REQUIRE (file.existsAsFile());
    juce::var list;
    REQUIRE (juce::JSON::parse (file.loadFileAsString(), list).wasOk());
    REQUIRE (list.isArray());

    std::set<CommandType> seen;
    for (int i = 0; i < list.size(); ++i)
    {
        const auto& entry = list[i];
        const auto& sent = entry["command"];
        INFO ("call: " << entry["call"].toString());
        const auto parsed = parseCommand (sent);
        REQUIRE (parsed.has_value());
        seen.insert (parsed->type);

        // The page's intent is the command itself: its type and every field it carries.
        CHECK (sent["type"].toString() == name (parsed->type));
        if (sent.hasProperty ("id"))
            CHECK (parsed->id == static_cast<int> (sent["id"]));
        CHECK (parsed->hasDepth == sent.hasProperty ("depth"));
        if (sent.hasProperty ("depth"))
            CHECK (static_cast<double> (parsed->depth) == Approx (static_cast<double> (sent["depth"])));
        if (sent.hasProperty ("seconds"))
            CHECK (parsed->seconds == static_cast<double> (sent["seconds"]));
    }
    CHECK (seen.size() == 8);   // the file exercises every command type
}
