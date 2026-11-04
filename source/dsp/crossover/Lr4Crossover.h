#pragma once
// Linkwitz-Riley 4th-order (24 dB/oct) crossovers built from TPT SVF sections.
// LR4 = two cascaded Butterworth 2nd-order sections; low + high sum to a 2nd-order all-pass,
// so a split has flat magnitude. Used in TRACK/MIX latency modes (zero latency).
#include <array>
#include <cmath>
#include <numbers>

#include "dsp/core/Smoother.h"
#include "dsp/core/SvfTpt.h"
#include "dsp/crossover/CrossoverPoints.h"

namespace strat3::dsp {

inline constexpr float kButterworthK = 1.41421356f;   // k = 1/Q with Q = 1/sqrt(2)
inline constexpr float kCrossoverSmoothMs = 20.0f;

namespace lr4detail {

struct Coeffs { float a1, a2, a3; };
struct Taps   { float low, band, high; };

inline float prewarp (float fc, double sampleRate) noexcept
{
    return static_cast<float> (std::tan (std::numbers::pi * static_cast<double> (fc) / sampleRate));
}

inline Coeffs coeffsFor (float g) noexcept
{
    const float a1 = 1.0f / (1.0f + g * (g + kButterworthK));
    const float a2 = g * a1;
    return { a1, a2, g * a2 };
}

/** One SVF tick returning all three outputs (one state, three responses). */
inline Taps tick (SvfState& s, const Coeffs& c, float v0) noexcept
{
    const float v3 = v0 - s.ic2eq;
    const float v1 = c.a1 * s.ic1eq + c.a2 * v3;
    const float v2 = s.ic2eq + c.a2 * s.ic1eq + c.a3 * v3;
    s.ic1eq = 2.0f * v1 - s.ic1eq;
    s.ic2eq = 2.0f * v2 - s.ic2eq;
    return { v2, v1, v0 - kButterworthK * v1 - v2 };
}

/** 2nd-order all-pass matching the LR4 sum at the same g (phase compensation). */
inline float allPass (SvfState& s, const Coeffs& c, float v0) noexcept
{
    return v0 - 2.0f * kButterworthK * tick (s, c, v0).band;
}

/** Shared first section + separate second sections for each branch. */
struct Stage
{
    SvfState first {}, low {}, high {};
};

inline void splitStage (Stage& st, const Coeffs& c, float x, float& low, float& high) noexcept
{
    const auto t = tick (st.first, c, x);
    low  = tick (st.low,  c, t.low).low;
    high = tick (st.high, c, t.high).high;
}

template <typename Fn>
void forEachState (Stage& st, Fn&& fn) { fn (st.first); fn (st.low); fn (st.high); }

} // namespace lr4detail

// ------------------------------------------------------------------------- 2-way

struct Split2Out { float low = 0.0f, high = 0.0f; };

struct Lr4Split2
{
    double sampleRate = 48000.0;
    OnePoleSmoother g {};
    float targetG = 0.0f;
    lr4detail::Stage stage {};
};

inline Lr4Split2 makeLr4Split2 (float fc, double sampleRate, float smoothMs = kCrossoverSmoothMs) noexcept
{
    Lr4Split2 s;
    s.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    const float f = std::clamp (std::isfinite (fc) ? fc : 100.0f, kMinCrossoverHz, static_cast<float> (0.45 * s.sampleRate));
    s.targetG = lr4detail::prewarp (f, s.sampleRate);
    s.g = makeOnePole (smoothMs, s.sampleRate, s.targetG);
    return s;
}

/** Glides to the new cutoff over the smoothing time (no zipper, no state reset). */
inline void setCutoff (Lr4Split2& s, float fc) noexcept
{
    if (! std::isfinite (fc))
        return;
    s.targetG = lr4detail::prewarp (std::clamp (fc, kMinCrossoverHz, static_cast<float> (0.45 * s.sampleRate)), s.sampleRate);
}

inline Split2Out process (Lr4Split2& s, float x) noexcept
{
    const auto c = lr4detail::coeffsFor (step (s.g, s.targetG));
    Split2Out out;
    lr4detail::splitStage (s.stage, c, x, out.low, out.high);
    return out;
}

// ------------------------------------------------------------------------- 4-band

using Bands4 = std::array<float, 4>;

/** Tree split: f1 -> (b1 | rest), f2 -> (b2 | rest), f3 -> (b3 | b4).
 *  b1 gets all-passes at f2 and f3, b2 at f3, so all bands share one phase response and
 *  sum = AP(f1) * AP(f2) * AP(f3): flat magnitude. */
struct Lr4Split4
{
    double sampleRate = 48000.0;
    std::array<OnePoleSmoother, 3> g {};
    std::array<float, 3> targetG {};
    std::array<lr4detail::Stage, 3> stages {};
    SvfState ap1at2 {}, ap1at3 {}, ap2at3 {};
};

inline void setCrossovers (Lr4Split4& s, CrossoverPoints p) noexcept
{
    p = sanitiseCrossovers (p, s.sampleRate);
    s.targetG = { lr4detail::prewarp (p.f1, s.sampleRate),
                  lr4detail::prewarp (p.f2, s.sampleRate),
                  lr4detail::prewarp (p.f3, s.sampleRate) };
}

inline Lr4Split4 makeLr4Split4 (CrossoverPoints p, double sampleRate, float smoothMs = kCrossoverSmoothMs) noexcept
{
    Lr4Split4 s;
    s.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    setCrossovers (s, p);
    for (size_t i = 0; i < 3; ++i)
        s.g[i] = makeOnePole (smoothMs, s.sampleRate, s.targetG[i]);   // start on target: no glide at load
    return s;
}

inline Bands4 process (Lr4Split4& s, float x) noexcept
{
    using namespace lr4detail;
    const Coeffs c1 = coeffsFor (step (s.g[0], s.targetG[0]));
    const Coeffs c2 = coeffsFor (step (s.g[1], s.targetG[1]));
    const Coeffs c3 = coeffsFor (step (s.g[2], s.targetG[2]));

    float b1, rest1, b2, rest2, b3, b4;
    splitStage (s.stages[0], c1, x,     b1, rest1);
    splitStage (s.stages[1], c2, rest1, b2, rest2);
    splitStage (s.stages[2], c3, rest2, b3, b4);

    b1 = allPass (s.ap1at3, c3, allPass (s.ap1at2, c2, b1));
    b2 = allPass (s.ap2at3, c3, b2);
    return { b1, b2, b3, b4 };
}

template <typename Fn>
void forEachState (Lr4Split4& s, Fn&& fn)
{
    for (auto& st : s.stages)
        lr4detail::forEachState (st, fn);
    fn (s.ap1at2);
    fn (s.ap1at3);
    fn (s.ap2at3);
}

/** Returns true if any state had to be reset (raise the UI flag; never log here). */
inline bool resetIfNotFinite (Lr4Split4& s) noexcept
{
    bool bad = false;
    forEachState (s, [&] (SvfState& st) { bad |= ! isFinite (st); });
    if (bad)
        forEachState (s, [] (SvfState& st) { st = {}; });   // reset all: partial reset leaves bands misaligned
    return bad;
}

inline void flushDenormals (Lr4Split4& s) noexcept
{
    forEachState (s, [] (SvfState& st) { flushDenormals (st); });
}

} // namespace strat3::dsp
