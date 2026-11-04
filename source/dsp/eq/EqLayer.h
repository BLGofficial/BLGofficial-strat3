#pragma once
// IIR EQ layer (TRACK/MIX, and the dynamic nodes in MASTER).
// 8 bands x up to 2 TPT SVF sections, coefficient glide per sample.
// Signal path: pass 1 = stereo bands on L/R; pass 2 (only while any mid/side band is active)
// = encode M/S -> mid/side bands -> decode. Running stereo bands on L/R keeps a stereo-only
// EQ bit-exact at neutral; M/S encode/decode costs ~1 ulp.
// Dynamic nodes: a 0 dB-peak band-pass detector at the node's freq/Q feeds a peak envelope;
// every kEqControlInterval samples the offset is recomputed and the coefficients glide to it.
#include <algorithm>
#include <array>
#include <cmath>

#include "dsp/core/Smoother.h"
#include "dsp/core/SvfTpt.h"
#include "dsp/eq/EqResponse.h"
#include "dsp/eq/EqTypes.h"

namespace strat3::dsp {

inline constexpr float kEqGlideMs = 5.0f;         // static parameter changes
inline constexpr float kEqDynGlideMs = 1.0f;      // dynamic gain: fast enough for short attacks
inline constexpr int   kEqControlInterval = 16;   // samples between dynamic coefficient updates

struct EqBandState
{
    EqBand settings {};
    std::array<std::array<ModulatedSvf, 2>, 2> sec {};   // [section][channel]
    std::array<SvfState, 2> det {};
    SvfCoeffs detCoeffs = passthroughCoeffs();
    std::array<float, 2> env {};
    float dynGainDb = 0.0f;
    float attackCoeff = 0.0f, releaseCoeff = 0.0f;
};

struct EqLayer
{
    double sampleRate = 48000.0;
    std::array<EqBandState, kEqBands> bands {};
    bool msActive = false;
    int controlCounter = 0;
    float glideCoeff = 0.0f, dynGlideCoeff = 0.0f;
};

namespace eqdetail {

inline bool isDynamic (const EqBand& b) noexcept { return b.on && b.dyn != EqDynMode::Off && supportsGain (b.type); }

inline SvfCoeffs neutralised (SvfCoeffs c) noexcept
{
    // Same g/k so the state keeps tracking the band; output mix = input. Exact passthrough.
    c.m0 = 1.0f; c.m1 = 0.0f; c.m2 = 0.0f;
    return c;
}

inline void applyTargets (EqLayer& eq, EqBandState& st) noexcept
{
    const auto active = bandSections (st.settings, st.dynGainDb, eq.sampleRate);
    EqBand shape = st.settings;
    shape.on = true;
    const auto shapeSections = bandSections (shape, 0.0f, eq.sampleRate);   // g/k for off bands

    for (size_t s = 0; s < 2; ++s)
    {
        const bool used = st.settings.on && static_cast<int> (s) < active.count;
        const auto target = used ? active.sections[s] : neutralised (shapeSections.sections[s]);
        for (auto& f : st.sec[s])
            setTarget (f, target);
    }
}

inline bool isSettledNeutral (const EqBandState& st) noexcept
{
    for (const auto& section : st.sec)
        for (const auto& f : section)
            if (f.current.m0 != 1.0f || f.current.m1 != 0.0f || f.current.m2 != 0.0f)
                return false;
    return true;
}

inline void updateMsActive (EqLayer& eq) noexcept
{
    eq.msActive = std::any_of (eq.bands.begin(), eq.bands.end(), [] (const EqBandState& st)
    {
        return st.settings.channel != EqChannel::Stereo && (st.settings.on || ! isSettledNeutral (st));
    });
}

/** Release/attack scale with frequency: low bands need longer windows to see a full cycle. */
inline float frequencyScale (float hz) noexcept
{
    return std::clamp (std::sqrt (1000.0f / std::max (hz, 1.0f)), 0.25f, 4.0f);
}

inline void controlTick (EqLayer& eq) noexcept
{
    for (auto& st : eq.bands)
    {
        if (! isDynamic (st.settings))
            continue;
        const auto& b = st.settings;
        const float env = b.channel == EqChannel::Stereo ? std::max (st.env[0], st.env[1]) : st.env[0];
        const float envDb = 20.0f * std::log10 (std::max (env, 1e-9f));
        const float travel = b.dyn == EqDynMode::Down ? envDb - b.threshDb : b.threshDb - envDb;
        st.dynGainDb = b.rangeDb * std::clamp (travel / kDynamicTravelDb, 0.0f, 1.0f);
        applyTargets (eq, st);
    }
    updateMsActive (eq);
}

inline float processBandChannel (EqBandState& st, size_t ch, float x) noexcept
{
    if (isDynamic (st.settings))
    {
        // Detector sees the band input, before the band's own gain: feedforward, no loop.
        const float rect = std::abs (processSample (st.det[ch], st.detCoeffs, x));
        float& e = st.env[ch];
        e = rect + (rect > e ? st.attackCoeff : st.releaseCoeff) * (e - rect);
    }
    x = process (st.sec[0][ch], x);
    return process (st.sec[1][ch], x);
}

} // namespace eqdetail

inline void prepare (EqLayer& eq, double sampleRate)
{
    eq.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    eq.glideCoeff    = makeOnePole (kEqGlideMs,    eq.sampleRate, 0.0f).coeff;
    eq.dynGlideCoeff = makeOnePole (kEqDynGlideMs, eq.sampleRate, 0.0f).coeff;
    eq.controlCounter = 0;
    for (auto& st : eq.bands)
    {
        st = {};
        for (auto& section : st.sec)
            for (auto& f : section)
                f = makeModulatedSvf (eq.sampleRate, kEqGlideMs, passthroughCoeffs());
        eqdetail::applyTargets (eq, st);
    }
    eqdetail::updateMsActive (eq);
}

/** Audio thread, once per block (or per sample: unchanged bands are a no-op).
 *  A band that changes channel (stereo <-> mid/side) restarts from silence: its old state
 *  belonged to a different signal. */
inline void setBands (EqLayer& eq, const EqSettings& settings) noexcept
{
    for (size_t i = 0; i < settings.size(); ++i)
    {
        auto& st = eq.bands[i];
        const auto& b = settings[i];
        if (b == st.settings)
            continue;

        if (b.channel != st.settings.channel)
            for (auto& section : st.sec)
                for (auto& f : section)
                    f.state = {};

        st.settings = b;
        const bool dynamic = eqdetail::isDynamic (b);
        if (! dynamic)
            st.dynGainDb = 0.0f;
        for (auto& section : st.sec)
            for (auto& f : section)
                f.coeff = dynamic ? eq.dynGlideCoeff : eq.glideCoeff;

        st.detCoeffs = makeSvfCoeffs (SvfType::BandPass, b.freq, b.q, 0.0f, eq.sampleRate);
        const float scale = eqdetail::frequencyScale (b.freq);
        st.attackCoeff  = makeOnePole (b.attackMs * scale,  eq.sampleRate, 0.0f).coeff;
        st.releaseCoeff = makeOnePole (b.releaseMs * scale, eq.sampleRate, 0.0f).coeff;
        eqdetail::applyTargets (eq, st);
    }
    eqdetail::updateMsActive (eq);
}

/** Jump all coefficient glides to their targets: after prepare / state load only. */
inline void snapToTargets (EqLayer& eq) noexcept
{
    for (auto& st : eq.bands)
        for (auto& section : st.sec)
            for (auto& f : section)
                f.current = f.target;
    eqdetail::updateMsActive (eq);
}

inline void process (EqLayer& eq, float& l, float& r) noexcept
{
    if (++eq.controlCounter >= kEqControlInterval)
    {
        eq.controlCounter = 0;
        eqdetail::controlTick (eq);
    }

    for (auto& st : eq.bands)
        if (st.settings.channel == EqChannel::Stereo)
        {
            l = eqdetail::processBandChannel (st, 0, l);
            r = eqdetail::processBandChannel (st, 1, r);
        }

    if (! eq.msActive)
        return;

    float m = (l + r) * 0.5f, s = (l - r) * 0.5f;
    for (auto& st : eq.bands)
    {
        if (st.settings.channel == EqChannel::Mid)
            m = eqdetail::processBandChannel (st, 0, m);
        else if (st.settings.channel == EqChannel::Side)
            s = eqdetail::processBandChannel (st, 0, s);
    }
    l = m + s;
    r = m - s;
}

/** Current dynamic offset of a band in dB (0 for static bands). For metering. */
inline float dynamicGainDb (const EqLayer& eq, int band) noexcept
{
    return eq.bands[static_cast<size_t> (band)].dynGainDb;
}

inline bool resetIfNotFinite (EqLayer& eq) noexcept
{
    bool bad = false;
    for (auto& st : eq.bands)
    {
        for (auto& section : st.sec)
            for (auto& f : section)
                bad |= ! isFinite (f.state);
        for (size_t c = 0; c < 2; ++c)
            bad |= ! isFinite (st.det[c]) || ! std::isfinite (st.env[c]);
    }
    if (! bad)
        return false;

    for (auto& st : eq.bands)
    {
        for (auto& section : st.sec)
            for (auto& f : section)
                f.state = {};
        st.det = {};
        st.env = {};
        st.dynGainDb = 0.0f;
        eqdetail::applyTargets (eq, st);
    }
    return true;
}

inline void flushDenormals (EqLayer& eq) noexcept
{
    for (auto& st : eq.bands)
    {
        for (auto& section : st.sec)
            for (auto& f : section)
                flushDenormals (f.state);
        for (size_t c = 0; c < 2; ++c)
        {
            flushDenormals (st.det[c]);
            if (st.env[c] < kSnapEpsilon)
                st.env[c] = 0.0f;
        }
    }
}

} // namespace strat3::dsp
