#pragma once
// WIDTH layer (band domain).
//   Per band: S' = (1 + w) * S + k * D,   L = M + S', R = M - S'
//   w = width.bN / 100 in [-1, 1];  k = stereoize / 100 * guard
//   D = mid -> 4 all-passes (200 Hz .. 8 kHz, log spaced) -> 4th-order high-pass at 300 Hz
// Everything lands in the side, so the mono fold-down (L + R = 2M) never changes: width and
// stereoize cannot cause mono cancellation. D is computed per band from that band's mid
// (all filters are linear, so the sum over bands equals one stereoize on the full mid) and
// stays inside its band for any later band layers.
// Mono guard: when the output correlation (300 ms) falls below 0.2, the stereoize amount is
// pulled down (fully off in 250 ms), and released again above 0.3 (back in 2 s). The
// hysteresis and rates are our own tuning [INFERRED]. The guard never touches width.
#include <algorithm>
#include <array>
#include <cmath>

#include "analysis/Correlation.h"
#include "dsp/core/Smoother.h"
#include "dsp/core/SvfTpt.h"
#include "dsp/crossover/BandDomain.h"

namespace strat3::dsp {

inline constexpr float kWidthRampMs = 20.0f;
inline constexpr std::array<float, 4> kStereoizeAllPassHz { 200.0f, 684.0f, 2339.0f, 8000.0f };
inline constexpr float kStereoizeAllPassQ = 0.7071f;
inline constexpr float kStereoizeHpHz = 300.0f;
inline constexpr float kGuardLow = 0.2f;
inline constexpr float kGuardHigh = 0.3f;
inline constexpr float kGuardDownPerSec = 4.0f;
inline constexpr float kGuardUpPerSec = 0.5f;
inline constexpr int   kWidthControlInterval = 16;

struct WidthSettings
{
    std::array<float, 4> widthPercent {};   // -100..100
    float stereoizePercent = 0.0f;          // 0..100
    bool monoGuard = true;
};

struct WidthBandState
{
    std::array<SvfState, 4> ap {};
    std::array<SvfState, 2> hp {};
    LinearRamp width {};
};

struct WidthLayer
{
    double sampleRate = 48000.0;
    WidthSettings settings {};
    std::array<WidthBandState, 4> bands {};
    std::array<SvfCoeffs, 4> apCoeffs {};
    SvfCoeffs hpCoeffs = passthroughCoeffs();
    LinearRamp amount {};
    float guard = 1.0f;
    float guardDownStep = 0.0f, guardUpStep = 0.0f;   // per control tick
    int controlCounter = 0;
    analysis::CorrelationMeter correlation {};
};

namespace widthdetail {

inline float finiteOr (float v, float keep) noexcept { return std::isfinite (v) ? v : keep; }

inline void controlTick (WidthLayer& w) noexcept
{
    if (! w.settings.monoGuard)
    {
        w.guard = std::min (1.0f, w.guard + w.guardUpStep);
        return;
    }
    const float r = analysis::value (w.correlation);
    if (r < kGuardLow)
        w.guard = std::max (0.0f, w.guard - w.guardDownStep);
    else if (r > kGuardHigh)
        w.guard = std::min (1.0f, w.guard + w.guardUpStep);
}

} // namespace widthdetail

inline int latencySamples (const WidthLayer&) noexcept { return 0; }
inline float stereoizeGuard (const WidthLayer& w) noexcept { return w.guard; }
inline float outputCorrelation (const WidthLayer& w) noexcept { return analysis::value (w.correlation); }

/** Audio thread, once per block. Non-finite fields keep their previous value. */
inline void setSettings (WidthLayer& w, const WidthSettings& in) noexcept
{
    using widthdetail::finiteOr;
    auto& t = w.settings;
    for (std::size_t b = 0; b < 4; ++b)
    {
        t.widthPercent[b] = std::clamp (finiteOr (in.widthPercent[b], t.widthPercent[b]), -100.0f, 100.0f);
        setTarget (w.bands[b].width, t.widthPercent[b] * 0.01f);
    }
    t.stereoizePercent = std::clamp (finiteOr (in.stereoizePercent, t.stereoizePercent), 0.0f, 100.0f);
    t.monoGuard = in.monoGuard;
    setTarget (w.amount, t.stereoizePercent * 0.01f);
}

/** Jump ramps to their targets: after prepare / state load only. */
inline void snapToTargets (WidthLayer& w) noexcept
{
    for (std::size_t b = 0; b < 4; ++b)
        w.bands[b].width = makeLinearRamp (kWidthRampMs, w.sampleRate, w.settings.widthPercent[b] * 0.01f);
    w.amount = makeLinearRamp (kWidthRampMs, w.sampleRate, w.settings.stereoizePercent * 0.01f);
}

/** prepareToPlay: resets all state, keeps the settings. */
inline void prepare (WidthLayer& w, double sampleRate)
{
    w.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    for (std::size_t i = 0; i < 4; ++i)
        w.apCoeffs[i] = makeSvfCoeffs (SvfType::AllPass, kStereoizeAllPassHz[i], kStereoizeAllPassQ, 0.0f, w.sampleRate);
    w.hpCoeffs = makeSvfCoeffs (SvfType::HighPass, kStereoizeHpHz, 0.70710678f, 0.0f, w.sampleRate);
    for (auto& st : w.bands)
        st = {};
    snapToTargets (w);
    w.guard = 1.0f;
    const float tick = static_cast<float> (kWidthControlInterval / w.sampleRate);
    w.guardDownStep = kGuardDownPerSec * tick;
    w.guardUpStep = kGuardUpPerSec * tick;
    w.controlCounter = 0;
    analysis::prepare (w.correlation, w.sampleRate);
}

inline void process (WidthLayer& w, StereoBands& io) noexcept
{
    if (++w.controlCounter >= kWidthControlInterval)
    {
        w.controlCounter = 0;
        widthdetail::controlTick (w);
    }

    const float k = step (w.amount) * w.guard;
    float sumL = 0.0f, sumR = 0.0f;
    for (std::size_t b = 0; b < 4; ++b)
    {
        auto& st = w.bands[b];
        const float ww = step (st.width);
        const float l = io.l[b], r = io.r[b];
        const float m = (l + r) * 0.5f, s = (l - r) * 0.5f;

        // Filters always run, so raising stereoize starts from a warm state, not a transient.
        float d = m;
        for (std::size_t i = 0; i < 4; ++i)
            d = processSample (st.ap[i], w.apCoeffs[i], d);
        for (auto& h : st.hp)
            d = processSample (h, w.hpCoeffs, d);

        if (ww != 0.0f || k != 0.0f)   // neutral bands stay bit-exact (M/S round trip rounds)
        {
            const float side = (1.0f + ww) * s + k * d;
            io.l[b] = m + side;
            io.r[b] = m - side;
        }
        sumL += io.l[b];
        sumR += io.r[b];
    }
    analysis::push (w.correlation, sumL, sumR);
}

template <typename Fn>
void forEachFloatState (WidthLayer& w, Fn&& fn)
{
    for (auto& st : w.bands)
    {
        for (auto& s : st.ap) { fn (s.ic1eq); fn (s.ic2eq); }
        for (auto& s : st.hp) { fn (s.ic1eq); fn (s.ic2eq); }
        fn (st.width.current);
    }
    fn (w.amount.current);
    fn (w.guard);
    fn (static_cast<float> (w.correlation.lr));
    fn (static_cast<float> (w.correlation.ll));
    fn (static_cast<float> (w.correlation.rr));
}

/** Returns true if any state had to be reset (raise the UI flag; never log here). */
inline bool resetIfNotFinite (WidthLayer& w) noexcept
{
    bool bad = ! analysis::isFinite (w.correlation);
    forEachFloatState (w, [&] (float v) { bad |= ! std::isfinite (v); });
    if (! bad)
        return false;
    for (auto& st : w.bands)
    {
        st.ap = {};
        st.hp = {};
    }
    analysis::reset (w.correlation);
    w.guard = 1.0f;
    return true;
}

inline void flushDenormals (WidthLayer& w) noexcept
{
    for (auto& st : w.bands)
    {
        for (auto& s : st.ap) flushDenormals (s);
        for (auto& s : st.hp) flushDenormals (s);
    }
}

} // namespace strat3::dsp
