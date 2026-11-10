// Assistant panel protocol and saved state (module 21a).
//   Commands (WebView -> native) are JSON objects: { "type": "...", ... }. Everything from the
//   page is untrusted input: a command that is not exactly well-formed is refused whole.
//   State (native -> WebView) is one JSON object carrying the whole session; the panel
//   re-renders from it, so there is no incremental protocol to drift out of sync.
//   Saved state: ASSISTANT child of EXTRA (references + log). A damaged entry is skipped,
//   the rest loads.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <string>

#include "assistant/Protocol.h"
#include "assistant/SessionState.h"

using namespace strat3::assistant;
using Catch::Approx;

namespace {

juce::var json (const char* text)
{
    juce::var v;
    REQUIRE (juce::JSON::parse (text, v).wasOk());
    return v;
}

Session sampleSession()
{
    Session s;
    prepare (s);
    Snapshot ref;
    for (std::size_t b = 0; b < static_cast<std::size_t> (kBands); ++b)
        ref.levelDb[b] = -30.0f + 0.1f * static_cast<float> (b);
    s.refs.push_back (ref);
    s.target = captureTarget (s.refs);
    s.hasTarget = true;

    Move m;
    m.id = 4;
    m.layer = "eq";
    m.changes = { { "eq.b1.on", 0.0f, 1.0f, false }, { "eq.b1.gain", 0.0f, -2.8f, true } };
    m.reason = "-2.8 dB @ 250 Hz: 4.0 dB above target";
    m.metric = "deviation from target (dB)";
    m.metricBefore = 4.0f;
    m.metricTarget = 0.0f;
    s.log.push_back ({ m, MoveStatus::Accepted, 0.5f, true });   // locked: from before the last analysis
    m.id = 7;
    m.layer = "limit";
    m.changes = { { "limit.thresh", 0.0f, -6.0f, true } };
    m.reason = "limiter threshold -6.0 dB";
    s.log.push_back ({ m, MoveStatus::Pending, 1.0f });
    s.nextId = 8;
    s.result = CaptureResult::Moves;
    return s;
}

} // namespace

// ------------------------------------------------------------------- commands

TEST_CASE ("protocol: every command parses", "[assistant][protocol]")
{
    auto c = parseCommand (json (R"({"type":"analyse","seconds":30})"));
    REQUIRE (c.has_value());
    CHECK (c->type == CommandType::Analyse);
    CHECK (c->seconds == 30.0);

    c = parseCommand (json (R"({"type":"captureReference","seconds":60})"));
    REQUIRE (c.has_value());
    CHECK (c->type == CommandType::CaptureReference);
    CHECK (c->seconds == 60.0);

    CHECK (parseCommand (json (R"({"type":"cancel"})"))->type == CommandType::Cancel);
    CHECK (parseCommand (json (R"({"type":"applyAll"})"))->type == CommandType::ApplyAll);
    CHECK (parseCommand (json (R"({"type":"clearTarget"})"))->type == CommandType::ClearTarget);

    c = parseCommand (json (R"({"type":"accept","id":3,"depth":0.25})"));
    REQUIRE (c.has_value());
    CHECK (c->type == CommandType::Accept);
    CHECK (c->id == 3);
    CHECK (c->hasDepth);
    CHECK (c->depth == 0.25f);

    c = parseCommand (json (R"({"type":"accept","id":3})"));   // no depth: the stored one
    REQUIRE (c.has_value());
    CHECK_FALSE (c->hasDepth);

    c = parseCommand (json (R"({"type":"reject","id":9})"));
    REQUIRE (c.has_value());
    CHECK (c->type == CommandType::Reject);
    CHECK (c->id == 9);

    c = parseCommand (json (R"({"type":"depth","id":2,"depth":1})"));
    REQUIRE (c.has_value());
    CHECK (c->type == CommandType::Depth);
    CHECK (c->depth == 1.0f);
}

TEST_CASE ("protocol: malformed commands are refused whole", "[assistant][protocol]")
{
    CHECK_FALSE (parseCommand (juce::var()).has_value());
    CHECK_FALSE (parseCommand (juce::var (42)).has_value());
    CHECK_FALSE (parseCommand (json (R"([1,2,3])")).has_value());
    CHECK_FALSE (parseCommand (json (R"({})")).has_value());
    CHECK_FALSE (parseCommand (json (R"({"type":7})")).has_value());
    CHECK_FALSE (parseCommand (json (R"({"type":"format-disk"})")).has_value());
    // Only the two capture lengths the panel offers.
    CHECK_FALSE (parseCommand (json (R"({"type":"analyse"})")).has_value());
    CHECK_FALSE (parseCommand (json (R"({"type":"analyse","seconds":45})")).has_value());
    CHECK_FALSE (parseCommand (json (R"({"type":"analyse","seconds":"30"})")).has_value());
    CHECK_FALSE (parseCommand (json (R"({"type":"captureReference","seconds":-30})")).has_value());
    // Ids are positive whole numbers.
    CHECK_FALSE (parseCommand (json (R"({"type":"accept"})")).has_value());
    CHECK_FALSE (parseCommand (json (R"({"type":"accept","id":"3"})")).has_value());
    CHECK_FALSE (parseCommand (json (R"({"type":"accept","id":2.5})")).has_value());
    CHECK_FALSE (parseCommand (json (R"({"type":"reject","id":0})")).has_value());
    CHECK_FALSE (parseCommand (json (R"({"type":"reject","id":-1})")).has_value());
    CHECK_FALSE (parseCommand (json (R"({"type":"reject","id":1e12})")).has_value());
    // Depth is a number in 0..1; the depth command needs one.
    CHECK_FALSE (parseCommand (json (R"({"type":"depth","id":2})")).has_value());
    CHECK_FALSE (parseCommand (json (R"({"type":"depth","id":2,"depth":"0.5"})")).has_value());
    CHECK_FALSE (parseCommand (json (R"({"type":"depth","id":2,"depth":1.5})")).has_value());
    CHECK_FALSE (parseCommand (json (R"({"type":"accept","id":2,"depth":-0.1})")).has_value());
    CHECK_FALSE (parseCommand (json (R"({"type":"accept","id":2,"depth":true})")).has_value());
}

TEST_CASE ("protocol: a non-finite depth is refused", "[assistant][protocol]")
{
    auto* o = new juce::DynamicObject();
    const juce::var v (o);
    o->setProperty ("type", "depth");
    o->setProperty ("id", 2);
    o->setProperty ("depth", std::numeric_limits<double>::quiet_NaN());
    CHECK_FALSE (parseCommand (v).has_value());
    o->setProperty ("depth", std::numeric_limits<double>::infinity());
    CHECK_FALSE (parseCommand (v).has_value());
}

// ------------------------------------------------------------------- state -> panel

TEST_CASE ("protocol: the state object carries the whole session", "[assistant][protocol]")
{
    auto s = sampleSession();
    REQUIRE (startCapture (s, CaptureKind::Analyse, 30.0));
    for (int f = 0; f < 225; ++f)
    {
        BandEnergies e;
        e.fill (1e-3);
        addFrame (s, e, e, 1.0f);
    }
    // Through text and back: what the page receives is exactly this JSON.
    const auto v = json (juce::JSON::toString (stateToVar (s)).toRawUTF8());

    CHECK (static_cast<int> (v["revision"]) == static_cast<int> (s.revision));
    CHECK (v["capture"]["kind"].toString() == "analyse");
    CHECK (static_cast<double> (v["capture"]["progress"]) == Approx (0.25));
    CHECK (static_cast<double> (v["capture"]["seconds"]) == 30.0);
    CHECK (v["capture"].hasProperty ("paused"));
    CHECK_FALSE (static_cast<bool> (v["capture"]["paused"]));
    CHECK (static_cast<bool> (stateToVar (s, true)["capture"]["paused"]));
    CHECK (v["result"].toString() == "moves");
    CHECK (static_cast<bool> (v["target"]["captured"]));
    CHECK (static_cast<int> (v["target"]["references"]) == 1);
    CHECK (static_cast<int> (v["target"]["maxReferences"]) == kMaxReferences);
    REQUIRE (v["target"]["levelDb"].isArray());
    REQUIRE (v["target"]["levelDb"].size() == kBands);
    CHECK (static_cast<double> (v["target"]["levelDb"][10]) == Approx (static_cast<double> (s.target.levelDb[10])));
    REQUIRE (v["target"]["bandHz"].size() == kBands);
    CHECK (static_cast<double> (v["target"]["bandHz"][34]) == Approx (1000.0));

    REQUIRE (v["moves"].isArray());
    REQUIRE (v["moves"].size() == 2);
    const auto m = v["moves"][0];
    CHECK (static_cast<int> (m["id"]) == 4);
    CHECK (m["layer"].toString() == "eq");
    CHECK (m["status"].toString() == "accepted");
    CHECK (static_cast<double> (m["depth"]) == 0.5);
    CHECK (m["reason"].toString() == "-2.8 dB @ 250 Hz: 4.0 dB above target");
    CHECK (m["metric"].toString() == "deviation from target (dB)");
    CHECK (static_cast<double> (m["metricBefore"]) == 4.0);
    CHECK (static_cast<double> (m["metricTarget"]) == 0.0);
    REQUIRE (m["changes"].size() == 2);
    CHECK (m["changes"][1]["paramId"].toString() == "eq.b1.gain");
    CHECK (static_cast<double> (m["changes"][1]["from"]) == 0.0);
    CHECK (static_cast<double> (m["changes"][1]["to"]) == Approx (-2.8));
    CHECK (static_cast<bool> (m["changes"][1]["scaled"]));
    CHECK_FALSE (static_cast<bool> (m["changes"][0]["scaled"]));
    CHECK (v["moves"][1]["status"].toString() == "pending");
    CHECK (static_cast<bool> (m["locked"]));
    CHECK (v["moves"][1].hasProperty ("locked"));
    CHECK_FALSE (static_cast<bool> (v["moves"][1]["locked"]));
}

TEST_CASE ("protocol: an empty session and every status / result name", "[assistant][protocol]")
{
    Session s;
    prepare (s);
    auto v = stateToVar (s);
    CHECK (v["capture"]["kind"].toString() == "none");
    CHECK_FALSE (static_cast<bool> (stateToVar (s, true)["capture"]["paused"]));   // nothing running: nothing to pause
    CHECK (static_cast<double> (v["capture"]["progress"]) == 0.0);
    CHECK (v["result"].toString() == "none");
    CHECK_FALSE (static_cast<bool> (v["target"]["captured"]));
    CHECK (v["target"]["levelDb"].size() == 0);   // no curve to draw yet
    CHECK (v["moves"].size() == 0);

    const std::pair<CaptureResult, const char*> results[] = {
        { CaptureResult::Moves, "moves" }, { CaptureResult::NoMoves, "noMoves" },
        { CaptureResult::ReferenceAdded, "referenceAdded" }, { CaptureResult::Silent, "silent" },
        { CaptureResult::Cancelled, "cancelled" } };
    for (const auto& [r, name] : results)
    {
        s.result = r;
        CHECK (stateToVar (s)["result"].toString() == name);
    }
    s = sampleSession();
    s.log[0].status = MoveStatus::Rejected;
    CHECK (stateToVar (s)["moves"][0]["status"].toString() == "rejected");
    REQUIRE (startCapture (s, CaptureKind::Reference, 60.0));
    CHECK (stateToVar (s)["capture"]["kind"].toString() == "reference");
}

TEST_CASE ("protocol: non-finite numbers never reach the page as invalid JSON", "[assistant][protocol]")
{
    auto s = sampleSession();
    s.log[0].move.metricBefore = std::numeric_limits<float>::quiet_NaN();
    s.log[0].move.changes[1].to = std::numeric_limits<float>::infinity();
    const auto text = juce::JSON::toString (stateToVar (s));
    juce::var back;
    REQUIRE (juce::JSON::parse (text, back).wasOk());
    CHECK_FALSE (text.containsIgnoreCase ("nan"));
    CHECK_FALSE (text.containsIgnoreCase ("inf"));
    // Still numbers, not null: the page's types say number, and null would get past them.
    const auto before = back["moves"][0]["metricBefore"], to = back["moves"][0]["changes"][1]["to"];
    CHECK ((before.isInt() || before.isInt64() || before.isDouble()));
    CHECK ((to.isInt() || to.isInt64() || to.isDouble()));
    CHECK (static_cast<double> (before) == 0.0);
    CHECK (static_cast<double> (to) == 0.0);
}

// ------------------------------------------------------------------- saved state

TEST_CASE ("saved state: references and the log round-trip", "[assistant][state]")
{
    const auto a = sampleSession();
    const auto tree = toTree (a);
    CHECK (tree.getType() == kAssistantType);
    // Through XML, as the host stores it.
    const auto xml = tree.createXml();
    REQUIRE (xml != nullptr);
    const auto loaded = juce::ValueTree::fromXml (*xml);

    Session b;
    prepare (b);
    fromTree (b, loaded);
    REQUIRE (b.refs.size() == 1);
    for (std::size_t k = 0; k < static_cast<std::size_t> (kBands); ++k)
        CHECK (b.refs[0].levelDb[k] == Approx (a.refs[0].levelDb[k]).margin (1e-4));
    CHECK (b.hasTarget);
    CHECK (b.target.levelDb[5] == Approx (a.target.levelDb[5]).margin (1e-4));
    REQUIRE (b.log.size() == 2);
    CHECK (b.log[0].move.id == 4);
    CHECK (b.log[0].status == MoveStatus::Accepted);
    CHECK (b.log[0].depth == 0.5f);
    CHECK (b.log[0].move.layer == "eq");
    CHECK (b.log[0].move.reason == a.log[0].move.reason);
    CHECK (b.log[0].move.metric == a.log[0].move.metric);
    CHECK (b.log[0].move.metricBefore == 4.0f);
    REQUIRE (b.log[0].move.changes.size() == 2);
    CHECK (b.log[0].move.changes[1].paramId == "eq.b1.gain");
    CHECK (b.log[0].move.changes[1].to == Approx (-2.8f));
    CHECK (b.log[0].move.changes[1].scaled);
    CHECK_FALSE (b.log[0].move.changes[0].scaled);
    CHECK (b.log[1].status == MoveStatus::Pending);
    CHECK (b.log[0].locked);
    CHECK_FALSE (b.log[1].locked);
    CHECK (b.nextId == 8);   // ids stay unique after a reload
    CHECK (b.capturing == CaptureKind::None);
}

TEST_CASE ("saved state: loading replaces the session and cancels a running capture", "[assistant][state]")
{
    auto b = sampleSession();
    REQUIRE (startCapture (b, CaptureKind::Analyse, 30.0));
    const auto rev = b.revision;
    Session empty;
    prepare (empty);
    fromTree (b, toTree (empty));
    CHECK (b.capturing == CaptureKind::None);
    CHECK (b.log.empty());
    CHECK (b.refs.empty());
    CHECK_FALSE (b.hasTarget);
    CHECK (b.revision != rev);

    b = sampleSession();
    fromTree (b, juce::ValueTree());   // a session saved before the assistant existed
    CHECK (b.log.empty());
    CHECK_FALSE (b.hasTarget);
}

TEST_CASE ("saved state: damaged entries are skipped, the rest loads", "[assistant][state]")
{
    auto tree = toTree (sampleSession());
    // A reference with the wrong band count, one with a NaN, and a stray node.
    juce::ValueTree shortRef (kReferenceType);
    shortRef.setProperty (kLevelsProp, "1,2,3", nullptr);
    tree.appendChild (shortRef, nullptr);
    juce::ValueTree nanRef (kReferenceType);
    juce::StringArray levels;
    for (int i = 0; i < kBands; ++i)
        levels.add (i == 7 ? "nan" : "-30");
    nanRef.setProperty (kLevelsProp, levels.joinIntoString (","), nullptr);
    tree.appendChild (nanRef, nullptr);
    juce::ValueTree longRef (kReferenceType);   // one band too many: not this build's layout
    levels.clear();
    for (int i = 0; i < kBands + 1; ++i)
        levels.add ("-30");
    longRef.setProperty (kLevelsProp, levels.joinIntoString (","), nullptr);
    tree.appendChild (longRef, nullptr);
    tree.appendChild (juce::ValueTree ("WHATEVER"), nullptr);
    // A move with no id, one with a duplicate id, one with a non-finite change, one with an
    // unknown status and an out-of-range depth.
    juce::ValueTree noId (kMoveType);
    tree.appendChild (noId, nullptr);
    juce::ValueTree dup (kMoveType);   // complete, but its id is taken: the panel addresses moves by id
    dup.setProperty (kIdProp, 4, nullptr);
    juce::ValueTree dupChange (kChangeType);
    dupChange.setProperty (kParamProp, "eq.b3.on", nullptr);
    dupChange.setProperty (kFromProp, 0.0, nullptr);
    dupChange.setProperty (kToProp, 1.0, nullptr);
    dup.appendChild (dupChange, nullptr);
    tree.appendChild (dup, nullptr);
    juce::ValueTree bad (kMoveType);
    bad.setProperty (kIdProp, 20, nullptr);
    juce::ValueTree goodChange (kChangeType);   // a valid change first: the move must still go as a whole
    goodChange.setProperty (kParamProp, "eq.b2.on", nullptr);
    goodChange.setProperty (kFromProp, 0.0, nullptr);
    goodChange.setProperty (kToProp, 1.0, nullptr);
    bad.appendChild (goodChange, nullptr);
    juce::ValueTree badChange (kChangeType);
    badChange.setProperty (kParamProp, "eq.b2.gain", nullptr);
    badChange.setProperty (kFromProp, 0.0, nullptr);
    badChange.setProperty (kToProp, "inf", nullptr);
    bad.appendChild (badChange, nullptr);
    tree.appendChild (bad, nullptr);
    juce::ValueTree odd (kMoveType);
    odd.setProperty (kIdProp, 30, nullptr);
    odd.setProperty (kStatusProp, "exploded", nullptr);
    odd.setProperty (kDepthProp, 9.0, nullptr);
    juce::ValueTree okChange (kChangeType);
    okChange.setProperty (kParamProp, "limit.thresh", nullptr);
    okChange.setProperty (kFromProp, 0.0, nullptr);
    okChange.setProperty (kToProp, -3.0, nullptr);
    okChange.setProperty (kScaledProp, true, nullptr);
    odd.appendChild (okChange, nullptr);
    tree.appendChild (odd, nullptr);

    Session s;
    prepare (s);
    fromTree (s, tree);
    CHECK (s.refs.size() == 1);
    REQUIRE (s.log.size() == 3);
    CHECK (s.log[0].move.id == 4);
    CHECK (s.log[1].move.id == 7);
    CHECK (s.log[2].move.id == 30);
    CHECK (s.log[2].status == MoveStatus::Pending);
    CHECK (s.log[2].depth == 1.0f);
    CHECK (s.nextId == 31);
}

TEST_CASE ("saved state: counts are capped on load", "[assistant][state]")
{
    juce::ValueTree tree (kAssistantType);
    juce::StringArray levels;
    for (int i = 0; i < kBands; ++i)
        levels.add ("-30");
    for (int i = 0; i < kMaxReferences + 10; ++i)
    {
        juce::ValueTree r (kReferenceType);
        r.setProperty (kLevelsProp, levels.joinIntoString (","), nullptr);
        tree.appendChild (r, nullptr);
    }
    for (int i = 0; i < kMaxLog + 10; ++i)
    {
        juce::ValueTree m (kMoveType);
        m.setProperty (kIdProp, i + 1, nullptr);
        juce::ValueTree c (kChangeType);
        c.setProperty (kParamProp, "limit.thresh", nullptr);
        c.setProperty (kFromProp, 0.0, nullptr);
        c.setProperty (kToProp, -1.0, nullptr);
        m.appendChild (c, nullptr);
        tree.appendChild (m, nullptr);
    }
    Session s;
    prepare (s);
    fromTree (s, tree);
    CHECK (static_cast<int> (s.refs.size()) == kMaxReferences);
    CHECK (static_cast<int> (s.log.size()) == kMaxLog);
}
