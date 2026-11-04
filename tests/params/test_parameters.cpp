// Parameter contract tests. These guard host compatibility: a renamed ID, a reordered
// choice list or a changed range silently breaks saved sessions and automation lanes.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <juce_events/juce_events.h>

#include <fstream>
#include <regex>
#include <set>
#include <sstream>

#include "params/ParameterSpecs.h"
#include "params/ParameterLayout.h"
#include "support/TestProcessor.h"

using namespace strat3::params;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

std::vector<std::string> readLines (const std::string& path)
{
    std::ifstream in (path);
    std::vector<std::string> lines;
    for (std::string line; std::getline (in, line);)
        if (! line.empty() && line.front() != '#')
            lines.push_back (line);
    return lines;
}

const ParamSpec* findSpec (std::string_view id)
{
    for (const auto& s : allSpecs())
        if (s.id == id)
            return &s;
    return nullptr;
}

} // namespace

TEST_CASE ("param IDs are unique and well-formed", "[params]")
{
    const std::regex pattern { R"(^[a-z][a-z0-9]*(\.[a-z][a-zA-Z0-9]*)+$)" };
    std::set<std::string> seen;

    for (const auto& s : allSpecs())
    {
        INFO (s.id);
        CHECK (std::regex_match (s.id, pattern));
        CHECK (seen.insert (s.id).second);
        CHECK (s.versionHint >= 1);
        CHECK_FALSE (s.name.empty());
    }
}

TEST_CASE ("param specs are internally consistent", "[params]")
{
    for (const auto& s : allSpecs())
    {
        INFO (s.id);
        CHECK (s.min < s.max);
        CHECK (s.def >= s.min);
        CHECK (s.def <= s.max);

        if (s.taper == Taper::Log)
            CHECK (s.min > 0.0f); // log mapping is undefined at or below zero

        if (s.kind == Kind::Choice)
        {
            REQUIRE (s.choices.size() >= 2);
            CHECK (s.min == 0.0f);
            CHECK (s.max == static_cast<float> (s.choices.size() - 1));
        }
        else
        {
            CHECK (s.choices.empty());
        }

        if (s.kind == Kind::Bool)
            CHECK ((s.def == 0.0f || s.def == 1.0f));
    }
}

TEST_CASE ("every processing layer has on + depth + delta; ceiling has no depth", "[params]")
{
    for (const auto& layer : processingLayerIds())
    {
        INFO (layer);
        const auto* on    = findSpec (layer + ".on");
        const auto* depth = findSpec (layer + ".depth");
        const auto* delta = findSpec (layer + ".delta");
        REQUIRE (on != nullptr);
        REQUIRE (depth != nullptr);
        REQUIRE (delta != nullptr);
        CHECK (on->def == 0.0f);   // default-off: a fresh instance must null against its input
        CHECK (depth->min == 0.0f);
        CHECK (depth->max == 150.0f);
        CHECK (depth->def == 100.0f);
    }

    CHECK (findSpec ("ceiling.depth") == nullptr);
    CHECK (findSpec ("ceiling.on") != nullptr);
}

TEST_CASE ("only monitoring and latency-changing params are non-automatable", "[params]")
{
    for (const auto& s : allSpecs())
    {
        INFO (s.id);
        const bool isDelta   = s.id.ends_with (".delta");
        const bool isLatency = s.id == "global.latencyMode";
        const bool isMonitor = s.id == "global.gainMatch" || s.id == "global.delta";
        CHECK (s.automatable == ! (isDelta || isLatency || isMonitor));
    }
}

TEST_CASE ("frozen IDs: every golden entry still exists with identical shape", "[params][golden]")
{
    const std::string goldenPath    = std::string (STRAT3_GOLDEN_DIR) + "/param_ids_v1.txt";
    const std::string candidatePath = std::string (STRAT3_CANDIDATE_DIR) + "/param_ids_v1.candidate.txt";

    // Always emit the current shape so a reviewer can diff it against the golden file.
    {
        std::ofstream out (candidatePath);
        out << "# id|kind|min|max|taper|choices  (append-only after first release)\n";
        for (const auto& s : allSpecs())
            out << goldenLine (s) << '\n';
    }

    const auto golden = readLines (goldenPath);
    INFO ("golden file: " << goldenPath << "  candidate: " << candidatePath);
    REQUIRE_FALSE (golden.empty());

    std::set<std::string> current;
    for (const auto& s : allSpecs())
        current.insert (goldenLine (s));

    for (const auto& line : golden)
    {
        INFO (line);
        CHECK (current.count (line) == 1);
    }
}

TEST_CASE ("APVTS registers every spec with the spec's range and default", "[params][apvts]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    strat3::test::TestProcessor proc;

    CHECK (static_cast<size_t> (proc.getParameters().size()) == allSpecs().size());

    for (const auto& s : allSpecs())
    {
        INFO (s.id);
        auto* p = proc.apvts.getParameter (s.id);
        REQUIRE (p != nullptr);
        CHECK (p->isAutomatable() == s.automatable);

        const auto& range = proc.apvts.getParameterRange (s.id);
        CHECK_THAT (range.start, WithinAbs (s.min, 1e-6));
        CHECK_THAT (range.end,   WithinAbs (s.max, 1e-6));
        CHECK_THAT (range.convertFrom0to1 (p->getDefaultValue()), WithinAbs (s.def, 1e-3 * static_cast<double> (s.max - s.min)));

        if (auto* c = dynamic_cast<juce::AudioParameterChoice*> (p))
            CHECK (static_cast<size_t> (c->choices.size()) == s.choices.size());
    }
}

TEST_CASE ("log taper puts the geometric mean at the centre of travel", "[params][taper]")
{
    for (const auto& s : allSpecs())
    {
        if (s.taper != Taper::Log)
            continue;

        INFO (s.id);
        const auto range = makeRange (s);
        const float geoMean = std::sqrt (s.min * s.max);
        CHECK_THAT (range.convertTo0to1 (geoMean), WithinAbs (0.5, 1e-4));
        CHECK_THAT (range.convertFrom0to1 (0.0f), WithinRel (s.min, 1e-5f));
        CHECK_THAT (range.convertFrom0to1 (1.0f), WithinRel (s.max, 1e-5f));
    }
}
