#pragma once
// LOW layer: processing below low.xover only.
//   1. LR4 split at low.xover (per channel).
//   2. Shaper on the low band, stereo-linked:
//        r = 20 log10 (fast / slow)          fast 1 ms, slow 50 ms envelopes
//        r > 0 (onset):  gain = punch   * r     i.e. g = (fast/slow)^punch
//        r < 0 (decay):  gain = sustain * -r
//      clamped to +-12 dB. Tight adds a downward expansion when the low band sits more than
//      6 dB below its recent peak (300 ms release), up to -24 dB.
//   3. Mono below: side split (LR4) at low.monoFreq, its low part faded out.
//
// Detector: both envelopes follow a 25 ms sliding maximum of |low|, not |low| itself. |x|
// of a 20 Hz+ sine peaks at least every 25 ms, so the held level of a steady tone is exactly
// constant; a 1 ms envelope on the raw waveform would ripple every cycle (gain modulation =
// distortion) and read steady bass as a constant transient. Cost: decay detection lags by
// up to 25 ms, which is short against bass decays.
//
// The layer also returns a reference: the input through the same all-passes with no
// processing. The depth blend must mix against this, not the raw input: LR4 low + high is an
// all-pass (-180 degrees at the crossover), and blending it with the dry input would notch.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

#include "dsp/core/Smoother.h"
#include "dsp/core/SvfTpt.h"
#include "dsp/crossover/Lr4Crossover.h"

namespace strat3::dsp {

inline constexpr float kLowMinHz = 40.0f;
inline constexpr float kLowMaxHz = 200.0f;
inline constexpr float kLowFastMs = 1.0f;
inline constexpr float kLowSlowMs = 50.0f;
inline constexpr float kLowHoldMs = 25.0f;           // half a period of 20 Hz
inline constexpr float kLowShapeMaxDb = 12.0f;
inline constexpr float kLowTightKneeDb = 6.0f;       // [INFERRED] own choice: normal decays pass
inline constexpr float kLowTightMaxDb = 24.0f;
inline constexpr float kLowTightReleaseMs = 300.0f;
inline constexpr float kLowAmountMs = 10.0f;         // punch / sustain / tight changes
inline constexpr float kLowMonoRampMs = 20.0f;
inline constexpr float kLowLevelFloor = 1e-7f;       // -140 dB: ratios of silence read as 1

struct LowSettings
{
    float xoverHz = 100.0f;
    float punch = 0.0f;     // -100..100 %
    float sustain = 0.0f;   // -100..100 %
    float tight = 0.0f;     // 0..100 %
    bool monoOn = false;
    float monoHz = 100.0f;
    bool operator== (const LowSettings&) const = default;
};

/** Running maximum over the last `window` samples (monotonic deque in a fixed ring).
 *  Amortised O(1); allocation only in prepare. */
struct SlidingMax
{
    std::vector<float> value;
    std::vector<std::int64_t> time;
    std::uint64_t mask = 0, head = 0, tail = 0;
    std::int64_t now = 0;
    std::int64_t window = 1;
};

inline void prepare (SlidingMax& m, int window)
{
    m.window = std::max (1, window);
    std::uint64_t cap = 1;
    while (cap < static_cast<std::uint64_t> (m.window) + 1)
        cap <<= 1;
    m.value.assign (cap, 0.0f);
    m.time.assign (cap, 0);
    m.mask = cap - 1;
    m.head = m.tail = 0;
    m.now = 0;
}

inline void reset (SlidingMax& m) noexcept
{
    m.head = m.tail = 0;
    m.now = 0;
}

inline float push (SlidingMax& m, float x) noexcept
{
    // Entries <= x can never be the maximum again while x is in the window.
    while (m.tail != m.head && m.value[(m.tail - 1) & m.mask] <= x)
        --m.tail;
    m.value[m.tail & m.mask] = x;
    m.time[m.tail & m.mask] = m.now;
    ++m.tail;
    while (m.time[m.head & m.mask] <= m.now - m.window)
        ++m.head;
    ++m.now;
    return m.value[m.head & m.mask];
}

struct LowLayer
{
    double sampleRate = 48000.0;
    LowSettings settings {};                 // sanitised targets

    std::array<Lr4Split2, 2> xover {};       // [L, R]
    std::array<Lr4Split2, 2> monoWet {};     // [mid, side] of the processed signal
    std::array<Lr4Split2, 2> monoRef {};     // [mid, side] of the reference

    SlidingMax hold {};
    OnePoleSmoother fast {}, slow {};
    float peak = 0.0f, peakRelease = 0.0f;

    OnePoleSmoother punch {}, sustain {}, tight {};   // fractions
    LinearRamp sideLow {};                            // 1 = side kept, 0 = mono below
    float gainDb = 0.0f;
};

struct LowFrame
{
    float l = 0.0f, r = 0.0f;         // processed
    float refL = 0.0f, refR = 0.0f;   // same all-passes, unprocessed: the depth blend's dry
};

namespace lowdetail {

inline float finiteOr (float v, float keep) noexcept { return std::isfinite (v) ? v : keep; }

template <typename Fn>
void forEachSvf (LowLayer& low, Fn&& fn)
{
    for (auto* group : { &low.xover, &low.monoWet, &low.monoRef })
        for (auto& s : *group)
            lr4detail::forEachState (s.stage, fn);
}

/** Mid/side split at monoHz; `sideLowGain` scales the side below it. Mid goes through the
 *  same split and is summed back (= the LR4 all-pass), and LR4 low and high are in phase, so
 *  mid and side keep their phase relationship at every frequency. */
inline void monoStage (std::array<Lr4Split2, 2>& st, float& l, float& r, float sideLowGain) noexcept
{
    const auto m = process (st[0], (l + r) * 0.5f);
    const auto s = process (st[1], (l - r) * 0.5f);
    const float mid = m.low + m.high;
    const float side = sideLowGain * s.low + s.high;
    l = mid + side;
    r = mid - side;
}

inline float shaperGainDb (LowLayer& low, float punch, float sustain, float tight) noexcept
{
    const float f = low.fast.state + kLowLevelFloor;
    const float s = low.slow.state + kLowLevelFloor;
    const float r = 20.0f * std::log10 (f / s);
    const float shape = std::clamp (r > 0.0f ? punch * r : -sustain * r, -kLowShapeMaxDb, kLowShapeMaxDb);
    if (tight == 0.0f)
        return shape;
    const float below = 20.0f * std::log10 ((low.peak + kLowLevelFloor) / f);
    return shape + std::max (-tight * std::max (0.0f, below - kLowTightKneeDb), -kLowTightMaxDb);
}

} // namespace lowdetail

inline int latencySamples (const LowLayer&) noexcept { return 0; }
inline float lowGainDb (const LowLayer& low) noexcept { return low.gainDb; }

/** Audio thread, once per block. Non-finite fields keep their previous value; the rest are
 *  clamped to the parameter ranges. */
inline void setSettings (LowLayer& low, const LowSettings& s) noexcept
{
    using lowdetail::finiteOr;
    auto& t = low.settings;
    t.xoverHz = std::clamp (finiteOr (s.xoverHz, t.xoverHz), kLowMinHz, kLowMaxHz);
    t.monoHz  = std::clamp (finiteOr (s.monoHz, t.monoHz), kLowMinHz, kLowMaxHz);
    t.punch   = std::clamp (finiteOr (s.punch, t.punch), -100.0f, 100.0f);
    t.sustain = std::clamp (finiteOr (s.sustain, t.sustain), -100.0f, 100.0f);
    t.tight   = std::clamp (finiteOr (s.tight, t.tight), 0.0f, 100.0f);
    t.monoOn  = s.monoOn;

    for (auto& x : low.xover)
        setCutoff (x, t.xoverHz);
    for (auto* group : { &low.monoWet, &low.monoRef })
        for (auto& x : *group)
            setCutoff (x, t.monoHz);
    setTarget (low.sideLow, t.monoOn ? 0.0f : 1.0f);
}

/** Jump every glide to its target: after prepare / state load only. */
inline void snapToTargets (LowLayer& low) noexcept
{
    for (auto* group : { &low.xover, &low.monoWet, &low.monoRef })
        for (auto& x : *group)
            x.g.state = x.targetG;
    low.punch.state = low.settings.punch * 0.01f;
    low.sustain.state = low.settings.sustain * 0.01f;
    low.tight.state = low.settings.tight * 0.01f;
    low.sideLow = makeLinearRamp (kLowMonoRampMs, low.sampleRate, low.settings.monoOn ? 0.0f : 1.0f);
}

/** prepareToPlay: allocates the detector ring, resets all state, keeps the settings. */
inline void prepare (LowLayer& low, double sampleRate)
{
    low.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    const auto& t = low.settings;
    for (auto& x : low.xover)
        x = makeLr4Split2 (t.xoverHz, low.sampleRate);
    for (auto* group : { &low.monoWet, &low.monoRef })
        for (auto& x : *group)
            x = makeLr4Split2 (t.monoHz, low.sampleRate);

    prepare (low.hold, static_cast<int> (std::lround (static_cast<double> (kLowHoldMs) * 0.001 * low.sampleRate)));
    low.fast = makeOnePole (kLowFastMs, low.sampleRate, 0.0f);
    low.slow = makeOnePole (kLowSlowMs, low.sampleRate, 0.0f);
    low.peak = 0.0f;
    low.peakRelease = static_cast<float> (std::exp (-1.0 / (static_cast<double> (kLowTightReleaseMs) * 0.001 * low.sampleRate)));

    low.punch   = makeOnePole (kLowAmountMs, low.sampleRate, t.punch * 0.01f);
    low.sustain = makeOnePole (kLowAmountMs, low.sampleRate, t.sustain * 0.01f);
    low.tight   = makeOnePole (kLowAmountMs, low.sampleRate, t.tight * 0.01f);
    low.sideLow = makeLinearRamp (kLowMonoRampMs, low.sampleRate, t.monoOn ? 0.0f : 1.0f);
    low.gainDb = 0.0f;
}

inline LowFrame process (LowLayer& low, float l, float r) noexcept
{
    const auto sl = process (low.xover[0], l);
    const auto sr = process (low.xover[1], r);

    // Stereo-linked detector: one gain for both channels, so the image never shifts.
    const float held = push (low.hold, std::max (std::abs (sl.low), std::abs (sr.low)));
    step (low.fast, held);
    step (low.slow, held);
    low.peak = std::max (held, low.peak * low.peakRelease);
    if (low.peak < kSnapEpsilon)
        low.peak = 0.0f;   // the release multiply would otherwise decay into subnormals

    const float punch   = step (low.punch, low.settings.punch * 0.01f);
    const float sustain = step (low.sustain, low.settings.sustain * 0.01f);
    const float tight   = step (low.tight, low.settings.tight * 0.01f);

    // Exactly 1 when every amount is 0: the neutral layer is bit-identical to its reference.
    float g = 1.0f;
    low.gainDb = 0.0f;
    if (punch != 0.0f || sustain != 0.0f || tight != 0.0f)
    {
        low.gainDb = lowdetail::shaperGainDb (low, punch, sustain, tight);
        g = std::exp (low.gainDb * static_cast<float> (std::numbers::ln10 / 20.0));
    }

    LowFrame out;
    out.l = g * sl.low + sl.high;
    out.r = g * sr.low + sr.high;
    out.refL = sl.low + sl.high;
    out.refR = sr.low + sr.high;

    lowdetail::monoStage (low.monoWet, out.l, out.r, step (low.sideLow));
    lowdetail::monoStage (low.monoRef, out.refL, out.refR, 1.0f);
    return out;
}

/** Visits every float of running state (filters, envelopes, live detector entries). */
template <typename Fn>
void forEachFloatState (LowLayer& low, Fn&& fn)
{
    lowdetail::forEachSvf (low, [&] (SvfState& s) { fn (s.ic1eq); fn (s.ic2eq); });
    fn (low.fast.state);
    fn (low.slow.state);
    fn (low.peak);
    for (auto i = low.hold.head; i != low.hold.tail; ++i)
        fn (low.hold.value[i & low.hold.mask]);
}

/** Returns true if any state had to be reset (raise the UI flag; never log here). */
inline bool resetIfNotFinite (LowLayer& low) noexcept
{
    bool bad = false;
    forEachFloatState (low, [&] (float v) { bad |= ! std::isfinite (v); });
    if (! bad)
        return false;

    lowdetail::forEachSvf (low, [] (SvfState& s) { s = {}; });
    reset (low.hold);
    low.fast.state = low.slow.state = 0.0f;
    low.peak = 0.0f;
    low.gainDb = 0.0f;
    return true;
}

inline void flushDenormals (LowLayer& low) noexcept
{
    lowdetail::forEachSvf (low, [] (SvfState& s) { flushDenormals (s); });
}

} // namespace strat3::dsp
