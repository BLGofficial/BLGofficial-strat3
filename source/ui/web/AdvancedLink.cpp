#include "ui/web/AdvancedLink.h"

#include "dsp/crossover/CrossoverPoints.h"
#include "engine/Engine.h"
#include "params/ParameterSpecs.h"

namespace strat3::ui::web {

namespace {

const params::ParamSpec* specFor (const char* id)
{
    for (const auto& s : params::allSpecs())
        if (s.id == id)
            return &s;
    return nullptr;
}

float value (const processor::Strat3Processor& p, const char* id)
{
    const auto* raw = p.apvts.getRawParameterValue (id);
    return raw != nullptr ? raw->load() : 0.0f;
}

/** sat.os's choice index as the factor it asks for. */
int requestedOversampling (const processor::Strat3Processor& p)
{
    return 1 << std::clamp (static_cast<int> (std::lround (value (p, "sat.os"))), 0, 2);
}

} // namespace

const char* relayKindName (RelayKind k) noexcept
{
    switch (k)
    {
        case RelayKind::Slider:   return "slider";
        case RelayKind::Toggle:   return "toggle";
        case RelayKind::ComboBox: return "comboBox";
    }
    return "";
}

juce::var relayEntry (const RelaySpec& r)
{
    const auto* s = specFor (r.id);
    jassert (s != nullptr);   // test_advanced_link checks every relay against the parameters
    if (s == nullptr)
        return {};
    auto* o = new juce::DynamicObject;
    o->setProperty ("id", juce::String (r.id));
    o->setProperty ("kind", juce::String (relayKindName (r.kind)));
    o->setProperty ("name", juce::String (s->name));
    o->setProperty ("min", s->min);
    o->setProperty ("max", s->max);
    o->setProperty ("default", s->def);
    o->setProperty ("unit", juce::String (s->unit));
    if (r.kind == RelayKind::Slider)
        o->setProperty ("taper", s->taper == params::Taper::Log ? "log" : "lin");
    if (r.kind == RelayKind::ComboBox)
    {
        juce::Array<juce::var> choices;
        for (const auto& c : s->choices)
            choices.add (juce::String (c));
        o->setProperty ("choices", choices);
    }
    return o;
}

juce::var relayContract()
{
    juce::Array<juce::var> out;
    for (const auto& r : kAdvancedRelays)
        if (auto entry = relayEntry (r); ! entry.isVoid())
            out.add (entry);
    return out;
}

juce::var advancedInfo (const processor::Strat3Processor& p)
{
    const double fs = p.preparedSampleRate();
    const bool running = fs > 0.0;

    auto* os = new juce::DynamicObject;
    // Running: the factor the engine was prepared with, not the parameter. A change waits for
    // the re-prepare (Strat3Processor::configPending), and comparing the new ask with the old
    // engine would read that wait as the rate cap.
    os->setProperty ("requested", running ? p.preparedSatOs() : requestedOversampling (p));
    // What the engine runs, after its cap for the rate (engine::capOversampling).
    os->setProperty ("active", running ? juce::var (engine::effectiveSatOs (p.getEngine())) : juce::var());

    auto* sc = new juce::DynamicObject;
    sc->setProperty ("channels", p.sidechainChannelCount());

    juce::var crossovers, crossoversSet;
    if (running)
    {
        // The engine sorts the points and keeps them apart before it designs the split;
        // shown as it runs, so a point dragged past its neighbour is not displayed as set.
        // The points as set go with them, read once: the page's sliders are ahead of this
        // info during a drag, and comparing the two would flicker the page's "in use" line.
        const dsp::CrossoverPoints set { value (p, "xover.f1"), value (p, "xover.f2"), value (p, "xover.f3") };
        const auto x = dsp::sanitiseCrossovers (set, fs);
        crossovers = juce::Array<juce::var> { x.f1, x.f2, x.f3 };
        crossoversSet = juce::Array<juce::var> { set.f1, set.f2, set.f3 };
    }

    auto* o = new juce::DynamicObject;
    o->setProperty ("topology", running ? juce::var (p.preparedMode() == engine::LatencyMode::Master ? "fir" : "iir") : juce::var());
    o->setProperty ("sampleRate", fs);
    o->setProperty ("crossovers", crossovers);
    o->setProperty ("crossoversSet", crossoversSet);
    o->setProperty ("oversampling", os);
    o->setProperty ("sidechain", sc);
    return o;
}

} // namespace strat3::ui::web
