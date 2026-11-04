#pragma once
// Topology-preserving-transform state-variable filter (Zavalishin's TPT, in Andrew Simper's
// "linear trapezoidal" formulation). Chosen over direct-form biquads because its states stay
// meaningful when coefficients change every sample, so automation and dynamic EQ do not blow up.
// Bilinear with prewarping: the response at fc is exact; it compresses towards Nyquist.
#include <algorithm>
#include <cmath>
#include <numbers>

#include "dsp/core/Smoother.h"

namespace strat3::dsp {

// Append only: tests and callers index some of these by value.
enum class SvfType { LowPass, HighPass, BandPass, Bell, LowShelf, HighShelf, Notch, AllPass };

/** Filter coefficients. g, k, m0..m2 define the filter; a1..a3 are derived from g and k. */
struct SvfCoeffs
{
    float g = 0.0f, k = 1.0f;
    float m0 = 1.0f, m1 = 0.0f, m2 = 0.0f;   // output mix of input, band and low outputs
    float a1 = 1.0f, a2 = 0.0f, a3 = 0.0f;
};

struct SvfState
{
    float ic1eq = 0.0f, ic2eq = 0.0f;
};

inline SvfCoeffs withDerived (SvfCoeffs c) noexcept
{
    c.a1 = 1.0f / (1.0f + c.g * (c.g + c.k));
    c.a2 = c.g * c.a1;
    c.a3 = c.g * c.a2;
    return c;
}

inline SvfCoeffs passthroughCoeffs() noexcept { return withDerived ({}); }

/** Builds coefficients. Inputs are sanitised rather than asserted, because they can arrive
 *  from automation, presets or the assistant:
 *  fc is clamped to [1 Hz, 0.49 fs] (tan() diverges at Nyquist), Q to [0.025, 40],
 *  gain to +-48 dB. Non-finite input or sampleRate <= 0 yields a passthrough.
 *  BandPass is normalised to 0 dB at fc (for detector side-chains).
 *  AllPass at Q = 1/sqrt(2) equals the sum of an LR4 low/high pair (crossover phase compensation). */
inline SvfCoeffs makeSvfCoeffs (SvfType type, float fc, float q, float gainDb, double sampleRate) noexcept
{
    if (sampleRate <= 0.0 || ! std::isfinite (fc) || ! std::isfinite (q) || ! std::isfinite (gainDb))
        return passthroughCoeffs();

    const double nyquistGuard = 0.49 * sampleRate;
    const double f = std::clamp (static_cast<double> (fc), 1.0, nyquistGuard);
    const double Q = std::clamp (static_cast<double> (q), 0.025, 40.0);
    const double A = std::pow (10.0, std::clamp (static_cast<double> (gainDb), -48.0, 48.0) / 40.0);
    const double w = std::tan (std::numbers::pi * f / sampleRate);

    double g = w, k = 1.0 / Q, m0 = 0.0, m1 = 0.0, m2 = 0.0;
    switch (type)
    {
        case SvfType::LowPass:   m2 = 1.0; break;
        case SvfType::HighPass:  m0 = 1.0; m1 = -k; m2 = -1.0; break;
        case SvfType::BandPass:  m1 = k; break;
        case SvfType::Notch:     m0 = 1.0; m1 = -k; break;
        case SvfType::AllPass:   m0 = 1.0; m1 = -2.0 * k; break;   // (s^2 - ks + 1) / (s^2 + ks + 1)
        case SvfType::Bell:      k = 1.0 / (Q * A); m0 = 1.0; m1 = k * (A * A - 1.0); break;
        case SvfType::LowShelf:  g = w / std::sqrt (A); m0 = 1.0; m1 = k * (A - 1.0); m2 = A * A - 1.0; break;
        case SvfType::HighShelf: g = w * std::sqrt (A); m0 = A * A; m1 = k * (1.0 - A) * A; m2 = 1.0 - A * A; break;
    }

    SvfCoeffs c;
    c.g  = static_cast<float> (g);
    c.k  = static_cast<float> (k);
    c.m0 = static_cast<float> (m0);
    c.m1 = static_cast<float> (m1);
    c.m2 = static_cast<float> (m2);
    return withDerived (c);
}

/** One sample through the filter. No checks: this is the inner loop.
 *  Call resetIfNotFinite()/flushDenormals() once per block instead. */
inline float processSample (SvfState& s, const SvfCoeffs& c, float v0) noexcept
{
    const float v3 = v0 - s.ic2eq;
    const float v1 = c.a1 * s.ic1eq + c.a2 * v3;
    const float v2 = s.ic2eq + c.a2 * s.ic1eq + c.a3 * v3;
    s.ic1eq = 2.0f * v1 - s.ic1eq;
    s.ic2eq = 2.0f * v2 - s.ic2eq;
    return c.m0 * v0 + c.m1 * v1 + c.m2 * v2;
}

inline bool isFinite (const SvfState& s) noexcept
{
    return std::isfinite (s.ic1eq) && std::isfinite (s.ic2eq);
}

/** Zeroes a poisoned state. Returns true if a reset happened, so the caller can raise the
 *  UI's "non-finite audio" flag (never log from the audio thread). */
inline bool resetIfNotFinite (SvfState& s) noexcept
{
    if (isFinite (s))
        return false;
    s = {};
    return true;
}

/** Zeroes states that have decayed to near-zero, before they reach subnormal range.
 *  Belt-and-braces alongside ScopedNoDenormals: FTZ/DAZ handling differs on ARM hosts. */
inline void flushDenormals (SvfState& s) noexcept
{
    if (std::abs (s.ic1eq) < kSnapEpsilon) s.ic1eq = 0.0f;
    if (std::abs (s.ic2eq) < kSnapEpsilon) s.ic2eq = 0.0f;
}

/** SVF whose coefficients glide per sample towards a target (one-pole on g, k, m0..m2).
 *  Smoothing g and k directly is safe in TPT form: any g > 0, k > 0 is stable, so every
 *  intermediate set is a valid filter, even when gliding between filter types. */
struct ModulatedSvf
{
    SvfCoeffs current = passthroughCoeffs();
    SvfCoeffs target  = passthroughCoeffs();
    float     coeff   = 0.0f;
    SvfState  state {};
};

inline ModulatedSvf makeModulatedSvf (double sampleRate, float timeMs, const SvfCoeffs& initial) noexcept
{
    ModulatedSvf f;
    f.current = initial;
    f.target  = initial;
    f.coeff   = makeOnePole (timeMs, sampleRate, 0.0f).coeff;
    return f;
}

inline void setTarget (ModulatedSvf& f, const SvfCoeffs& target) noexcept { f.target = target; }

inline float process (ModulatedSvf& f, float x) noexcept
{
    auto glide = [a = f.coeff] (float& cur, float tgt)
    {
        const float next = tgt + a * (cur - tgt);
        cur = (std::abs (next - tgt) <= kSnapEpsilon || next == cur) ? tgt : next;   // see step(OnePoleSmoother&)
    };
    glide (f.current.g,  f.target.g);
    glide (f.current.k,  f.target.k);
    glide (f.current.m0, f.target.m0);
    glide (f.current.m1, f.target.m1);
    glide (f.current.m2, f.target.m2);
    f.current = withDerived (f.current);
    return processSample (f.state, f.current, x);
}

} // namespace strat3::dsp
