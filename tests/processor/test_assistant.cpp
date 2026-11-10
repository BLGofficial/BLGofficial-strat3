// Processor <-> assistant session (module 21a): the capture is fed from the analysis thread,
// commands arrive as JSON on the message thread, accepted moves reach the host as gestures.
// Contracts tested here:
//   - a capture is fed the OUTPUT spectrum and loudness, one frame per analysis tick that
//     saw new audio; a stopped transport stalls it
//   - accept / depth / reject set real parameters inside begin/endChangeGesture (host undo
//     and automation see them); a change naming an unknown parameter is skipped
//   - while a delta is heard the output is not the mix: the capture pauses (and for half a
//     second after, until the delta has left the analysis windows) and says so
//   - commands the protocol refuses change nothing
//   - the session (target, log) is saved with the plugin state and restored
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <chrono>
#include <map>
#include <memory>
#include <numbers>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "assistant/Protocol.h"
#include "assistant/SessionState.h"
#include "params/StateMigration.h"
#include "processor/Strat3Processor.h"

using namespace strat3;
using processor::Strat3Processor;
using Catch::Approx;

namespace {

constexpr double kFs = 48000.0;
constexpr int kBlock = 512;

struct JuceScope
{
    juce::ScopedJuceInitialiser_GUI init;
};

juce::var json (const std::string& text)
{
    juce::var v;
    REQUIRE (juce::JSON::parse (juce::String (text), v).wasOk());
    return v;
}

std::unique_ptr<Strat3Processor> makeProc()
{
    auto p = std::make_unique<Strat3Processor> (false);
    p->setRateAndBufferSizeDetails (kFs, kBlock);
    p->prepareToPlay (kFs, kBlock);
    return p;
}

void setParam (Strat3Processor& p, const std::string& id, float value)
{
    auto* param = p.apvts.getParameter (id);
    REQUIRE (param != nullptr);
    param->setValueNotifyingHost (param->convertTo0to1 (value));
}

float rawValue (Strat3Processor& p, const std::string& id) { return p.apvts.getRawParameterValue (id)->load(); }

/** `seconds` of stereo noise (independent channels) plus an optional mono sine, with an
 *  analysis tick every 3 blocks (~31 Hz, like the analysis thread). */
void play (Strat3Processor& p, double seconds, float rms, unsigned seed, double sineHz = 0.0, float sineAmp = 0.0f)
{
    std::mt19937 rng (seed);
    const float a = rms * std::sqrt (3.0f);
    std::uniform_real_distribution<float> d (-a, a);
    juce::AudioBuffer<float> buf (2, kBlock);
    juce::MidiBuffer midi;
    const int blocks = static_cast<int> (seconds * kFs / kBlock);
    std::int64_t n = 0;
    for (int b = 0; b < blocks; ++b)
    {
        for (int i = 0; i < kBlock; ++i, ++n)
        {
            const auto s = sineAmp * static_cast<float> (std::sin (2.0 * std::numbers::pi * sineHz * static_cast<double> (n) / kFs));
            buf.setSample (0, i, d (rng) + s);
            buf.setSample (1, i, d (rng) + s);
        }
        p.processBlock (buf, midi);
        if (b % 3 == 2)
            p.analysisTick();
    }
}

struct GestureLog final : juce::AudioProcessorListener
{
    std::map<int, int> open;
    int begun = 0, ended = 0, changesOutsideGesture = 0;
    void audioProcessorParameterChanged (juce::AudioProcessor*, int index, float) override
    {
        if (open[index] <= 0)
            ++changesOutsideGesture;
    }
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override {}
    void audioProcessorParameterChangeGestureBegin (juce::AudioProcessor*, int index) override
    {
        ++open[index];
        ++begun;
    }
    void audioProcessorParameterChangeGestureEnd (juce::AudioProcessor*, int index) override
    {
        --open[index];
        ++ended;
    }
};

juce::var findMove (const juce::var& state, const juce::String& layer)
{
    for (int i = 0; i < state["moves"].size(); ++i)
        if (state["moves"][i]["layer"].toString() == layer)
            return state["moves"][i];
    return {};
}

double changeTo (const juce::var& move, const juce::String& paramId)
{
    for (int i = 0; i < move["changes"].size(); ++i)
        if (move["changes"][i]["paramId"].toString() == paramId)
            return static_cast<double> (move["changes"][i]["to"]);
    FAIL ("no change for " << paramId);
    return 0.0;
}

std::string cmd (const char* type, int id) { return std::string (R"({"type":")") + type + R"(","id":)" + std::to_string (id) + "}"; }

} // namespace

TEST_CASE ("assistant: an analysis capture measures the output and proposes; accept sets the parameters as gestures", "[processor][assistant]")
{
    const JuceScope juce;
    auto p = makeProc();
    setParam (*p, "global.target", 1.0f);   // -14 LUFS
    setParam (*p, "low.monoOn", 1.0f);      // independent channels: keep the low rule out of this test
    CHECK (static_cast<int> (p->assistantState()["moves"].size()) == 0);

    const auto rev = p->assistantRevision();
    REQUIRE (p->startAssistantCapture (assistant::CaptureKind::Analyse, 2.0));
    CHECK (p->assistantRevision() != rev);
    play (*p, 1.0, 0.05f, 1);
    auto state = p->assistantState();
    CHECK (state["capture"]["kind"].toString() == "analyse");
    CHECK (static_cast<double> (state["capture"]["progress"]) > 0.3);
    CHECK (static_cast<double> (state["capture"]["progress"]) < 0.7);
    play (*p, 1.5, 0.05f, 2);

    state = p->assistantState();
    CHECK (state["capture"]["kind"].toString() == "none");
    CHECK (state["result"].toString() == "moves");
    const auto move = findMove (state, "limit");
    REQUIRE (move.isObject());
    // The capture's own LUFS-I: steady noise, so it matches the output meter.
    CHECK (static_cast<double> (move["metricBefore"]) == Approx (static_cast<double> (p->meters().integratedLufs)).margin (0.5));
    CHECK (static_cast<double> (move["metricBefore"]) < -17.0);   // well under the -14 target

    CHECK (rawValue (*p, "limit.on") == 0.0f);
    GestureLog log;
    p->addListener (&log);
    const int id = static_cast<int> (move["id"]);
    REQUIRE (p->assistantCommand (json (cmd ("accept", id))));
    p->removeListener (&log);

    CHECK (rawValue (*p, "limit.on") == 1.0f);
    CHECK (rawValue (*p, "limit.thresh") == Approx (static_cast<float> (changeTo (move, "limit.thresh"))).margin (1e-3));
    CHECK (log.begun == static_cast<int> (move["changes"].size()));
    CHECK (log.ended == log.begun);
    CHECK (log.changesOutsideGesture == 0);
    CHECK (findMove (p->assistantState(), "limit")["status"].toString() == "accepted");
}

TEST_CASE ("assistant: depth re-applies an accepted move, reject restores it", "[processor][assistant]")
{
    const JuceScope juce;
    auto p = makeProc();
    setParam (*p, "global.target", 1.0f);
    setParam (*p, "low.monoOn", 1.0f);
    REQUIRE (p->startAssistantCapture (assistant::CaptureKind::Analyse, 1.0));
    play (*p, 1.5, 0.05f, 3);
    const auto move = findMove (p->assistantState(), "limit");
    REQUIRE (move.isObject());
    const int id = static_cast<int> (move["id"]);
    const auto to = static_cast<float> (changeTo (move, "limit.thresh"));
    REQUIRE (to < -1.0f);

    REQUIRE (p->assistantCommand (json (R"({"type":"accept","id":)" + std::to_string (id) + R"(,"depth":0.5})")));
    CHECK (rawValue (*p, "limit.thresh") == Approx (0.5f * to).margin (1e-3));
    REQUIRE (p->assistantCommand (json (R"({"type":"depth","id":)" + std::to_string (id) + R"(,"depth":0.25})")));
    CHECK (rawValue (*p, "limit.thresh") == Approx (0.25f * to).margin (1e-3));
    CHECK (rawValue (*p, "limit.on") == 1.0f);

    REQUIRE (p->assistantCommand (json (cmd ("reject", id))));
    CHECK (rawValue (*p, "limit.thresh") == 0.0f);
    CHECK (rawValue (*p, "limit.on") == 0.0f);
    CHECK (findMove (p->assistantState(), "limit")["status"].toString() == "rejected");

    REQUIRE (p->assistantCommand (json (R"({"type":"applyAll"})")));   // nothing pending
    CHECK (rawValue (*p, "limit.on") == 0.0f);

    // Accept without a depth uses the one the slider left on the move.
    REQUIRE (p->assistantCommand (json (R"({"type":"depth","id":)" + std::to_string (id) + R"(,"depth":0.75})")));
    CHECK (rawValue (*p, "limit.thresh") == 0.0f);   // rejected: the slider alone sets nothing
    REQUIRE (p->assistantCommand (json (cmd ("accept", id))));
    CHECK (rawValue (*p, "limit.thresh") == Approx (0.75f * to).margin (1e-3));
}

TEST_CASE ("assistant: after a new analysis, commands on the older moves are refused", "[processor][assistant]")
{
    const JuceScope juce;
    auto p = makeProc();
    setParam (*p, "global.target", 1.0f);
    setParam (*p, "low.monoOn", 1.0f);
    REQUIRE (p->startAssistantCapture (assistant::CaptureKind::Analyse, 1.0));
    play (*p, 1.5, 0.05f, 11);
    const auto first = findMove (p->assistantState(), "limit");
    REQUIRE (first.isObject());
    const int id = static_cast<int> (first["id"]);
    REQUIRE (p->assistantCommand (json (cmd ("accept", id))));
    const float thresh = rawValue (*p, "limit.thresh");
    REQUIRE (thresh < -1.0f);

    REQUIRE (p->startAssistantCapture (assistant::CaptureKind::Analyse, 1.0));
    play (*p, 1.5, 0.05f, 12);
    const auto state = p->assistantState();
    REQUIRE (static_cast<int> (state["moves"][0]["id"]) == id);
    CHECK (static_cast<bool> (state["moves"][0]["locked"]));

    const auto rev = p->assistantRevision();
    CHECK_FALSE (p->assistantCommand (json (cmd ("reject", id))));
    CHECK_FALSE (p->assistantCommand (json (cmd ("accept", id))));
    CHECK_FALSE (p->assistantCommand (json (R"({"type":"depth","id":)" + std::to_string (id) + R"(,"depth":0})")));
    CHECK (rawValue (*p, "limit.thresh") == thresh);
    CHECK (rawValue (*p, "limit.on") == 1.0f);
    CHECK (p->assistantRevision() == rev);
}

TEST_CASE ("assistant: a stopped transport stalls the capture; cancel ends it", "[processor][assistant]")
{
    const JuceScope juce;
    auto p = makeProc();
    play (*p, 0.5, 0.05f, 4);
    REQUIRE (p->assistantCommand (json (R"({"type":"analyse","seconds":30})")));
    play (*p, 0.5, 0.05f, 5);
    p->analysisTick();   // takes the audio that arrived after play()'s last tick
    const double before = static_cast<double> (p->assistantState()["capture"]["progress"]);
    CHECK (before > 0.0);
    for (int i = 0; i < 2000; ++i)   // more ticks than the capture has frames, no audio
        p->analysisTick();
    const auto state = p->assistantState();
    CHECK (state["capture"]["kind"].toString() == "analyse");
    CHECK (static_cast<double> (state["capture"]["progress"]) == before);

    CHECK_FALSE (p->assistantCommand (json (R"({"type":"captureReference","seconds":30})")));   // one at a time
    REQUIRE (p->assistantCommand (json (R"({"type":"cancel"})")));
    CHECK (p->assistantState()["capture"]["kind"].toString() == "none");
    CHECK (p->assistantState()["result"].toString() == "cancelled");
}

TEST_CASE ("assistant: a delta listen pauses the capture until the mix is back", "[processor][assistant]")
{
    const JuceScope juce;
    // Delta replaces the monitor signal, and the output taps sit after the monitor point:
    // analysing it would propose moves for the difference signal, not for the mix.
    for (const std::string deltaParam : { "global.delta", "eq.delta" })
    {
        INFO (deltaParam);
        auto p = makeProc();
        REQUIRE (p->startAssistantCapture (assistant::CaptureKind::Analyse, 3.0));
        play (*p, 1.0, 0.05f, 20);
        p->analysisTick();
        auto state = p->assistantState();
        const double before = static_cast<double> (state["capture"]["progress"]);
        CHECK (before > 0.2);
        CHECK_FALSE (static_cast<bool> (state["capture"]["paused"]));

        const auto rev = p->assistantRevision();
        setParam (*p, deltaParam, 1.0f);
        play (*p, 1.0, 0.05f, 21);
        state = p->assistantState();
        CHECK (state["capture"]["kind"].toString() == "analyse");
        CHECK (static_cast<double> (state["capture"]["progress"]) == before);
        CHECK (static_cast<bool> (state["capture"]["paused"]));
        CHECK (p->assistantRevision() != rev);   // the panel hears about the pause

        // Delta off: the pause holds for kAssistantDeltaHoldTicks more ticks (the analysis
        // windows and the loudness gate still contain delta audio), then the capture goes on.
        setParam (*p, deltaParam, 0.0f);
        play (*p, 0.25, 0.05f, 22);   // ~8 ticks: inside the hold
        state = p->assistantState();
        CHECK (static_cast<double> (state["capture"]["progress"]) == before);
        CHECK (static_cast<bool> (state["capture"]["paused"]));
        play (*p, 1.0, 0.05f, 23);
        state = p->assistantState();
        CHECK (static_cast<double> (state["capture"]["progress"]) > before);
        CHECK_FALSE (static_cast<bool> (state["capture"]["paused"]));
    }
}

TEST_CASE ("assistant: loudness heard during a delta listen is not part of the capture", "[processor][assistant]")
{
    const JuceScope juce;
    auto p = makeProc();
    setParam (*p, "global.target", 1.0f);
    setParam (*p, "low.monoOn", 1.0f);
    // A loud limiter push makes the global delta (everything the chain adds) clearly audible.
    setParam (*p, "limit.on", 1.0f);
    setParam (*p, "limit.thresh", -12.0f);
    play (*p, 2.5, 0.05f, 30);   // with the next second: a full 3 s short-term window of the mix
    REQUIRE (p->startAssistantCapture (assistant::CaptureKind::Analyse, 2.0));
    play (*p, 1.0, 0.05f, 31);
    const float mixLufs = p->meters().shortTermLufs;
    setParam (*p, "global.delta", 1.0f);
    play (*p, 4.0, 0.05f, 32);   // long enough to dominate an integrated reading if it leaked in
    const float deltaLufs = p->meters().shortTermLufs;
    REQUIRE (std::abs (deltaLufs - mixLufs) > 1.0f);   // the delta really is a different signal
    setParam (*p, "global.delta", 0.0f);
    play (*p, 2.5, 0.05f, 33);

    const auto state = p->assistantState();
    REQUIRE (state["capture"]["kind"].toString() == "none");
    const auto move = findMove (state, "limit");
    REQUIRE (move.isObject());
    CHECK (static_cast<double> (move["metricBefore"]) == Approx (static_cast<double> (mixLufs)).margin (0.5));
}

TEST_CASE ("assistant: a reference capture sets the target; a different mix gets a tonal move", "[processor][assistant]")
{
    const JuceScope juce;
    auto p = makeProc();
    setParam (*p, "low.monoOn", 1.0f);
    REQUIRE (p->startAssistantCapture (assistant::CaptureKind::Reference, 2.0));
    play (*p, 2.5, 0.05f, 6);
    auto state = p->assistantState();
    CHECK (state["result"].toString() == "referenceAdded");
    REQUIRE (static_cast<bool> (state["target"]["captured"]));
    CHECK (static_cast<int> (state["target"]["references"]) == 1);
    CHECK (state["moves"].size() == 0);

    // The same noise with a strong 250 Hz tone on top: one cut near 250 Hz.
    REQUIRE (p->startAssistantCapture (assistant::CaptureKind::Analyse, 2.0));
    play (*p, 2.5, 0.05f, 7, 250.0, 0.1f);
    state = p->assistantState();
    const auto move = findMove (state, "eq");
    REQUIRE (move.isObject());
    CHECK (changeTo (move, "eq.b1.freq") == Approx (250.0).epsilon (0.15));
    CHECK (changeTo (move, "eq.b1.gain") < -1.0);
    CHECK (static_cast<double> (move["metricBefore"]) > 2.0);

    setParam (*p, "eq.on", 0.0f);
    REQUIRE (p->assistantCommand (json (cmd ("accept", static_cast<int> (move["id"])))));
    CHECK (rawValue (*p, "eq.b1.on") == 1.0f);
    CHECK (rawValue (*p, "eq.b1.gain") == Approx (static_cast<float> (changeTo (move, "eq.b1.gain"))).margin (1e-3));

    REQUIRE (p->assistantCommand (json (R"({"type":"clearTarget"})")));
    CHECK_FALSE (static_cast<bool> (p->assistantState()["target"]["captured"]));
}

TEST_CASE ("assistant: refused commands change nothing", "[processor][assistant]")
{
    const JuceScope juce;
    auto p = makeProc();
    const auto rev = p->assistantRevision();
    CHECK_FALSE (p->assistantCommand (juce::var()));
    CHECK_FALSE (p->assistantCommand (json (R"({"type":"analyse","seconds":45})")));
    CHECK_FALSE (p->assistantCommand (json (R"({"type":"accept","id":1})")));   // no such move
    CHECK_FALSE (p->assistantCommand (json (R"({"type":"cancel"})")));          // nothing running
    CHECK (p->assistantRevision() == rev);
    CHECK (p->assistantState()["capture"]["kind"].toString() == "none");
}

TEST_CASE ("assistant: the session is saved with the plugin state and restored", "[processor][assistant][state]")
{
    const JuceScope juce;
    auto a = makeProc();
    setParam (*a, "global.target", 1.0f);
    setParam (*a, "low.monoOn", 1.0f);
    REQUIRE (a->startAssistantCapture (assistant::CaptureKind::Reference, 1.0));
    play (*a, 1.5, 0.05f, 8);
    REQUIRE (a->startAssistantCapture (assistant::CaptureKind::Analyse, 1.0));
    play (*a, 1.5, 0.05f, 9);
    const auto before = a->assistantState();
    REQUIRE (before["moves"].size() >= 1);
    REQUIRE (a->startAssistantCapture (assistant::CaptureKind::Analyse, 30.0));   // a running capture is not saved

    juce::MemoryBlock blob;
    a->getStateInformation (blob);
    auto b = makeProc();
    const auto rev = b->assistantRevision();
    b->setStateInformation (blob.getData(), static_cast<int> (blob.getSize()));
    const auto after = b->assistantState();
    CHECK (b->assistantRevision() != rev);
    CHECK (after["capture"]["kind"].toString() == "none");
    CHECK (static_cast<bool> (after["target"]["captured"]));
    CHECK (static_cast<int> (after["target"]["references"]) == 1);
    REQUIRE (after["moves"].size() == before["moves"].size());
    CHECK (after["moves"][0]["reason"].toString() == before["moves"][0]["reason"].toString());
    CHECK (static_cast<int> (after["moves"][0]["id"]) == static_cast<int> (before["moves"][0]["id"]));

    // The restored move still works.
    const auto move = findMove (after, "limit");
    REQUIRE (move.isObject());
    REQUIRE (b->assistantCommand (json (cmd ("accept", static_cast<int> (move["id"])))));
    CHECK (rawValue (*b, "limit.on") == 1.0f);

    // One copy of the session: it lives in the processor, not also in the EXTRA tree (a stale
    // copy there would be saved again next to the live one).
    CHECK_FALSE (b->extraState.getChildWithName (assistant::kAssistantType).isValid());
    juce::MemoryBlock again;
    b->getStateInformation (again);
    const auto xml = juce::AudioProcessor::getXmlFromBinary (again.getData(), static_cast<int> (again.getSize()));
    REQUIRE (xml != nullptr);
    const auto extra = juce::ValueTree::fromXml (*xml).getChildWithName (params::kExtraType);
    int sessions = 0;
    for (const auto& child : extra)
        sessions += child.hasType (assistant::kAssistantType) ? 1 : 0;
    CHECK (sessions == 1);

    // The layer order next to it in EXTRA is untouched.
    CHECK (params::decodeLayerOrder (b->extraState) == params::defaultLayerOrder());
}

TEST_CASE ("assistant: a saved move naming an unknown parameter applies the rest", "[processor][assistant][state]")
{
    const JuceScope juce;
    auto a = makeProc();
    assistant::Session s;
    assistant::prepare (s);
    assistant::Move m;
    m.id = 1;
    m.layer = "limit";
    m.changes = { { "no.such.param", 0.0f, 1.0f, false }, { "limit.thresh", 0.0f, -4.0f, true }, { "limit.on", 0.0f, 1.0f, false } };
    m.reason = "from a newer build";
    s.log.push_back ({ m, assistant::MoveStatus::Pending, 1.0f });

    auto extra = params::makeDefaultExtraState();
    extra.appendChild (assistant::toTree (s), nullptr);
    const auto envelope = params::wrapState (a->apvts.copyState(), extra);
    juce::MemoryBlock blob;
    const auto xml = envelope.createXml();
    REQUIRE (xml != nullptr);
    juce::AudioProcessor::copyXmlToBinary (*xml, blob);   // the container the processor writes
    a->setStateInformation (blob.getData(), static_cast<int> (blob.getSize()));
    REQUIRE (a->assistantState()["moves"].size() == 1);
    REQUIRE (a->assistantCommand (json (cmd ("accept", 1))));
    CHECK (rawValue (*a, "limit.thresh") == Approx (-4.0f));
    CHECK (rawValue (*a, "limit.on") == 1.0f);
}

TEST_CASE ("assistant: commands and state reads race the analysis thread safely", "[processor][assistant][threads]")
{
    // Run under TSAN: the real analysis thread feeds the capture while this thread plays
    // audio, reads the state and sends commands.
    const JuceScope juce;
    auto p = std::make_unique<Strat3Processor> (true);
    p->setRateAndBufferSizeDetails (kFs, kBlock);
    p->prepareToPlay (kFs, kBlock);
    setParam (*p, "global.target", 1.0f);

    std::mt19937 rng (10);
    std::uniform_real_distribution<float> d (-0.1f, 0.1f);
    juce::AudioBuffer<float> buf (2, kBlock);
    juce::MidiBuffer midi;
    int accepted = 0;
    // At least 600 ms of racing; longer (up to 10 s) on a slow machine until one capture has
    // completed and its move was accepted.
    const auto start = std::chrono::steady_clock::now();
    const auto elapsedMs = [&] { return std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::steady_clock::now() - start).count(); };
    for (int i = 0; elapsedMs() < 600 || (accepted == 0 && elapsedMs() < 10000); ++i)
    {
        for (int c = 0; c < 2; ++c)
            for (int n = 0; n < kBlock; ++n)
                buf.setSample (c, n, d (rng));
        p->processBlock (buf, midi);
        if (i % 40 == 0)
            p->startAssistantCapture (assistant::CaptureKind::Analyse, 0.1);   // 3 frames: completes on the analysis thread
        const auto state = p->assistantState();
        for (int m = 0; m < state["moves"].size(); ++m)
            if (state["moves"][m]["status"].toString() == "pending"
                && p->assistantCommand (json (cmd ("accept", static_cast<int> (state["moves"][m]["id"])))))
                ++accepted;
        juce::MemoryBlock blob;
        if (i % 25 == 0)
            p->getStateInformation (blob);
        std::this_thread::sleep_for (std::chrono::milliseconds (2));
    }
    CHECK (p->assistantRevision() > 0);
    CHECK (accepted >= 1);
    // Which rule fires depends on how much audio each capture saw, so check what must hold
    // for any of them: every switch of every accepted move is set.
    const auto state = p->assistantState();
    int switches = 0;
    for (int m = 0; m < state["moves"].size(); ++m)
    {
        if (state["moves"][m]["status"].toString() != "accepted")
            continue;
        const auto changes = state["moves"][m]["changes"];
        for (int c = 0; c < changes.size(); ++c)
            if (! static_cast<bool> (changes[c]["scaled"]))
            {
                INFO (changes[c]["paramId"].toString());
                CHECK (rawValue (*p, changes[c]["paramId"].toString().toStdString()) == Approx (static_cast<double> (changes[c]["to"])).margin (1e-3));
                ++switches;
            }
    }
    CHECK (switches >= 1);
}
