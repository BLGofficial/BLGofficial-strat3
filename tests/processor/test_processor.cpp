// Strat3Processor: the AudioProcessor wrapper (module 17).
// Contracts tested here:
//   - buses: mono or stereo main (in == out), optional mono/stereo sidechain, nothing else
//   - every mapped APVTS parameter lands in exactly its EngineParams field; the only unmapped
//     ones are stems.* (later module), global.target (assistant), latencyMode and sat.os (config)
//   - latencyMode / sat.os changes stay pending until the message thread re-prepares; the new
//     latency is then reported. Pending compares the *requested* oversampling, so a factor the
//     engine caps by sample rate does not re-prepare forever
//   - state round-trips (params + EXTRA); unusable state keeps the current state
//   - mono: stereoize is ignored; a sidechain bus drives MULTIBAND external
//   - bypass is delayed by the reported latency and stays warm while processing
//   - the analysis tick: loudness meters, gain match, impact map, MASTER FIR redesigns
//   - non-finite flag latches; no allocation on the audio thread; blocks larger than the
//     prepared size are chunked without changing the output; CPU load is reported
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <numbers>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "params/ParameterSpecs.h"
#include "params/StateMigration.h"
#include "processor/ParamBridge.h"
#include "processor/Strat3Processor.h"
#include "support/AllocCounter.h"

using namespace strat3;
using processor::Strat3Processor;
using Catch::Approx;
using Set = juce::AudioChannelSet;

namespace {

constexpr double kFs = 48000.0;

// Every test owns a message manager: the APVTS runs a Timer, and the re-prepare request is an
// AsyncUpdater. Without one, JUCE asserts that the timer thread outlived the event system.
struct JuceScope
{
    juce::ScopedJuceInitialiser_GUI init;
};

std::unique_ptr<Strat3Processor> makeProc() { return std::make_unique<Strat3Processor> (false); }

void setParam (Strat3Processor& p, const std::string& id, float value)
{
    auto* param = p.apvts.getParameter (id);
    REQUIRE (param != nullptr);
    param->setValueNotifyingHost (param->convertTo0to1 (value));
}

float rawValue (Strat3Processor& p, const std::string& id) { return p.apvts.getRawParameterValue (id)->load(); }

juce::AudioProcessor::BusesLayout layout (Set main, Set sidechain = Set::disabled())
{
    juce::AudioProcessor::BusesLayout l;
    l.inputBuses.add (main);
    l.inputBuses.add (sidechain);
    l.outputBuses.add (main);
    return l;
}

std::vector<float> sine (int n, double hz, float amp, double fs = kFs)
{
    std::vector<float> x (static_cast<std::size_t> (n));
    for (int i = 0; i < n; ++i)
        x[static_cast<std::size_t> (i)] = amp * static_cast<float> (std::sin (2.0 * std::numbers::pi * hz * i / fs));
    return x;
}

std::vector<float> noise (int n, float rms, unsigned seed)
{
    std::mt19937 rng (seed);
    const float a = rms * std::sqrt (3.0f);   // uniform [-a, a] has RMS a / sqrt 3
    std::uniform_real_distribution<float> d (-a, a);
    std::vector<float> x (static_cast<std::size_t> (n));
    for (auto& v : x)
        v = d (rng);
    return x;
}

double rmsDb (const std::vector<float>& x, std::size_t from, std::size_t to)
{
    double e = 0.0;
    for (std::size_t i = from; i < to; ++i)
        e += static_cast<double> (x[i]) * static_cast<double> (x[i]);
    return 10.0 * std::log10 (e / static_cast<double> (to - from) + 1e-30);
}

/** Runs channels (in place) through processBlock in blocks of `block`; `chans` = main + sidechain.
 *  Calls analysisTick every `tickEvery` blocks (0 = never). */
void run (Strat3Processor& p, std::vector<std::vector<float>*> chans, int block, int tickEvery = 0, bool bypassed = false)
{
    const int n = static_cast<int> (chans[0]->size());
    juce::MidiBuffer midi;
    std::vector<float*> ptrs (chans.size());
    int count = 0;
    for (int off = 0; off < n; off += block)
    {
        const int len = std::min (block, n - off);
        for (std::size_t c = 0; c < chans.size(); ++c)
            ptrs[c] = chans[c]->data() + off;
        juce::AudioBuffer<float> buf (ptrs.data(), static_cast<int> (ptrs.size()), len);
        if (bypassed)
            p.processBlockBypassed (buf, midi);
        else
            p.processBlock (buf, midi);
        if (tickEvery > 0 && ++count % tickEvery == 0)
            p.analysisTick();
    }
}

void runStereo (Strat3Processor& p, std::vector<float>& l, std::vector<float>& r, int block, int tickEvery = 0)
{
    run (p, { &l, &r }, block, tickEvery);
}

bool allFinite (const std::vector<float>& x)
{
    for (float v : x)
        if (! std::isfinite (v))
            return false;
    return true;
}

// ---- the mapping oracle: parameter ID -> the EngineParams field it must land in.
using Getter = std::function<float (const engine::EngineParams&)>;

float f (bool b) { return b ? 1.0f : 0.0f; }
template <typename E> float f (E e) requires std::is_enum_v<E> { return static_cast<float> (static_cast<int> (e)); }
float f (float v) { return v; }

std::vector<std::pair<std::string, Getter>> fieldTable()
{
    std::vector<std::pair<std::string, Getter>> t;
    auto add = [&] (std::string id, Getter g) { t.emplace_back (std::move (id), std::move (g)); };
    using P = const engine::EngineParams&;

    add ("global.inTrim", [] (P p) { return f (p.inTrimDb); });
    add ("global.outTrim", [] (P p) { return f (p.outTrimDb); });
    add ("global.gainMatch", [] (P p) { return f (p.gainMatch); });
    add ("global.delta", [] (P p) { return f (p.globalDelta); });

    const std::array<std::string, engine::kDepthLayers> layers { "eq", "low", "mb", "width", "sat", "limit" };
    for (std::size_t i = 0; i < layers.size(); ++i)
    {
        add (layers[i] + ".on", [i] (P p) { return f (p.layer[i].on); });
        add (layers[i] + ".depth", [i] (P p) { return f (p.layer[i].depth); });
        add (layers[i] + ".delta", [i] (P p) { return f (p.layer[i].delta); });
    }

    for (std::size_t b = 0; b < 8; ++b)
    {
        const std::string base = "eq.b" + std::to_string (b + 1);
        add (base + ".on", [b] (P p) { return f (p.eq[b].on); });
        add (base + ".type", [b] (P p) { return f (p.eq[b].type); });
        add (base + ".freq", [b] (P p) { return f (p.eq[b].freq); });
        add (base + ".gain", [b] (P p) { return f (p.eq[b].gainDb); });
        add (base + ".q", [b] (P p) { return f (p.eq[b].q); });
        add (base + ".ms", [b] (P p) { return f (p.eq[b].channel); });
        add (base + ".dyn", [b] (P p) { return f (p.eq[b].dyn); });
        add (base + ".thresh", [b] (P p) { return f (p.eq[b].threshDb); });
        add (base + ".range", [b] (P p) { return f (p.eq[b].rangeDb); });
        add (base + ".atk", [b] (P p) { return f (p.eq[b].attackMs); });
        add (base + ".rel", [b] (P p) { return f (p.eq[b].releaseMs); });
    }
    add ("eq.phase", [] (P p) { return f (p.eqPhase); });

    add ("xover.f1", [] (P p) { return f (p.xover.f1); });
    add ("xover.f2", [] (P p) { return f (p.xover.f2); });
    add ("xover.f3", [] (P p) { return f (p.xover.f3); });

    add ("low.xover", [] (P p) { return f (p.low.xoverHz); });
    add ("low.punch", [] (P p) { return f (p.low.punch); });
    add ("low.sustain", [] (P p) { return f (p.low.sustain); });
    add ("low.monoOn", [] (P p) { return f (p.low.monoOn); });
    add ("low.monoFreq", [] (P p) { return f (p.low.monoHz); });
    add ("low.tight", [] (P p) { return f (p.low.tight); });

    for (std::size_t b = 0; b < 4; ++b)
    {
        const std::string base = "mb.b" + std::to_string (b + 1);
        add (base + ".thresh", [b] (P p) { return f (p.mb.bands[b].threshDb); });
        add (base + ".ratio", [b] (P p) { return f (p.mb.bands[b].ratio); });
        add (base + ".dir", [b] (P p) { return f (p.mb.bands[b].dir); });
        add (base + ".knee", [b] (P p) { return f (p.mb.bands[b].kneeDb); });
        add (base + ".atk", [b] (P p) { return f (p.mb.bands[b].attackMs); });
        add (base + ".rel", [b] (P p) { return f (p.mb.bands[b].releaseMs); });
        add (base + ".det", [b] (P p) { return f (p.mb.bands[b].detector); });
        add (base + ".mix", [b] (P p) { return f (p.mb.bands[b].mixPercent); });
    }
    add ("mb.scSource", [] (P p) { return f (p.mb.sidechain); });
    add ("mb.scHpOn", [] (P p) { return f (p.mb.sidechainHpOn); });
    add ("mb.scHpFreq", [] (P p) { return f (p.mb.sidechainHpHz); });

    for (std::size_t b = 0; b < 4; ++b)
        add ("width.b" + std::to_string (b + 1), [b] (P p) { return f (p.width.widthPercent[b]); });
    add ("width.stereoize", [] (P p) { return f (p.width.stereoizePercent); });
    add ("width.monoGuard", [] (P p) { return f (p.width.monoGuard); });

    for (std::size_t b = 0; b < 4; ++b)
    {
        const std::string base = "sat.b" + std::to_string (b + 1);
        add (base + ".model", [b] (P p) { return f (p.sat.bands[b].model); });
        add (base + ".drive", [b] (P p) { return f (p.sat.bands[b].driveDb); });
        add (base + ".mix", [b] (P p) { return f (p.sat.bands[b].mixPercent); });
    }

    add ("limit.style", [] (P p) { return f (p.limit.style); });
    add ("limit.thresh", [] (P p) { return f (p.limit.threshDb); });
    add ("limit.release", [] (P p) { return f (p.limit.releaseMs); });
    add ("limit.link", [] (P p) { return f (p.limit.linkPercent); });
    add ("limit.transient", [] (P p) { return f (p.limit.transientPercent); });

    add ("ceiling.on", [] (P p) { return f (p.ceilingOn); });
    add ("ceiling.dBTP", [] (P p) { return f (p.ceiling.ceilingDb); });
    add ("ceiling.dither", [] (P p) { return f (p.ceiling.dither); });
    add ("ceiling.bits", [] (P p) { return static_cast<float> (p.ceiling.bits); });
    return t;
}

bool isUnmapped (const std::string& id)
{
    return id.rfind ("stems.", 0) == 0 || id == "global.target" || id == "global.latencyMode" || id == "sat.os";
}

/** A value inside the range that differs from the default, in plain units. */
float probeValue (const params::ParamSpec& s)
{
    switch (s.kind)
    {
        case params::Kind::Bool:   return s.def > 0.5f ? 0.0f : 1.0f;
        case params::Kind::Choice: return s.def < s.max ? s.max : 0.0f;
        case params::Kind::Float:  break;
    }
    const float v = s.min + 0.73f * (s.max - s.min);
    return std::abs (v - s.def) > 1e-3f * (s.max - s.min) ? v : s.min + 0.31f * (s.max - s.min);
}

} // namespace

// ------------------------------------------------------------------- buses

TEST_CASE ("processor: bus layouts are mono or stereo main with an optional sidechain", "[processor][buses]")
{
    const JuceScope juce;
    auto p = makeProc();
    CHECK (p->isBusesLayoutSupported (layout (Set::stereo())));
    CHECK (p->isBusesLayoutSupported (layout (Set::mono())));
    CHECK (p->isBusesLayoutSupported (layout (Set::stereo(), Set::stereo())));
    CHECK (p->isBusesLayoutSupported (layout (Set::stereo(), Set::mono())));
    CHECK (p->isBusesLayoutSupported (layout (Set::mono(), Set::stereo())));

    auto mismatched = layout (Set::stereo());
    mismatched.outputBuses.getReference (0) = Set::mono();
    CHECK_FALSE (p->isBusesLayoutSupported (mismatched));
    CHECK_FALSE (p->isBusesLayoutSupported (layout (Set::create5point1())));
    CHECK_FALSE (p->isBusesLayoutSupported (layout (Set::disabled())));
    CHECK_FALSE (p->isBusesLayoutSupported (layout (Set::stereo(), Set::create5point1())));

    // The default instance: stereo in/out, sidechain bus present but off.
    CHECK (p->getTotalNumInputChannels() == 2);
    CHECK (p->getTotalNumOutputChannels() == 2);
    REQUIRE (p->getBusCount (true) == 2);
    CHECK_FALSE (p->getBus (true, 1)->isEnabled());
}

// ------------------------------------------------------------------- mapping

TEST_CASE ("processor: every parameter is mapped or explicitly unmapped", "[processor][mapping]")
{
    const JuceScope juce;
    const auto table = fieldTable();
    std::set<std::string> mapped;
    for (const auto& [id, g] : table)
    {
        INFO (id);
        CHECK (mapped.insert (id).second);   // no duplicates
    }
    std::set<std::string> known;
    for (const auto& s : params::allSpecs())
    {
        known.insert (s.id);
        INFO (s.id);
        CHECK ((mapped.count (s.id) == 1) != isUnmapped (s.id));
    }
    for (const auto& id : mapped)
    {
        INFO (id);
        CHECK (known.count (id) == 1);
    }
}

TEST_CASE ("processor: each mapped parameter lands in its own EngineParams field", "[processor][mapping]")
{
    const JuceScope juce;
    auto p = makeProc();
    const auto bridge = processor::makeParamBridge (p->apvts);
    const auto table = fieldTable();
    const auto& specs = params::allSpecs();

    for (const auto& [id, get] : table)
    {
        const auto it = std::find_if (specs.begin(), specs.end(), [&] (const auto& s) { return s.id == id; });
        REQUIRE (it != specs.end());
        // Through the normalised path first: a log range does not round-trip a default exactly.
        setParam (*p, id, it->def);
        const auto before = processor::readParams (bridge);
        const float defaultField = get (before);

        setParam (*p, id, probeValue (*it));
        const float stored = rawValue (*p, id);
        float expected = stored;
        if (id == "ceiling.bits")
            expected = stored < 0.5f ? 16.0f : 24.0f;   // choice index -> bit depth
        const auto after = processor::readParams (bridge);
        INFO (id << " stored " << stored);
        CHECK (get (after) == Approx (expected).margin (1e-4));
        CHECK (get (after) != Approx (defaultField).margin (1e-4));   // the probe moved it
        CHECK_FALSE (after == before);

        setParam (*p, id, it->def);
        CHECK (processor::readParams (bridge) == before);   // back to default: nothing else moved
    }
}

TEST_CASE ("processor: default parameters map to the spec defaults", "[processor][mapping]")
{
    const JuceScope juce;
    auto p = makeProc();
    const auto e = processor::readParams (processor::makeParamBridge (p->apvts));
    for (const auto& l : e.layer)
    {
        CHECK_FALSE (l.on);
        CHECK (l.depth == 100.0f);
        CHECK_FALSE (l.delta);
    }
    CHECK (e.ceilingOn);
    CHECK (e.ceiling.bits == 24);
    CHECK (e.ceiling.ceilingDb == Approx (-1.0f));
    CHECK (e.eqPhase == 0.0f);   // spec default, not the EngineParams default (100)
    CHECK (e.width.monoGuard);
    CHECK (e.mb.bands[3].ratio == Approx (1.0f));
}

TEST_CASE ("processor: mono forces stereoize off and nothing else", "[processor][mapping]")
{
    const JuceScope juce;
    auto p = makeProc();
    const auto bridge = processor::makeParamBridge (p->apvts);
    setParam (*p, "width.stereoize", 80.0f);
    setParam (*p, "width.b2", 40.0f);
    const auto stereo = processor::readParams (bridge, false);
    auto mono = processor::readParams (bridge, true);
    CHECK (stereo.width.stereoizePercent == Approx (80.0f));
    CHECK (mono.width.stereoizePercent == 0.0f);
    mono.width.stereoizePercent = stereo.width.stereoizePercent;
    CHECK (mono == stereo);
}

TEST_CASE ("processor: latencyMode and sat.os choices map to their config values", "[processor][mapping]")
{
    const JuceScope juce;
    auto p = makeProc();
    const auto bridge = processor::makeParamBridge (p->apvts);
    CHECK (processor::readLatencyMode (bridge) == engine::LatencyMode::Mix);   // default
    CHECK (processor::readSatOs (bridge) == 2);
    const std::array<engine::LatencyMode, 3> modes { engine::LatencyMode::Track, engine::LatencyMode::Mix, engine::LatencyMode::Master };
    for (int i = 0; i < 3; ++i)
    {
        setParam (*p, "global.latencyMode", static_cast<float> (i));
        CHECK (processor::readLatencyMode (bridge) == modes[static_cast<std::size_t> (i)]);
        setParam (*p, "sat.os", static_cast<float> (i));
        CHECK (processor::readSatOs (bridge) == (1 << i));
    }
}

// ------------------------------------------------------------------- latency and re-prepare

TEST_CASE ("processor: prepareToPlay uses the mode parameter and reports the engine latency", "[processor][latency]")
{
    const JuceScope juce;
    for (int mode = 0; mode < 3; ++mode)
    {
        auto p = makeProc();
        setParam (*p, "global.latencyMode", static_cast<float> (mode));
        p->prepareToPlay (kFs, 512);
        INFO ("mode " << mode);
        CHECK (static_cast<int> (p->preparedMode()) == mode);
        CHECK (p->getLatencySamples() == engine::latencySamples (p->getEngine()));
        CHECK_FALSE (p->configPending());
        if (mode == 2)
            CHECK (p->getLatencySamples() > 4000);   // FIR EQ + FIR split: ~100 ms
        CHECK (p->getTailLengthSeconds() == Approx (p->getLatencySamples() / kFs));
    }
}

TEST_CASE ("processor: a mode change stays pending until the message thread re-prepares", "[processor][latency]")
{
    const JuceScope juce;
    auto p = makeProc();
    p->prepareToPlay (kFs, 512);
    const int mixLatency = p->getLatencySamples();
    CHECK (p->preparedMode() == engine::LatencyMode::Mix);

    setParam (*p, "global.latencyMode", 2.0f);
    CHECK (p->configPending());
    std::vector<float> l (2048, 0.1f), r (2048, 0.1f);
    runStereo (*p, l, r, 512);   // the audio thread never re-prepares
    CHECK (p->preparedMode() == engine::LatencyMode::Mix);
    CHECK (p->getLatencySamples() == mixLatency);

    p->applyPendingConfig();
    CHECK_FALSE (p->configPending());
    CHECK (p->preparedMode() == engine::LatencyMode::Master);
    CHECK (p->getLatencySamples() == engine::latencySamples (p->getEngine()));
    CHECK (p->getLatencySamples() > mixLatency);

    p->applyPendingConfig();   // nothing pending: no change
    CHECK (p->preparedMode() == engine::LatencyMode::Master);
}

TEST_CASE ("processor: sat.os re-prepares; pending compares the requested factor, not the capped one", "[processor][latency]")
{
    const JuceScope juce;
    auto p = makeProc();
    p->prepareToPlay (96000.0, 512);
    CHECK (p->preparedSatOs() == 2);
    CHECK (engine::effectiveSatOs (p->getEngine()) == 2);

    setParam (*p, "sat.os", 2.0f);   // 4x
    CHECK (p->configPending());
    p->applyPendingConfig();
    CHECK (p->preparedSatOs() == 4);
    CHECK (engine::effectiveSatOs (p->getEngine()) == 2);   // 96k * 4 > 192k: capped
    CHECK_FALSE (p->configPending());                        // and it does not loop

    setParam (*p, "sat.os", 0.0f);
    p->applyPendingConfig();
    CHECK (engine::effectiveSatOs (p->getEngine()) == 1);
    CHECK (p->getLatencySamples() == engine::latencySamples (p->getEngine()));
}

TEST_CASE ("processor: the analysis tick asks the message thread for a re-prepare", "[processor][latency]")
{
    const JuceScope juce;
    auto p = makeProc();
    p->prepareToPlay (kFs, 512);
    p->analysisTick();
    CHECK_FALSE (p->reprepareRequested());

    setParam (*p, "global.latencyMode", 0.0f);
    p->analysisTick();
    CHECK (p->reprepareRequested());
    CHECK (p->preparedMode() == engine::LatencyMode::Mix);   // the tick itself never re-prepares
}

TEST_CASE ("processor: nothing is processed before prepareToPlay or after releaseResources", "[processor][latency]")
{
    const JuceScope juce;
    auto p = makeProc();
    std::vector<float> l (256, 0.5f), r (256, 0.5f);
    runStereo (*p, l, r, 256);
    CHECK (rmsDb (l, 0, l.size()) < -200.0);
    CHECK (rmsDb (r, 0, r.size()) < -200.0);

    p->prepareToPlay (kFs, 256);
    p->releaseResources();
    std::fill (l.begin(), l.end(), 0.5f);
    std::fill (r.begin(), r.end(), 0.5f);
    runStereo (*p, l, r, 256);
    CHECK (rmsDb (l, 0, l.size()) < -200.0);
    setParam (*p, "global.latencyMode", 2.0f);
    p->applyPendingConfig();   // not prepared: must not prepare behind the host's back
    std::fill (l.begin(), l.end(), 0.5f);
    std::fill (r.begin(), r.end(), 0.5f);
    runStereo (*p, l, r, 256);
    CHECK (rmsDb (l, 0, l.size()) < -200.0);
    CHECK (p->preparedMode() == engine::LatencyMode::Mix);
}

// ------------------------------------------------------------------- state

TEST_CASE ("processor: state round-trips parameters and EXTRA", "[processor][state]")
{
    const JuceScope juce;
    auto a = makeProc();
    setParam (*a, "global.outTrim", 3.5f);
    setParam (*a, "eq.b2.freq", 250.0f);
    setParam (*a, "global.latencyMode", 2.0f);
    setParam (*a, "sat.os", 2.0f);
    setParam (*a, "limit.on", 1.0f);
    auto order = params::defaultLayerOrder();
    std::swap (order[1], order[2]);
    a->extraState = params::withLayerOrder (a->extraState, order);

    juce::MemoryBlock blob;
    a->getStateInformation (blob);
    REQUIRE (blob.getSize() > 0);

    auto b = makeProc();
    b->setStateInformation (blob.getData(), static_cast<int> (blob.getSize()));
    for (const auto& s : params::allSpecs())
    {
        INFO (s.id);
        CHECK (rawValue (*b, s.id) == rawValue (*a, s.id));
    }
    CHECK (params::decodeLayerOrder (b->extraState) == order);
}

TEST_CASE ("processor: unusable state keeps the current state", "[processor][state]")
{
    const JuceScope juce;
    auto p = makeProc();
    setParam (*p, "global.outTrim", -4.0f);
    auto order = params::defaultLayerOrder();
    std::swap (order[0], order[3]);
    p->extraState = params::withLayerOrder (p->extraState, order);

    SECTION ("garbage bytes")
    {
        const char junk[] = "definitely not a STRAT3 state";
        p->setStateInformation (junk, static_cast<int> (sizeof (junk)));
    }
    SECTION ("empty")
    {
        p->setStateInformation (nullptr, 0);
    }
    SECTION ("written by a newer build")
    {
        auto other = makeProc();
        setParam (*other, "global.outTrim", 9.0f);
        auto env = params::wrapState (other->apvts.copyState(), other->extraState);
        env.setProperty (params::kSchemaProperty, params::kCurrentSchema + 1, nullptr);
        juce::MemoryBlock blob;
        juce::AudioProcessor::copyXmlToBinary (*env.createXml(), blob);
        p->setStateInformation (blob.getData(), static_cast<int> (blob.getSize()));
    }
    SECTION ("a bare APVTS tree without the envelope")
    {
        auto other = makeProc();
        setParam (*other, "global.outTrim", 9.0f);
        juce::MemoryBlock blob;
        juce::AudioProcessor::copyXmlToBinary (*other->apvts.copyState().createXml(), blob);
        p->setStateInformation (blob.getData(), static_cast<int> (blob.getSize()));
    }
    CHECK (rawValue (*p, "global.outTrim") == Approx (-4.0f));
    CHECK (params::decodeLayerOrder (p->extraState) == order);
}

TEST_CASE ("processor: loading a state with another latency mode leaves a pending re-prepare", "[processor][state]")
{
    const JuceScope juce;
    auto a = makeProc();
    setParam (*a, "global.latencyMode", 2.0f);
    juce::MemoryBlock blob;
    a->getStateInformation (blob);

    auto b = makeProc();
    b->prepareToPlay (kFs, 512);
    b->setStateInformation (blob.getData(), static_cast<int> (blob.getSize()));
    CHECK (b->configPending());
    CHECK (b->reprepareRequested());   // the host may never touch the processor again otherwise
    CHECK (b->preparedMode() == engine::LatencyMode::Mix);
    b->applyPendingConfig();
    CHECK (b->preparedMode() == engine::LatencyMode::Master);
}

// ------------------------------------------------------------------- audio paths

TEST_CASE ("processor: a mono bus ignores stereoize", "[processor][audio]")
{
    const JuceScope juce;
    auto make = [] (float stereoize)
    {
        auto p = makeProc();
        REQUIRE (p->setBusesLayout (layout (Set::mono())));
        setParam (*p, "width.on", 1.0f);
        setParam (*p, "width.stereoize", stereoize);
        setParam (*p, "width.monoGuard", 0.0f);
        p->prepareToPlay (kFs, 512);
        return p;
    };
    auto withS = make (100.0f), without = make (0.0f);
    auto a = noise (24000, 0.1f, 7), b = a;
    run (*withS, { &a }, 512);
    run (*without, { &b }, 512);
    CHECK (a == b);
    CHECK (allFinite (a));
    CHECK (rmsDb (a, 12000, 24000) > -30.0);   // it did pass audio
}

TEST_CASE ("processor: a stereo instance with stereoize on does add side (control for the mono test)", "[processor][audio]")
{
    const JuceScope juce;
    auto p = makeProc();
    setParam (*p, "width.on", 1.0f);
    setParam (*p, "width.stereoize", 100.0f);
    setParam (*p, "width.monoGuard", 0.0f);
    p->prepareToPlay (kFs, 512);
    auto l = noise (24000, 0.1f, 7), r = l;
    runStereo (*p, l, r, 512);
    std::vector<float> side (l.size());
    for (std::size_t i = 0; i < l.size(); ++i)
        side[i] = 0.5f * (l[i] - r[i]);
    CHECK (rmsDb (side, 12000, 24000) > -40.0);
}

TEST_CASE ("processor: the sidechain bus drives MULTIBAND external", "[processor][audio]")
{
    const JuceScope juce;
    auto make = [] (int source)
    {
        auto p = makeProc();
        REQUIRE (p->setBusesLayout (layout (Set::stereo(), Set::stereo())));
        setParam (*p, "mb.on", 1.0f);
        setParam (*p, "mb.scSource", static_cast<float> (source));
        for (int b = 1; b <= 4; ++b)
        {
            setParam (*p, "mb.b" + std::to_string (b) + ".thresh", -40.0f);
            setParam (*p, "mb.b" + std::to_string (b) + ".ratio", 20.0f);
        }
        p->prepareToPlay (kFs, 512);
        return p;
    };
    const int n = 48000;
    auto render = [&] (Strat3Processor& p, bool loudSidechain)
    {
        auto l = sine (n, 1000.0, 0.01f), r = l;   // -40 dBFS peak: at threshold on its own
        auto sc = loudSidechain ? noise (n, 0.5f, 3) : std::vector<float> (static_cast<std::size_t> (n), 0.0f);
        auto sc2 = sc;
        run (p, { &l, &r, &sc, &sc2 }, 512);
        return l;
    };
    auto quiet = make (1), loud = make (1);
    const auto outQuiet = render (*quiet, false);
    const auto outLoud = render (*loud, true);
    CHECK (rmsDb (outQuiet, 24000, 48000) - rmsDb (outLoud, 24000, 48000) > 10.0);

    // Internal: the sidechain bus is ignored.
    auto i1 = make (0), i2 = make (0);
    CHECK (render (*i1, false) == render (*i2, true));
}

TEST_CASE ("processor: both sidechain channels reach the detector, also when chunked", "[processor][audio]")
{
    const JuceScope juce;
    auto make = []
    {
        auto p = makeProc();
        REQUIRE (p->setBusesLayout (layout (Set::stereo(), Set::stereo())));
        setParam (*p, "mb.on", 1.0f);
        setParam (*p, "mb.scSource", 1.0f);
        for (int b = 1; b <= 4; ++b)
        {
            setParam (*p, "mb.b" + std::to_string (b) + ".thresh", -40.0f);
            setParam (*p, "mb.b" + std::to_string (b) + ".ratio", 20.0f);
        }
        p->prepareToPlay (kFs, 256);
        return p;
    };
    const int n = 48000;

    // Key only in the right sidechain channel: it must still compress.
    {
        auto keyed = make(), unkeyed = make();
        auto l1 = sine (n, 1000.0, 0.01f), r1 = l1, l2 = l1, r2 = l1;
        std::vector<float> silent (static_cast<std::size_t> (n), 0.0f), s2 = silent, s3 = silent;
        auto key = noise (n, 0.5f, 9);
        run (*keyed, { &l1, &r1, &silent, &key }, 256);
        run (*unkeyed, { &l2, &r2, &s2, &s3 }, 256);
        CHECK (rmsDb (l2, 24000, 48000) - rmsDb (l1, 24000, 48000) > 10.0);
    }
    // A host block 4x the prepared size: the sidechain is chunked with the main audio.
    {
        auto big = make(), small = make();
        auto l1 = sine (n, 1000.0, 0.01f), r1 = l1, l2 = l1, r2 = l1;
        // Key bursts, so a misaligned sidechain changes the gain-reduction timing.
        auto key = noise (n, 0.5f, 10);
        for (int i = 0; i < n; ++i)
            if ((i / 700) % 2 == 0)
                key[static_cast<std::size_t> (i)] = 0.0f;
        auto kL1 = key, kR1 = key, kL2 = key, kR2 = key;
        run (*big, { &l1, &r1, &kL1, &kR1 }, 1024);
        run (*small, { &l2, &r2, &kL2, &kR2 }, 128);
        CHECK (l1 == l2);
        CHECK (r1 == r2);
    }
}

TEST_CASE ("processor: a mono sidechain feeds both detector channels", "[processor][audio]")
{
    const JuceScope juce;
    auto make = [] (Set sc)
    {
        auto p = makeProc();
        REQUIRE (p->setBusesLayout (layout (Set::stereo(), sc)));
        setParam (*p, "mb.on", 1.0f);
        setParam (*p, "mb.scSource", 1.0f);
        for (int b = 1; b <= 4; ++b)
        {
            setParam (*p, "mb.b" + std::to_string (b) + ".thresh", -40.0f);
            setParam (*p, "mb.b" + std::to_string (b) + ".ratio", 20.0f);
        }
        p->prepareToPlay (kFs, 512);
        return p;
    };
    const int n = 24000;
    auto mono = make (Set::mono()), stereo = make (Set::stereo());
    auto l1 = sine (n, 1000.0, 0.01f), r1 = l1, sc = noise (n, 0.5f, 5);
    auto l2 = l1, r2 = l1, scL = sc, scR = sc;
    run (*mono, { &l1, &r1, &sc }, 512);
    run (*stereo, { &l2, &r2, &scL, &scR }, 512);
    CHECK (l1 == l2);
    CHECK (r1 == r2);
}

TEST_CASE ("processor: bypass is delayed by the reported latency and stays warm", "[processor][audio]")
{
    const JuceScope juce;
    auto p = makeProc();
    setParam (*p, "global.latencyMode", 2.0f);
    p->prepareToPlay (kFs, 512);
    const int lat = p->getLatencySamples();
    REQUIRE (lat > 512);

    // Processed for a while, then bypassed: the bypassed output is the input from `lat`
    // samples ago, also across the switch (no stale or zeroed delay line).
    const int n = 4 * 4096;
    std::vector<float> in (static_cast<std::size_t> (n));
    for (int i = 0; i < n; ++i)
        in[static_cast<std::size_t> (i)] = static_cast<float> ((i % 1000) + 1) * 1e-4f;
    auto l = in, r = in;
    const int switchAt = n / 2;
    std::vector<float> l1 (l.begin(), l.begin() + switchAt), r1 (r.begin(), r.begin() + switchAt);
    std::vector<float> l2 (l.begin() + switchAt, l.end()), r2 (r.begin() + switchAt, r.end());
    run (*p, { &l1, &r1 }, 512);
    run (*p, { &l2, &r2 }, 512, 0, true);
    for (int i = 0; i < n - switchAt; ++i)
    {
        const int src = switchAt + i - lat;
        const float expect = src >= 0 ? in[static_cast<std::size_t> (src)] : 0.0f;
        if (l2[static_cast<std::size_t> (i)] != expect || r2[static_cast<std::size_t> (i)] != expect)
        {
            FAIL ("bypass sample " << i << " = " << l2[static_cast<std::size_t> (i)] << ", expected " << expect);
        }
    }
}

TEST_CASE ("processor: a block larger than the prepared size is chunked without changing the output", "[processor][audio]")
{
    const JuceScope juce;
    auto make = [] (Set main)
    {
        auto p = makeProc();
        REQUIRE (p->setBusesLayout (layout (main)));
        setParam (*p, "eq.on", 1.0f);
        setParam (*p, "eq.b4.on", 1.0f);
        setParam (*p, "eq.b4.gain", 6.0f);
        setParam (*p, "limit.on", 1.0f);
        setParam (*p, "limit.thresh", -6.0f);
        p->prepareToPlay (kFs, 256);
        return p;
    };
    for (auto main : { Set::stereo(), Set::mono() })
    {
        auto big = make (main), small = make (main);
        auto l1 = noise (6000, 0.2f, 11), r1 = noise (6000, 0.2f, 12);
        auto l2 = l1, r2 = r1;
        if (main == Set::stereo())
        {
            runStereo (*big, l1, r1, 1000);   // 4 x the prepared block size
            runStereo (*small, l2, r2, 100);
            CHECK (r1 == r2);
        }
        else
        {
            run (*big, { &l1 }, 1000);
            run (*small, { &l2 }, 100);
        }
        CHECK (l1 == l2);
        CHECK (allFinite (l1));
    }
}

TEST_CASE ("processor: a non-finite input latches the flag until cleared", "[processor][audio]")
{
    const JuceScope juce;
    auto p = makeProc();
    p->prepareToPlay (kFs, 512);
    auto l = sine (512, 440.0, 0.5f), r = l;
    l[10] = std::numeric_limits<float>::quiet_NaN();
    runStereo (*p, l, r, 512);
    CHECK (rmsDb (l, 0, 512) < -200.0);   // the engine muted the block
    CHECK (p->meters().nonFinite);

    for (int k = 0; k < 10; ++k)
    {
        auto a = sine (512, 440.0, 0.5f), b = a;
        runStereo (*p, a, b, 512);
        CHECK (allFinite (a));
    }
    CHECK (p->meters().nonFinite);   // latched
    p->clearNonFiniteFlag();
    CHECK_FALSE (p->meters().nonFinite);
}

TEST_CASE ("processor: the audio thread does not allocate", "[processor][audio][realtime]")
{
    const JuceScope juce;
    for (auto main : { Set::stereo(), Set::mono() })
    {
        auto p = makeProc();
        REQUIRE (p->setBusesLayout (layout (main, Set::stereo())));
        setParam (*p, "global.latencyMode", 2.0f);
        setParam (*p, "eq.on", 1.0f);
        setParam (*p, "mb.on", 1.0f);
        setParam (*p, "mb.scSource", 1.0f);
        setParam (*p, "limit.on", 1.0f);
        p->prepareToPlay (kFs, 512);
        const auto bridge = processor::makeParamBridge (p->apvts);

        const int chans = p->getTotalNumInputChannels();
        juce::AudioBuffer<float> buf (chans, 1024);   // 2 x the prepared size: chunked
        for (int c = 0; c < chans; ++c)
            for (int i = 0; i < 1024; ++i)
                buf.setSample (c, i, 0.1f * std::sin (0.05f * static_cast<float> (i + c)));
        juce::MidiBuffer midi;

        test::ScopedAllocCounter counter;
        for (int k = 0; k < 8; ++k)
            p->processBlock (buf, midi);
        p->processBlockBypassed (buf, midi);
        auto e = processor::readParams (bridge);
        CHECK (counter.count() == 0);
        CHECK (e.layer[engine::kEq].on);
    }
}

TEST_CASE ("processor: CPU load is reported as a fraction of the block duration", "[processor][audio]")
{
    const JuceScope juce;
    auto p = makeProc();
    p->prepareToPlay (kFs, 512);
    CHECK (p->meters().cpuLoad == 0.0f);
    auto l = noise (48000, 0.1f, 1), r = l;
    const auto t0 = std::chrono::steady_clock::now();
    runStereo (*p, l, r, 512);
    const double wall = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
    // Measured against the test's own clock (1 s of audio), not a fixed bound: a sanitizer
    // build really is slower than real time. The smoothed reading tracks the recent blocks.
    const double load = static_cast<double> (p->meters().cpuLoad);
    CHECK (load > 0.0);
    CHECK (load > 0.3 * wall);
    CHECK (load < 3.0 * wall);
}

// ------------------------------------------------------------------- analysis tick

TEST_CASE ("processor: loudness meters follow the output trim", "[processor][analysis]")
{
    const JuceScope juce;
    auto p = makeProc();
    setParam (*p, "global.outTrim", -6.0f);
    p->prepareToPlay (kFs, 512);
    auto l = sine (4 * 48000, 1000.0, 0.1f), r = l;
    runStereo (*p, l, r, 512, 3);
    p->analysisTick();
    const auto m = p->meters();
    CHECK (m.inputShortTermLufs > -40.0f);
    CHECK (m.shortTermLufs - m.inputShortTermLufs == Approx (-6.0f).margin (0.2));
    CHECK (m.momentaryLufs == Approx (m.shortTermLufs).margin (0.3));
    CHECK (m.integratedLufs == Approx (m.shortTermLufs).margin (0.5));
}

TEST_CASE ("processor: gain match converges to the input loudness and holds through silence", "[processor][analysis]")
{
    const JuceScope juce;
    auto p = makeProc();
    setParam (*p, "global.outTrim", 6.0f);
    setParam (*p, "global.gainMatch", 1.0f);
    p->prepareToPlay (kFs, 512);
    const int n = 6 * 48000;
    auto in = noise (n, 0.1f, 21);
    auto l = in, r = in;
    runStereo (*p, l, r, 512, 3);
    CHECK (p->meters().gainMatchDb == Approx (-6.0f).margin (0.3));
    // The output (gain match included) is back at the input level. Latency is ~0.2 ms in MIX;
    // a 1 s window makes that irrelevant.
    CHECK (rmsDb (l, n - 48000, n) - rmsDb (in, n - 48000, n) == Approx (0.0).margin (0.5));

    std::vector<float> sl (4 * 48000, 0.0f), sr = sl;
    runStereo (*p, sl, sr, 512, 3);
    CHECK (p->meters().gainMatchDb == Approx (-6.0f).margin (0.3));   // gated: kept

    setParam (*p, "global.gainMatch", 0.0f);
    p->analysisTick();
    CHECK (p->meters().gainMatchDb == 0.0f);
}

TEST_CASE ("processor: gain match off never sets an offset", "[processor][analysis]")
{
    const JuceScope juce;
    auto p = makeProc();
    setParam (*p, "global.outTrim", 6.0f);
    p->prepareToPlay (kFs, 512);
    auto in = noise (4 * 48000, 0.1f, 22);
    auto l = in, r = in;
    runStereo (*p, l, r, 512, 3);
    CHECK (p->meters().gainMatchDb == 0.0f);
    CHECK (rmsDb (l, 96000, 192000) - rmsDb (in, 96000, 192000) == Approx (6.0).margin (0.3));
}

TEST_CASE ("processor: the impact map shows an EQ boost on the EQ row only", "[processor][analysis]")
{
    const JuceScope juce;
    auto p = makeProc();
    setParam (*p, "eq.on", 1.0f);
    setParam (*p, "eq.b1.on", 1.0f);
    setParam (*p, "eq.b1.freq", 1000.0f);
    setParam (*p, "eq.b1.gain", 6.0f);
    setParam (*p, "eq.b1.q", 1.0f);
    p->prepareToPlay (kFs, 512);
    auto l = noise (3 * 48000, 0.05f, 31), r = noise (3 * 48000, 0.05f, 32);
    runStereo (*p, l, r, 512, 3);

    analysis::ImpactFrame frame;
    REQUIRE (p->readImpact (frame));
    CHECK (frame.layers == engine::kRows);
    constexpr int k1k = 34;   // 1 kHz band
    CHECK (frame.gainDb[engine::kRowEq][k1k] == Approx (6.0f).margin (1.0));
    CHECK (std::abs (frame.gainDb[engine::kRowLow][k1k]) < 0.5f);
    CHECK (std::abs (frame.gainDb[engine::kRowLimit][k1k]) < 0.5f);
}

TEST_CASE ("processor: impact rows compare time-aligned windows (MASTER, gated noise)", "[processor][analysis]")
{
    const JuceScope juce;
    // Every layer off: each row must read ~0 dB. The gate period equals the tick period, so
    // every frame sees the same gate phase; a row whose before-window is not delayed by that
    // row's latency (~50 ms for the FIRs, about half a period) compares "on" with "off".
    auto p = makeProc();
    setParam (*p, "global.latencyMode", 2.0f);
    p->prepareToPlay (kFs, 512);
    constexpr int period = 4800, block = 480;
    const int n = 4 * 48000;
    auto l = noise (n, 0.1f, 41), r = noise (n, 0.1f, 42);
    for (int i = 0; i < n; ++i)
        if ((i % period) >= period / 2)
            l[static_cast<std::size_t> (i)] = r[static_cast<std::size_t> (i)] = 0.0f;
    run (*p, { &l, &r }, block, period / block);

    analysis::ImpactFrame frame;
    REQUIRE (p->readImpact (frame));
    constexpr int k1k = 34;
    for (int row = 0; row < engine::kRows; ++row)
    {
        INFO ("row " << row << " latency " << engine::rowLatencies (p->getEngine())[static_cast<std::size_t> (row)]);
        CHECK (std::abs (frame.gainDb[static_cast<std::size_t> (row)][k1k]) < 1.0f);
    }
}

TEST_CASE ("processor: in MASTER the tick redesigns the FIR EQ", "[processor][analysis]")
{
    const JuceScope juce;
    auto p = makeProc();
    setParam (*p, "global.latencyMode", 2.0f);
    setParam (*p, "eq.on", 1.0f);
    setParam (*p, "eq.b1.on", 1.0f);
    setParam (*p, "eq.b1.freq", 1000.0f);
    setParam (*p, "eq.b1.q", 1.0f);
    p->prepareToPlay (kFs, 512);
    setParam (*p, "eq.b1.gain", 6.0f);   // after prepare: only a redesign can apply it

    auto in = sine (48000, 1000.0, 0.05f);
    auto l = in, r = in;
    runStereo (*p, l, r, 512);   // no tick
    CHECK (rmsDb (l, 24000, 48000) - rmsDb (in, 24000, 48000) == Approx (0.0).margin (0.3));

    p->analysisTick();
    l = in;
    r = in;
    runStereo (*p, l, r, 512);
    CHECK (rmsDb (l, 24000, 48000) - rmsDb (in, 24000, 48000) == Approx (6.0).margin (0.3));
}

// ------------------------------------------------------------------- threads

TEST_CASE ("processor: re-prepare while the audio and analysis threads run", "[processor][threads]")
{
    const JuceScope juce;
    auto p = std::make_unique<Strat3Processor> (true);   // real analysis thread
    setParam (*p, "eq.on", 1.0f);
    setParam (*p, "eq.b1.on", 1.0f);
    p->prepareToPlay (kFs, 512);

    std::atomic<bool> stop { false }, nonFinite { false };
    std::atomic<int> blocks { 0 };
    std::thread audio ([&]
    {
        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer midi;
        int k = 0;
        while (! stop.load())
        {
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < 512; ++i)
                    buf.setSample (c, i, 0.1f * std::sin (0.07f * static_cast<float> (k * 512 + i)));
            p->processBlock (buf, midi);
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < 512; ++i)
                    if (! std::isfinite (buf.getSample (c, i)))
                        nonFinite.store (true);
            ++k;
            blocks.store (k);
        }
    });

    for (int k = 0; k < 6; ++k)
    {
        setParam (*p, "global.latencyMode", static_cast<float> (k % 3));
        setParam (*p, "eq.b1.gain", static_cast<float> (k));
        std::this_thread::sleep_for (std::chrono::milliseconds (60));
        p->applyPendingConfig();
        CHECK (static_cast<int> (p->preparedMode()) == k % 3);
        CHECK (p->getLatencySamples() == engine::latencySamples (p->getEngine()));
    }
    stop.store (true);
    audio.join();
    CHECK (blocks.load() > 10);
    CHECK_FALSE (nonFinite.load());
    p.reset();   // stops the analysis thread before the message manager goes
}
