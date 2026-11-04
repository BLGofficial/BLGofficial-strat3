#include "processor/ParamBridge.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace strat3::processor {
namespace {

Raw get (juce::AudioProcessorValueTreeState& apvts, const std::string& id)
{
    // A missing ID is a programming error (spec table and bridge out of step). Throwing on the
    // message thread at construction beats a null dereference later on the audio thread.
    auto* p = apvts.getRawParameterValue (id);
    if (p == nullptr)
        throw std::logic_error ("STRAT3: no parameter " + id);
    return p;
}

float v (Raw p) noexcept { return p->load (std::memory_order_relaxed); }
bool on (Raw p) noexcept { return v (p) >= 0.5f; }

/** Choice raw values are indices stored as floats; round and clamp before the enum cast. */
template <typename E>
E pick (Raw p, int count) noexcept
{
    const float x = v (p);
    const int i = std::isfinite (x) ? std::clamp (static_cast<int> (std::lround (x)), 0, count - 1) : 0;
    return static_cast<E> (i);
}

std::string band (const char* prefix, std::size_t b) { return std::string (prefix) + std::to_string (b + 1); }

} // namespace

ParamBridge makeParamBridge (juce::AudioProcessorValueTreeState& apvts)
{
    ParamBridge b;
    auto g = [&] (const std::string& id) { return get (apvts, id); };

    b.latencyMode = g ("global.latencyMode");
    b.inTrim = g ("global.inTrim");
    b.outTrim = g ("global.outTrim");
    b.gainMatch = g ("global.gainMatch");
    b.globalDelta = g ("global.delta");
    b.target = g ("global.target");

    const std::array<const char*, engine::kDepthLayers> layers { "eq", "low", "mb", "width", "sat", "limit" };
    static_assert (engine::kEq == 0 && engine::kLow == 1 && engine::kMb == 2 && engine::kWidth == 3
                   && engine::kSat == 4 && engine::kLimit == 5, "layer names follow DepthLayer order");
    for (std::size_t i = 0; i < layers.size(); ++i)
    {
        const std::string l = layers[i];
        b.layer[i] = { g (l + ".on"), g (l + ".depth"), g (l + ".delta") };
    }

    b.ceilingOn = g ("ceiling.on");
    b.ceilingDb = g ("ceiling.dBTP");
    b.dither = g ("ceiling.dither");
    b.bits = g ("ceiling.bits");

    for (std::size_t i = 0; i < b.eq.size(); ++i)
    {
        const auto base = band ("eq.b", i);
        b.eq[i] = { g (base + ".on"), g (base + ".type"), g (base + ".freq"), g (base + ".gain"),
                    g (base + ".q"), g (base + ".ms"), g (base + ".dyn"), g (base + ".thresh"),
                    g (base + ".range"), g (base + ".atk"), g (base + ".rel") };
    }
    b.eqPhase = g ("eq.phase");

    b.xover1 = g ("xover.f1");
    b.xover2 = g ("xover.f2");
    b.xover3 = g ("xover.f3");

    b.lowXover = g ("low.xover");
    b.lowPunch = g ("low.punch");
    b.lowSustain = g ("low.sustain");
    b.lowMonoOn = g ("low.monoOn");
    b.lowMonoFreq = g ("low.monoFreq");
    b.lowTight = g ("low.tight");

    for (std::size_t i = 0; i < b.mb.size(); ++i)
    {
        const auto base = band ("mb.b", i);
        b.mb[i] = { g (base + ".thresh"), g (base + ".ratio"), g (base + ".dir"), g (base + ".knee"),
                    g (base + ".atk"), g (base + ".rel"), g (base + ".det"), g (base + ".mix") };
    }
    b.scSource = g ("mb.scSource");
    b.scHpOn = g ("mb.scHpOn");
    b.scHpFreq = g ("mb.scHpFreq");

    for (std::size_t i = 0; i < b.width.size(); ++i)
        b.width[i] = g (band ("width.b", i));
    b.stereoize = g ("width.stereoize");
    b.monoGuard = g ("width.monoGuard");

    for (std::size_t i = 0; i < b.sat.size(); ++i)
    {
        const auto base = band ("sat.b", i);
        b.sat[i] = { g (base + ".model"), g (base + ".drive"), g (base + ".mix") };
    }
    b.satOs = g ("sat.os");

    b.limitStyle = g ("limit.style");
    b.limitThresh = g ("limit.thresh");
    b.limitRelease = g ("limit.release");
    b.limitLink = g ("limit.link");
    b.limitTransient = g ("limit.transient");
    return b;
}

engine::EngineParams readParams (const ParamBridge& b, bool mono) noexcept
{
    engine::EngineParams p;
    p.inTrimDb = v (b.inTrim);
    p.outTrimDb = v (b.outTrim);
    p.gainMatch = on (b.gainMatch);
    p.globalDelta = on (b.globalDelta);
    for (std::size_t i = 0; i < p.layer.size(); ++i)
        p.layer[i] = { on (b.layer[i].on), v (b.layer[i].depth), on (b.layer[i].delta) };
    p.ceilingOn = on (b.ceilingOn);

    for (std::size_t i = 0; i < p.eq.size(); ++i)
    {
        const auto& s = b.eq[i];
        auto& e = p.eq[i];
        e.on = on (s.on);
        e.type = pick<dsp::EqType> (s.type, 6);
        e.freq = v (s.freq);
        e.gainDb = v (s.gain);
        e.q = v (s.q);
        e.channel = pick<dsp::EqChannel> (s.ms, 3);
        e.dyn = pick<dsp::EqDynMode> (s.dyn, 3);
        e.threshDb = v (s.thresh);
        e.rangeDb = v (s.range);
        e.attackMs = v (s.atk);
        e.releaseMs = v (s.rel);
    }
    p.eqPhase = v (b.eqPhase);

    p.low = { v (b.lowXover), v (b.lowPunch), v (b.lowSustain), v (b.lowTight), on (b.lowMonoOn), v (b.lowMonoFreq) };
    p.xover = { v (b.xover1), v (b.xover2), v (b.xover3) };

    for (std::size_t i = 0; i < p.mb.bands.size(); ++i)
    {
        const auto& s = b.mb[i];
        auto& m = p.mb.bands[i];
        m.threshDb = v (s.thresh);
        m.ratio = v (s.ratio);
        m.dir = pick<dsp::MbDir> (s.dir, 2);
        m.kneeDb = v (s.knee);
        m.attackMs = v (s.atk);
        m.releaseMs = v (s.rel);
        m.detector = pick<dsp::MbDetector> (s.det, 3);
        m.mixPercent = v (s.mix);
    }
    p.mb.sidechain = pick<dsp::MbSidechain> (b.scSource, 2);
    p.mb.sidechainHpOn = on (b.scHpOn);
    p.mb.sidechainHpHz = v (b.scHpFreq);

    for (std::size_t i = 0; i < p.width.widthPercent.size(); ++i)
        p.width.widthPercent[i] = v (b.width[i]);
    p.width.stereoizePercent = mono ? 0.0f : v (b.stereoize);
    p.width.monoGuard = on (b.monoGuard);

    for (std::size_t i = 0; i < p.sat.bands.size(); ++i)
        p.sat.bands[i] = { pick<dsp::SatModel> (b.sat[i].model, 4), v (b.sat[i].drive), v (b.sat[i].mix) };

    p.limit.style = pick<dsp::LimitStyle> (b.limitStyle, 3);
    p.limit.threshDb = v (b.limitThresh);
    p.limit.releaseMs = v (b.limitRelease);
    p.limit.linkPercent = v (b.limitLink);
    p.limit.transientPercent = v (b.limitTransient);
    // limit.ceilingDb is set by the engine from ceiling.dBTP (one control for both stages).

    p.ceiling.ceilingDb = v (b.ceilingDb);
    p.ceiling.dither = pick<dsp::DitherMode> (b.dither, 3);
    p.ceiling.bits = pick<int> (b.bits, 2) == 0 ? 16 : 24;
    return p;
}

engine::LatencyMode readLatencyMode (const ParamBridge& b) noexcept
{
    return pick<engine::LatencyMode> (b.latencyMode, 3);
}

int readSatOs (const ParamBridge& b) noexcept
{
    return 1 << pick<int> (b.satOs, 3);
}

} // namespace strat3::processor
