#include "params/ParameterSpecs.h"

#include <cstdio>

namespace strat3::params {
namespace {

// ---- spec builders: small pure helpers so the table below reads like the Phase 5 map.

ParamSpec flt (std::string id, std::string name, float lo, float hi, float def,
               std::string unit, Taper taper = Taper::Linear)
{
    return { std::move (id), std::move (name), Kind::Float, lo, hi, def, taper, {}, std::move (unit) };
}

ParamSpec logf (std::string id, std::string name, float lo, float hi, float def, std::string unit)
{
    return flt (std::move (id), std::move (name), lo, hi, def, std::move (unit), Taper::Log);
}

ParamSpec choice (std::string id, std::string name, std::vector<std::string> options, int defIndex)
{
    const auto top = static_cast<float> (options.size() - 1);
    return { std::move (id), std::move (name), Kind::Choice, 0.0f, top,
             static_cast<float> (defIndex), Taper::Linear, std::move (options) };
}

ParamSpec toggle (std::string id, std::string name, bool def)
{
    return { std::move (id), std::move (name), Kind::Bool, 0.0f, 1.0f, def ? 1.0f : 0.0f };
}

ParamSpec monitorOnly (ParamSpec s)
{
    s.automatable = false;
    return s;
}

std::string upperFirst (std::string s)
{
    if (! s.empty() && s[0] >= 'a' && s[0] <= 'z')
        s[0] = static_cast<char> (s[0] - 'a' + 'A');
    return s;
}

// Layers default OFF so a fresh instance nulls against its input: even "neutral" layers are
// not bit-transparent (an LR4 split sums to an all-pass, not a wire).
void addLayerCommon (std::vector<ParamSpec>& out, const std::string& layer, const std::string& label)
{
    out.push_back (toggle (layer + ".on", label + " On", false));
    out.push_back (flt (layer + ".depth", label + " Depth", 0.0f, 150.0f, 100.0f, "%"));
    out.push_back (monitorOnly (toggle (layer + ".delta", label + " Delta", false)));
}

void addGlobal (std::vector<ParamSpec>& out)
{
    // Changing latency mid-playback glitches some hosts, so it is never automatable.
    out.push_back (monitorOnly (choice ("global.latencyMode", "Latency Mode", { "TRACK", "MIX", "MASTER" }, 1)));
    out.push_back (flt ("global.inTrim",  "Input Trim",  -24.0f, 24.0f, 0.0f, "dB"));
    out.push_back (flt ("global.outTrim", "Output Trim", -24.0f, 24.0f, 0.0f, "dB"));
    out.push_back (monitorOnly (toggle ("global.gainMatch", "Gain Match", false)));
    out.push_back (monitorOnly (toggle ("global.delta", "Global Delta", false)));
    out.push_back (choice ("global.target", "Loudness Target",
                           { "OFF", "-14 LUFS", "-11 LUFS", "-9 LUFS", "-8 LUFS", "USER" }, 0));
}

void addSharedCrossover (std::vector<ParamSpec>& out)
{
    // One split shared by the band domain (MULTIBAND, WIDTH, SATURATE). Ranges overlap on
    // purpose; the DSP enforces f1 < f2 < f3 with a minimum ratio, not the host ranges.
    out.push_back (logf ("xover.f1", "Crossover 1",   40.0f,   500.0f,  120.0f, "Hz"));
    out.push_back (logf ("xover.f2", "Crossover 2",  200.0f,  5000.0f, 1000.0f, "Hz"));
    out.push_back (logf ("xover.f3", "Crossover 3", 1000.0f, 16000.0f, 5000.0f, "Hz"));
}

void addStems (std::vector<ParamSpec>& out)
{
    addLayerCommon (out, "stems", "Stems");
    for (const auto* stem : { "voc", "bass", "drm", "oth" })
    {
        const std::string base = std::string ("stems.") + stem;
        const std::string label = std::string ("Stem ") + upperFirst (stem);
        out.push_back (flt (base + ".gain", label + " Gain", -12.0f, 12.0f, 0.0f, "dB"));
        out.push_back (flt (base + ".tilt", label + " Tilt", -6.0f, 6.0f, 0.0f, "dB/oct"));
    }
}

void addEq (std::vector<ParamSpec>& out)
{
    addLayerCommon (out, "eq", "EQ");
    // Spread defaults so enabling a band lands it somewhere useful instead of stacking at 1 kHz.
    constexpr float defaultFreqs[8] = { 30.0f, 80.0f, 200.0f, 500.0f, 1000.0f, 2500.0f, 6000.0f, 12000.0f };

    for (int b = 1; b <= 8; ++b)
    {
        const std::string base = "eq.b" + std::to_string (b);
        const std::string label = "EQ " + std::to_string (b);
        out.push_back (toggle (base + ".on", label + " On", false));
        out.push_back (choice (base + ".type", label + " Type",
                               { "Bell", "Low Shelf", "High Shelf", "High Pass", "Low Pass", "Passive Shelf" }, 0));
        out.push_back (logf (base + ".freq", label + " Freq", 20.0f, 20000.0f, defaultFreqs[b - 1], "Hz"));
        out.push_back (flt (base + ".gain", label + " Gain", -18.0f, 18.0f, 0.0f, "dB"));
        out.push_back (logf (base + ".q", label + " Q", 0.1f, 18.0f, 0.707f, ""));
        out.push_back (choice (base + ".ms", label + " Channel", { "Stereo", "Mid", "Side" }, 0));
        out.push_back (choice (base + ".dyn", label + " Dynamic", { "Off", "Up", "Down" }, 0));
        out.push_back (flt (base + ".thresh", label + " Dyn Threshold", -60.0f, 0.0f, -20.0f, "dBFS"));
        out.push_back (flt (base + ".range", label + " Dyn Range", -12.0f, 12.0f, -3.0f, "dB"));
        out.push_back (logf (base + ".atk", label + " Dyn Attack", 0.1f, 100.0f, 10.0f, "ms"));
        out.push_back (logf (base + ".rel", label + " Dyn Release", 5.0f, 1000.0f, 100.0f, "ms"));
    }
    out.push_back (flt ("eq.phase", "EQ Phase", 0.0f, 100.0f, 0.0f, "%"));
}

void addLow (std::vector<ParamSpec>& out)
{
    addLayerCommon (out, "low", "Low");
    out.push_back (logf ("low.xover", "Low Crossover", 40.0f, 200.0f, 100.0f, "Hz"));
    out.push_back (flt ("low.punch", "Low Punch", -100.0f, 100.0f, 0.0f, "%"));
    out.push_back (flt ("low.sustain", "Low Sustain", -100.0f, 100.0f, 0.0f, "%"));
    out.push_back (toggle ("low.monoOn", "Mono Below On", false));
    out.push_back (logf ("low.monoFreq", "Mono Below Freq", 40.0f, 200.0f, 100.0f, "Hz"));
    out.push_back (flt ("low.tight", "Low Tight", 0.0f, 100.0f, 0.0f, "%"));
}

void addMultiband (std::vector<ParamSpec>& out)
{
    addLayerCommon (out, "mb", "Multiband");
    for (int b = 1; b <= 4; ++b)
    {
        const std::string base = "mb.b" + std::to_string (b);
        const std::string label = "MB " + std::to_string (b);
        out.push_back (flt (base + ".thresh", label + " Threshold", -60.0f, 0.0f, 0.0f, "dBFS"));
        out.push_back (logf (base + ".ratio", label + " Ratio", 1.0f, 20.0f, 1.0f, ":1")); // 1:1 = neutral
        out.push_back (choice (base + ".dir", label + " Direction", { "Down", "Up" }, 0));
        out.push_back (flt (base + ".knee", label + " Knee", 0.0f, 24.0f, 6.0f, "dB"));
        out.push_back (logf (base + ".atk", label + " Attack", 0.1f, 100.0f, 10.0f, "ms"));
        out.push_back (logf (base + ".rel", label + " Release", 5.0f, 1000.0f, 120.0f, "ms"));
        out.push_back (choice (base + ".det", label + " Detector", { "Peak", "RMS", "Envelope" }, 1));
        out.push_back (flt (base + ".mix", label + " Mix", 0.0f, 100.0f, 100.0f, "%"));
    }
    out.push_back (choice ("mb.scSource", "MB Sidechain", { "Internal", "External" }, 0));
    out.push_back (toggle ("mb.scHpOn", "MB Sidechain HP On", false));
    out.push_back (logf ("mb.scHpFreq", "MB Sidechain HP Freq", 20.0f, 500.0f, 80.0f, "Hz"));
}

void addWidth (std::vector<ParamSpec>& out)
{
    addLayerCommon (out, "width", "Width");
    for (int b = 1; b <= 4; ++b)
        out.push_back (flt ("width.b" + std::to_string (b), "Width " + std::to_string (b),
                            -100.0f, 100.0f, 0.0f, "%"));
    out.push_back (flt ("width.stereoize", "Stereoize", 0.0f, 100.0f, 0.0f, "%"));
    out.push_back (toggle ("width.monoGuard", "Mono Guard", true));
}

void addSaturate (std::vector<ParamSpec>& out)
{
    addLayerCommon (out, "sat", "Saturate");
    for (int b = 1; b <= 4; ++b)
    {
        const std::string base = "sat.b" + std::to_string (b);
        const std::string label = "Sat " + std::to_string (b);
        out.push_back (choice (base + ".model", label + " Model",
                               { "Warm Even", "Odd", "Tape Style", "Triode Style" }, 0));
        out.push_back (flt (base + ".drive", label + " Drive", 0.0f, 24.0f, 0.0f, "dB"));
        out.push_back (flt (base + ".mix", label + " Mix", 0.0f, 100.0f, 100.0f, "%"));
    }
    out.push_back (choice ("sat.os", "Saturate Oversampling", { "1x", "2x", "4x" }, 1));
}

void addLimit (std::vector<ParamSpec>& out)
{
    addLayerCommon (out, "limit", "Limit");
    out.push_back (choice ("limit.style", "Limit Style", { "Clean", "Dense", "Clip" }, 0));
    out.push_back (flt ("limit.thresh", "Limit Threshold", -24.0f, 0.0f, 0.0f, "dB"));
    out.push_back (logf ("limit.release", "Limit Release", 1.0f, 1000.0f, 100.0f, "ms"));
    out.push_back (flt ("limit.link", "Limit Stereo Link", 0.0f, 100.0f, 100.0f, "%"));
    out.push_back (flt ("limit.transient", "Limit Transient", 0.0f, 100.0f, 0.0f, "%"));
}

void addCeiling (std::vector<ParamSpec>& out)
{
    // ON by default: a mastering chain that can emit overs on first load is a footgun.
    // It is transparent for material that stays below the ceiling.
    out.push_back (toggle ("ceiling.on", "Ceiling On", true));
    out.push_back (flt ("ceiling.dBTP", "Ceiling", -3.0f, 0.0f, -1.0f, "dBTP"));
    out.push_back (choice ("ceiling.dither", "Dither", { "Off", "TPDF", "TPDF Shaped" }, 0));
    out.push_back (choice ("ceiling.bits", "Dither Bits", { "16", "24" }, 1));
}

std::vector<ParamSpec> buildSpecs()
{
    std::vector<ParamSpec> out;
    out.reserve (192);
    addGlobal (out);
    addSharedCrossover (out);
    addStems (out);
    addEq (out);
    addLow (out);
    addMultiband (out);
    addWidth (out);
    addSaturate (out);
    addLimit (out);
    addCeiling (out);
    return out;
}

const char* kindName (Kind k)
{
    switch (k)
    {
        case Kind::Float:  return "float";
        case Kind::Choice: return "choice";
        case Kind::Bool:   return "bool";
    }
    return "?";
}

std::string fmt (float v)
{
    char buf[32];
    std::snprintf (buf, sizeof (buf), "%.6g", static_cast<double> (v));
    return buf;
}

} // namespace

const std::vector<ParamSpec>& allSpecs()
{
    static const std::vector<ParamSpec> specs = buildSpecs();
    return specs;
}

const std::vector<std::string>& processingLayerIds()
{
    // Default chain order. Stems first: separation masks were computed on the raw mix.
    static const std::vector<std::string> ids { "stems", "eq", "low", "mb", "width", "sat", "limit" };
    return ids;
}

std::string goldenLine (const ParamSpec& s)
{
    std::string choices;
    for (size_t i = 0; i < s.choices.size(); ++i)
        choices += (i ? "," : "") + s.choices[i];

    return s.id + "|" + kindName (s.kind) + "|" + fmt (s.min) + "|" + fmt (s.max) + "|"
         + (s.taper == Taper::Log ? "log" : "lin") + "|" + choices;
}

} // namespace strat3::params
