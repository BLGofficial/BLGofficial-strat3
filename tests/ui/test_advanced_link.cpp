// The advanced view's native side (module 21d), without a browser. Contracts tested here:
//   - every relay names a real parameter, of the kind its relay needs (a slider for a float,
//     a toggle for a bool, a combo box for a choice), and none twice
//   - the relay contract the page reads carries each parameter's range, unit, taper or
//     choices from the parameter specs
//   - the info says what the engine runs with, not what the parameters ask for: the
//     prepared topology, the crossover points after sorting and spacing (with the points as
//     set, from the same moment), the oversampling
//     it was prepared with and what runs after the rate cap, the sidechain channels the host connected; nothing while the
//     processor is not prepared
//   - the committed contract file (webui/contract/advanced.json) is what this code writes
// To re-record after a change: run this test, then copy
//   <build>/panel-contract/advanced.json  over  webui/contract/advanced.json
// and run the page's tests (npm test in webui/).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <set>
#include <string>

#include "processor/Strat3Processor.h"
#include "support/JsonDiff.h"
#include "ui/web/AdvancedLink.h"

using namespace strat3;
using processor::Strat3Processor;
using Catch::Approx;
namespace web = strat3::ui::web;

namespace {

struct JuceScope
{
    juce::ScopedJuceInitialiser_GUI init;
};

using Set = juce::AudioChannelSet;

std::unique_ptr<Strat3Processor> makeProc (double fs = 48000.0, Set sidechain = Set::disabled())
{
    auto p = std::make_unique<Strat3Processor> (false);
    juce::AudioProcessor::BusesLayout l;
    l.inputBuses.add (Set::stereo());
    l.inputBuses.add (sidechain);
    l.outputBuses.add (Set::stereo());
    REQUIRE (p->setBusesLayout (l));
    p->setRateAndBufferSizeDetails (fs, 512);
    p->prepareToPlay (fs, 512);
    return p;
}

void setParam (Strat3Processor& p, const char* id, float value)
{
    auto* param = p.apvts.getParameter (id);
    REQUIRE (param != nullptr);
    param->setValueNotifyingHost (param->convertTo0to1 (value));
}

double num (const juce::var& v) { return static_cast<double> (v); }

/** The situations the page draws, each from a real processor. */
juce::var infoScenarios()
{
    auto* o = new juce::DynamicObject;
    {
        auto p = makeProc();
        o->setProperty ("default", web::advancedInfo (*p));
    }
    {
        // MASTER (FIR), 4x asked at 96 kHz (capped), crossover points out of order.
        auto p = makeProc (96000.0);
        setParam (*p, "global.latencyMode", 2.0f);
        setParam (*p, "sat.os", 2.0f);
        setParam (*p, "xover.f1", 450.0f);
        setParam (*p, "xover.f2", 300.0f);
        p->applyPendingConfig();
        o->setProperty ("master-96k", web::advancedInfo (*p));
    }
    {
        auto p = makeProc (44100.0, Set::stereo());
        setParam (*p, "mb.scSource", 1.0f);
        o->setProperty ("sidechain-connected", web::advancedInfo (*p));
    }
    {
        Strat3Processor p (false);
        o->setProperty ("not-prepared", web::advancedInfo (p));
    }
    return o;
}

} // namespace

TEST_CASE ("advanced link: every relay is a real parameter of the kind its relay needs", "[ui][web][advanced]")
{
    const JuceScope juce;
    Strat3Processor p (false);
    std::set<std::string> ids;
    for (const auto& r : web::kAdvancedRelays)
    {
        INFO ("relay: " << r.id);
        CHECK (ids.insert (r.id).second);
        auto* param = p.apvts.getParameter (r.id);
        REQUIRE (param != nullptr);
        switch (r.kind)
        {
            case web::RelayKind::Slider:   CHECK (dynamic_cast<juce::AudioParameterFloat*> (param) != nullptr); break;
            case web::RelayKind::Toggle:   CHECK (dynamic_cast<juce::AudioParameterBool*> (param) != nullptr); break;
            case web::RelayKind::ComboBox: CHECK (dynamic_cast<juce::AudioParameterChoice*> (param) != nullptr); break;
        }
    }
    CHECK (std::string (web::relayKindName (web::RelayKind::Slider)) == "slider");
    CHECK (std::string (web::relayKindName (web::RelayKind::Toggle)) == "toggle");
    CHECK (std::string (web::relayKindName (web::RelayKind::ComboBox)) == "comboBox");
}

TEST_CASE ("advanced link: the relay contract carries ranges, units, tapers and choices", "[ui][web][advanced]")
{
    const auto c = web::relayContract();
    REQUIRE (c.isArray());
    REQUIRE (c.size() == static_cast<int> (web::kAdvancedRelays.size()));
    for (int i = 0; i < c.size(); ++i)
        CHECK (c[i]["id"].toString() == web::kAdvancedRelays[static_cast<std::size_t> (i)].id);

    const auto f1 = c[0];
    CHECK (f1["kind"].toString() == "slider");
    CHECK (f1["name"].toString() == "Crossover 1");
    CHECK (num (f1["min"]) == 40.0);
    CHECK (num (f1["max"]) == 500.0);
    CHECK (num (f1["default"]) == 120.0);
    CHECK (f1["unit"].toString() == "Hz");
    CHECK (f1["taper"].toString() == "log");
    CHECK_FALSE (f1.hasProperty ("choices"));

    const auto os = c[3];
    CHECK (os["kind"].toString() == "comboBox");
    REQUIRE (os["choices"].size() == 3);
    CHECK (os["choices"][0].toString() == "1x");
    CHECK (os["choices"][2].toString() == "4x");
    CHECK (num (os["default"]) == 1.0);   // the index of 2x
    CHECK_FALSE (os.hasProperty ("taper"));

    // Every relayed slider is log today; a linear one must still say so.
    const auto trim = web::relayEntry ({ "global.inTrim", web::RelayKind::Slider });
    CHECK (trim["taper"].toString() == "lin");
    CHECK (num (trim["min"]) == -24.0);
    CHECK (trim["unit"].toString() == "dB");

    const auto hp = c[7];
    CHECK (hp["id"].toString() == "mb.scHpOn");
    CHECK (hp["kind"].toString() == "toggle");
    CHECK (num (hp["default"]) == 0.0);
    CHECK_FALSE (hp.hasProperty ("choices"));
    CHECK_FALSE (hp.hasProperty ("taper"));
}

TEST_CASE ("advanced link: the info is what the engine runs with", "[ui][web][advanced]")
{
    const JuceScope juce;
    auto p = makeProc();
    auto info = web::advancedInfo (*p);
    CHECK (info["topology"].toString() == "iir");   // MIX
    CHECK (num (info["sampleRate"]) == 48000.0);
    REQUIRE (info["crossovers"].size() == 3);
    CHECK (num (info["crossovers"][0]) == Approx (120.0).margin (0.01));
    CHECK (num (info["crossovers"][1]) == Approx (1000.0).margin (0.01));
    CHECK (num (info["crossovers"][2]) == Approx (5000.0).margin (0.01));
    CHECK (num (info["oversampling"]["requested"]) == 2.0);
    CHECK (num (info["oversampling"]["active"]) == 2.0);
    CHECK (num (info["sidechain"]["channels"]) == 0.0);

    // Asked for, not yet running: the topology follows the prepare, not the parameter.
    setParam (*p, "global.latencyMode", 2.0f);
    CHECK (web::advancedInfo (*p)["topology"].toString() == "iir");
    p->applyPendingConfig();
    CHECK (web::advancedInfo (*p)["topology"].toString() == "fir");

    // Out of order and too close: the engine sorts them and keeps them 1.25x apart.
    setParam (*p, "xover.f1", 450.0f);
    setParam (*p, "xover.f2", 400.0f);
    info = web::advancedInfo (*p);
    CHECK (num (info["crossovers"][0]) == Approx (400.0).margin (0.01));
    CHECK (num (info["crossovers"][1]) == Approx (500.0).margin (0.01));
    CHECK (num (info["crossovers"][2]) == Approx (5000.0).margin (0.01));
    // With the points as set, from the same moment: the page tells the two apart without
    // comparing its own, newer slider values against an older info (a drag would flicker).
    REQUIRE (info["crossoversSet"].size() == 3);
    CHECK (num (info["crossoversSet"][0]) == Approx (450.0).margin (0.01));
    CHECK (num (info["crossoversSet"][1]) == Approx (400.0).margin (0.01));
    CHECK (num (info["crossoversSet"][2]) == Approx (5000.0).margin (0.01));

    // 4x at 96 kHz is over the CPU cap: 2x runs.
    auto hi = makeProc (96000.0);
    setParam (*hi, "sat.os", 2.0f);
    hi->applyPendingConfig();
    info = web::advancedInfo (*hi);
    CHECK (num (info["oversampling"]["requested"]) == 4.0);
    CHECK (num (info["oversampling"]["active"]) == 2.0);

    // Asked for, not yet running: the factor the engine was prepared with, so the page never
    // takes a re-prepare still to come for the rate cap.
    auto pending = makeProc();
    setParam (*pending, "sat.os", 2.0f);
    info = web::advancedInfo (*pending);
    CHECK (num (info["oversampling"]["requested"]) == 2.0);
    CHECK (num (info["oversampling"]["active"]) == 2.0);
    pending->applyPendingConfig();
    info = web::advancedInfo (*pending);
    CHECK (num (info["oversampling"]["requested"]) == 4.0);
    CHECK (num (info["oversampling"]["active"]) == 4.0);

    auto sc = makeProc (48000.0, Set::mono());
    CHECK (num (web::advancedInfo (*sc)["sidechain"]["channels"]) == 1.0);
}

TEST_CASE ("advanced link: nothing runs before the processor is prepared", "[ui][web][advanced]")
{
    const JuceScope juce;
    Strat3Processor p (false);
    const auto info = web::advancedInfo (p);
    CHECK (info["topology"].isVoid());
    CHECK (info["crossovers"].isVoid());
    CHECK (info["crossoversSet"].isVoid());
    CHECK (info["oversampling"]["active"].isVoid());
    CHECK (num (info["oversampling"]["requested"]) == 2.0);   // the parameter's default
    CHECK (num (info["sampleRate"]) == 0.0);
    CHECK (num (info["sidechain"]["channels"]) == 0.0);

    auto prepared = makeProc();
    prepared->releaseResources();
    CHECK (web::advancedInfo (*prepared)["crossovers"].isVoid());
}

TEST_CASE ("advanced link: the committed contract is what this code writes", "[ui][web][advanced][contract]")
{
    const JuceScope juce;
    auto* o = new juce::DynamicObject;
    o->setProperty ("relays", web::relayContract());
    o->setProperty ("info", infoScenarios());
    const juce::var current (o);

    // The candidate first, so a re-record is a plain copy even when the comparison fails.
    const auto candidate = juce::File (STRAT3_CANDIDATE_DIR).getChildFile ("panel-contract").getChildFile ("advanced.json");
    REQUIRE (candidate.getParentDirectory().createDirectory().wasOk());
    REQUIRE (candidate.replaceWithText (juce::JSON::toString (current) + "\n", false, false, "\n"));

    const auto committed = juce::File (STRAT3_PANEL_CONTRACT_DIR).getChildFile ("advanced.json");
    INFO ("candidate: " << candidate.getFullPathName());
    REQUIRE (committed.existsAsFile());
    juce::var parsed;
    REQUIRE (juce::JSON::parse (committed.loadFileAsString(), parsed).wasOk());
    CHECK (strat3::test::difference (parsed, current, "advanced") == "");

    // Every situation the page draws is in it.
    for (const auto* name : { "default", "master-96k", "sidechain-connected", "not-prepared" })
    {
        INFO ("scenario: " << name);
        CHECK (parsed["info"].hasProperty (name));
    }
}
