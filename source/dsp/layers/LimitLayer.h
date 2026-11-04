#pragma once
// LIMIT layer (broadband, stereo). Per sample:
//   1. 3-band LR4 split at 120 Hz / 4 kHz (low gets the 4 kHz all-pass: flat sum). Always runs,
//      so every style has the same phase and style switches are click-free crossfades.
//      ref = sum of the bands: the dry the depth blend must use.
//   2. DENSE only: per-band pre-limit at half ratio (each band takes half of its own overshoot
//      of the ceiling, 1 ms attack, release = limit.release), so roughly half the reduction
//      happens per band before the broadband stage [INFERRED own tuning].
//   3. wet = band sum * makeup (-thresh: maximiser convention) * transient emphasis.
//   4. 2x IIR oversampler, always running (constant latency/phase); CLIP adds a cubic soft clip
//      with ceiling = limiter ceiling, residual form with first-order ADAA.
//   5. Brickwall to the ceiling (sample peak): needed gain in dB -> sliding minimum over L+1
//      -> two-stage release: fast follower (instant attack, release/10) and slow follower
//      (release time both ways), take the lower -> box filter over L+1 -> applied to the
//      signal delayed by L. Every value entering the box is <= the gain the delayed sample
//      needs, so the average is too: no overshoot by construction.
//   L = 0 / 1.5 / 5 ms (TRACK / MIX / MASTER). With L = 0 the attack is instant: the brickwall
//   then clips overs, and true peak is not guarded (the delivery bar shows "TP UNGUARDED").
// The ceiling the processor passes in should be the CEILING layer's level, so that layer only
// catches inter-sample peaks.
// "Transient" emphasis: up to +6 dB on onsets (fast vs slow envelope ratio, 1 dB deadband),
// before the limiter [INFERRED own design].
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "dsp/core/CubicClip.h"
#include "dsp/core/DelayLine.h"
#include "dsp/core/Oversampler.h"
#include "dsp/core/Smoother.h"
#include "dsp/crossover/Lr4Crossover.h"
#include "dsp/layers/LowLayer.h"         // SlidingMax
#include "dsp/layers/MultibandLayer.h"   // mbdetail::follow, coeffFor

namespace strat3::dsp {

enum class LimitStyle { Clean, Dense, Clip };

inline constexpr std::array<float, 3> kLimitLookaheadMs { 0.0f, 1.5f, 5.0f };   // TRACK, MIX, MASTER
inline constexpr float kLimitRampMs = 20.0f;
inline constexpr float kLimitFastReleaseDiv = 10.0f;
inline constexpr float kDenseLowHz = 120.0f;
inline constexpr float kDenseHighHz = 4000.0f;
inline constexpr float kDenseRatio = 0.5f;
inline constexpr float kDenseAttackMs = 1.0f;
inline constexpr float kTeFastAttackMs = 0.5f, kTeFastReleaseMs = 20.0f;
inline constexpr float kTeSlowAttackMs = 20.0f, kTeSlowReleaseMs = 200.0f;
inline constexpr float kTeDeadbandDb = 1.0f;
inline constexpr float kTeMaxDb = 6.0f;
inline constexpr double kLimitAdaaMinDelta = 1e-5;
inline constexpr int kLimitBoxResync = 65536;   // recompute the box sum exactly this often

struct LimitSettings
{
    LimitStyle style = LimitStyle::Clean;
    float threshDb = 0.0f;           // -24..0, makeup gain = -thresh
    float ceilingDb = 0.0f;          // -24..0, sample-peak output limit
    float releaseMs = 100.0f;        // 1..1000
    float linkPercent = 100.0f;      // 0..100
    float transientPercent = 0.0f;   // 0..100
    bool operator== (const LimitSettings&) const = default;
};

struct LimitFrame
{
    float l = 0.0f, r = 0.0f;         // processed
    float refL = 0.0f, refR = 0.0f;   // same split, oversampler and delay, unprocessed
};

struct LimitChannel
{
    lr4detail::Stage splitLow {}, splitHigh {};
    SvfState lowAtHigh {};
    std::array<float, 3> denseGr {};   // dB
    UpChain upWet {}, upRef {};
    DownChain downWet {}, downRef {};
    double prevX = 0.0, prevR = 0.0;   // clip ADAA
    float prevInvT = -1.0f;
    SlidingMax hold {};                // of -needed dB
    float fast = 0.0f, slow = 0.0f;    // release stages, dB
    std::vector<float> box {};
    double boxSum = 0.0;
    int boxPos = 0;
    float gainDb = 0.0f;
    DelayLine wet {}, ref {};
};

struct LimitLayer
{
    double sampleRate = 48000.0;
    LimitSettings settings {};
    int lookahead = 0;
    OsConfig os {};
    lr4detail::Coeffs cLow {}, cHigh {};
    std::array<LimitChannel, 2> ch {};
    LinearRamp thresh {}, ceiling {}, link {}, transient {}, dense {}, clip {};
    float lastThresh = 1.0f, inGain = 1.0f;       // cached dB -> linear
    float lastCeiling = 1.0f, ceilingLin = 1.0f;
    float fastCoeff = 0.0f, slowCoeff = 0.0f, denseAttack = 0.0f, denseRelease = 0.0f;
    float teFastA = 0.0f, teFastR = 0.0f, teSlowA = 0.0f, teSlowR = 0.0f;
    float teFast = 0.0f, teSlow = 0.0f;
    int boxCounter = 0;
};

/** Current brickwall gain (dB, <= 0) of channel 0 / 1. Excludes makeup and DENSE. */
inline float limitGainDb (const LimitLayer& lim, std::size_t channel) noexcept { return lim.ch[channel].gainDb; }

/** Current DENSE gain (dB) of band 0..2, channel 0 / 1. */
inline float denseGainDb (const LimitLayer& lim, std::size_t band, std::size_t channel) noexcept
{
    return lim.ch[channel].denseGr[band];
}

inline int latencySamples (const LimitLayer& lim) noexcept { return lim.lookahead + latencySamples (lim.os); }

namespace limdetail {

inline float finiteOr (float v, float keep) noexcept { return std::isfinite (v) ? v : keep; }
inline float dbToGain (float db) noexcept { return std::exp (db * 0.11512925f); }   // ln 10 / 20
inline float gainToDb (float g) noexcept { return 20.0f * std::log10 (std::max (g, 1e-12f)); }

inline void updateReleaseCoeffs (LimitLayer& lim) noexcept
{
    lim.fastCoeff = mbdetail::coeffFor (lim.settings.releaseMs / kLimitFastReleaseDiv, lim.sampleRate);
    lim.slowCoeff = mbdetail::coeffFor (lim.settings.releaseMs, lim.sampleRate);
    lim.denseRelease = lim.slowCoeff;
}

inline void resetChannel (LimitChannel& c) noexcept
{
    c.splitLow = {};
    c.splitHigh = {};
    c.lowAtHigh = {};
    c.denseGr = {};
    reset (c.upWet);
    reset (c.upRef);
    reset (c.downWet);
    reset (c.downRef);
    c.prevX = c.prevR = 0.0;
    c.prevInvT = -1.0f;
    reset (c.hold);
    c.fast = c.slow = 0.0f;
    std::fill (c.box.begin(), c.box.end(), 0.0f);
    c.boxSum = 0.0;
    c.boxPos = 0;
    c.gainDb = 0.0f;
    reset (c.wet);
    reset (c.ref);
}

/** First-order ADAA of the cubic clip residual at u = x / T, returned in the x domain. */
inline float clipAdaa (LimitChannel& c, float invT, float t, double x) noexcept
{
    const double it = invT;
    if (invT != c.prevInvT)
    {
        c.prevR = cubicClipIntegral (it * c.prevX, 1.0);
        c.prevInvT = invT;
    }
    const double u0 = it * c.prevX, u1 = it * x;
    const double r1 = cubicClipIntegral (u1, 1.0);
    const double du = u1 - u0;
    const double r = std::abs (du) > kLimitAdaaMinDelta ? (r1 - c.prevR) / du : cubicClipResidual (0.5 * (u0 + u1), 1.0);
    c.prevX = x;
    c.prevR = r1;
    return static_cast<float> (r) * t;
}

} // namespace limdetail

/** Audio thread, once per block. Clamps; non-finite fields keep their previous value. */
inline void setSettings (LimitLayer& lim, const LimitSettings& in) noexcept
{
    using limdetail::finiteOr;
    auto& t = lim.settings;
    const float oldRelease = t.releaseMs;
    t.style = in.style;
    t.threshDb = std::clamp (finiteOr (in.threshDb, t.threshDb), -24.0f, 0.0f);
    t.ceilingDb = std::clamp (finiteOr (in.ceilingDb, t.ceilingDb), -24.0f, 0.0f);
    t.releaseMs = std::clamp (finiteOr (in.releaseMs, t.releaseMs), 1.0f, 1000.0f);
    t.linkPercent = std::clamp (finiteOr (in.linkPercent, t.linkPercent), 0.0f, 100.0f);
    t.transientPercent = std::clamp (finiteOr (in.transientPercent, t.transientPercent), 0.0f, 100.0f);
    if (t.releaseMs != oldRelease)
        limdetail::updateReleaseCoeffs (lim);

    setTarget (lim.thresh, t.threshDb);
    setTarget (lim.ceiling, t.ceilingDb);
    setTarget (lim.link, t.linkPercent * 0.01f);
    setTarget (lim.transient, t.transientPercent * 0.01f);
    setTarget (lim.dense, t.style == LimitStyle::Dense ? 1.0f : 0.0f);
    setTarget (lim.clip, t.style == LimitStyle::Clip ? 1.0f : 0.0f);
}

/** Jump ramps to their targets: after prepare / state load only. */
inline void snapToTargets (LimitLayer& lim) noexcept
{
    const auto& t = lim.settings;
    lim.thresh = makeLinearRamp (kLimitRampMs, lim.sampleRate, t.threshDb);
    lim.ceiling = makeLinearRamp (kLimitRampMs, lim.sampleRate, t.ceilingDb);
    lim.link = makeLinearRamp (kLimitRampMs, lim.sampleRate, t.linkPercent * 0.01f);
    lim.transient = makeLinearRamp (kLimitRampMs, lim.sampleRate, t.transientPercent * 0.01f);
    lim.dense = makeLinearRamp (kLimitRampMs, lim.sampleRate, t.style == LimitStyle::Dense ? 1.0f : 0.0f);
    lim.clip = makeLinearRamp (kLimitRampMs, lim.sampleRate, t.style == LimitStyle::Clip ? 1.0f : 0.0f);
    limdetail::updateReleaseCoeffs (lim);
}

/** prepareToPlay (allocates). Resets all state, keeps the settings.
 *  @param lookaheadMs one of kLimitLookaheadMs (from the latency mode). */
inline void prepare (LimitLayer& lim, double sampleRate, float lookaheadMs)
{
    lim.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    const float la = std::isfinite (lookaheadMs) ? std::clamp (lookaheadMs, 0.0f, 20.0f) : 0.0f;
    lim.lookahead = static_cast<int> (std::lround (static_cast<double> (la) * 0.001 * lim.sampleRate));
    lim.os = makeOsConfig (2, OsKind::Iir, lim.sampleRate);
    lim.cLow = lr4detail::coeffsFor (lr4detail::prewarp (kDenseLowHz, lim.sampleRate));
    lim.cHigh = lr4detail::coeffsFor (lr4detail::prewarp (kDenseHighHz, lim.sampleRate));
    lim.denseAttack = mbdetail::coeffFor (kDenseAttackMs, lim.sampleRate);
    lim.teFastA = mbdetail::coeffFor (kTeFastAttackMs, lim.sampleRate);
    lim.teFastR = mbdetail::coeffFor (kTeFastReleaseMs, lim.sampleRate);
    lim.teSlowA = mbdetail::coeffFor (kTeSlowAttackMs, lim.sampleRate);
    lim.teSlowR = mbdetail::coeffFor (kTeSlowReleaseMs, lim.sampleRate);
    lim.teFast = lim.teSlow = 0.0f;
    lim.boxCounter = 0;
    for (auto& c : lim.ch)
    {
        prepare (c.hold, lim.lookahead + 1);
        c.box.assign (static_cast<std::size_t> (lim.lookahead + 1), 0.0f);
        prepare (c.wet, static_cast<std::size_t> (lim.lookahead));
        prepare (c.ref, static_cast<std::size_t> (lim.lookahead));
        setDelay (c.wet, static_cast<std::size_t> (lim.lookahead));
        setDelay (c.ref, static_cast<std::size_t> (lim.lookahead));
        limdetail::resetChannel (c);
    }
    lim.lastThresh = lim.lastCeiling = 1.0f;   // impossible dB values: force the first conversion
    snapToTargets (lim);
}

inline LimitFrame process (LimitLayer& lim, float inL, float inR) noexcept
{
    using limdetail::dbToGain;
    using limdetail::gainToDb;
    using mbdetail::follow;

    const float thr = step (lim.thresh);
    if (thr != lim.lastThresh)
    {
        lim.lastThresh = thr;
        lim.inGain = dbToGain (-thr);
    }
    const float ceilDb = step (lim.ceiling);
    if (ceilDb != lim.lastCeiling)
    {
        lim.lastCeiling = ceilDb;
        lim.ceilingLin = dbToGain (ceilDb);
    }
    const float link = step (lim.link);
    const float te = step (lim.transient);
    const float denseAmt = step (lim.dense);
    const float clipAmt = step (lim.clip);

    // Transient emphasis: gain-invariant ratio of a fast and a slow peak envelope.
    const float det = std::max (std::abs (inL), std::abs (inR));
    lim.teFast = follow (lim.teFast, det, det > lim.teFast ? lim.teFastA : lim.teFastR);
    lim.teSlow = follow (lim.teSlow, det, det > lim.teSlow ? lim.teSlowA : lim.teSlowR);
    float gte = 1.0f;
    if (te > 0.0f)
    {
        const float ratioDb = gainToDb ((lim.teFast + 1e-9f) / (lim.teSlow + 1e-9f));
        gte = dbToGain (te * std::clamp (ratioDb - kTeDeadbandDb, 0.0f, kTeMaxDb));
    }
    const float pre = lim.inGain * gte;

    // Split.
    std::array<std::array<float, 3>, 2> bands {};
    std::array<float, 2> refIn {};
    const std::array<float, 2> in { inL, inR };
    for (std::size_t c = 0; c < 2; ++c)
    {
        auto& st = lim.ch[c];
        float low, rest, mid, high;
        lr4detail::splitStage (st.splitLow, lim.cLow, in[c], low, rest);
        lr4detail::splitStage (st.splitHigh, lim.cHigh, rest, mid, high);
        low = lr4detail::allPass (st.lowAtHigh, lim.cHigh, low);
        bands[c] = { low, mid, high };
        refIn[c] = (low + mid) + high;
    }

    // DENSE band pre-limit.
    std::array<std::array<float, 3>, 2> bandGain {};
    for (auto& g : bandGain)
        g.fill (1.0f);
    if (denseAmt > 0.0f)
    {
        for (std::size_t b = 0; b < 3; ++b)
        {
            const float lMag = std::abs (bands[0][b]) * pre, rMag = std::abs (bands[1][b]) * pre;
            const float both = std::max (lMag, rMag);
            for (std::size_t c = 0; c < 2; ++c)
            {
                const float level = (1.0f - link) * (c == 0 ? lMag : rMag) + link * both;
                const float target = level > lim.ceilingLin ? kDenseRatio * (ceilDb - gainToDb (level)) : 0.0f;
                auto& gr = lim.ch[c].denseGr[b];
                gr = follow (gr, target, target < gr ? lim.denseAttack : lim.denseRelease);
                bandGain[c][b] = gr == 0.0f ? 1.0f : dbToGain (denseAmt * gr);
            }
        }
    }
    else
        for (auto& c : lim.ch)
            c.denseGr = {};

    // Wet, oversampled clip, and the reference through the same chain.
    std::array<float, 2> wet {}, ref {};
    const float invT = 1.0f / lim.ceilingLin;
    for (std::size_t c = 0; c < 2; ++c)
    {
        auto& st = lim.ch[c];
        const auto& b = bands[c];
        const auto& g = bandGain[c];
        const float w = ((b[0] * g[0] + b[1] * g[1]) + b[2] * g[2]) * pre;

        float v[4], vr[4];
        const int n = upsample (lim.os, st.upWet, w, v);
        upsample (lim.os, st.upRef, refIn[c], vr);
        ref[c] = downsample (lim.os, st.downRef, vr);
        if (clipAmt > 0.0f)
        {
            for (int i = 0; i < n; ++i)
                v[i] += clipAmt * limdetail::clipAdaa (st, invT, lim.ceilingLin, v[i]);
        }
        else
        {
            st.prevX = v[n - 1];
            st.prevInvT = -1.0f;
        }
        wet[c] = downsample (lim.os, st.downWet, v);
    }

    // Brickwall.
    const float both = std::max (std::abs (wet[0]), std::abs (wet[1]));
    const bool resync = ++lim.boxCounter >= kLimitBoxResync;
    if (resync)
        lim.boxCounter = 0;
    LimitFrame out;
    for (std::size_t c = 0; c < 2; ++c)
    {
        auto& st = lim.ch[c];
        const float p = (1.0f - link) * std::abs (wet[c]) + link * both;
        const float need = p > lim.ceilingLin ? ceilDb - gainToDb (p) : 0.0f;
        const float m = -push (st.hold, -need);
        st.fast = m < st.fast ? m : follow (st.fast, m, lim.fastCoeff);
        st.slow = follow (st.slow, m, lim.slowCoeff);
        const float gr = std::min (st.fast, st.slow);

        auto& slot = st.box[static_cast<std::size_t> (st.boxPos)];
        st.boxSum += static_cast<double> (gr) - static_cast<double> (slot);
        slot = gr;
        if (++st.boxPos == static_cast<int> (st.box.size()))
            st.boxPos = 0;
        if (resync)   // running sums drift; the exact sum costs L+1 adds every 64k samples
        {
            double s = 0.0;
            for (float v : st.box)
                s += static_cast<double> (v);
            st.boxSum = s;
        }
        st.gainDb = static_cast<float> (st.boxSum / static_cast<double> (st.box.size()));

        const float delayed = process (st.wet, wet[c]);
        const float y = st.gainDb == 0.0f ? delayed : delayed * dbToGain (st.gainDb);
        const float r = process (st.ref, ref[c]);
        (c == 0 ? out.l : out.r) = y;
        (c == 0 ? out.refL : out.refR) = r;
    }
    return out;
}

template <typename Fn>
void forEachFloatState (LimitLayer& lim, Fn&& fn)
{
    for (auto& c : lim.ch)
    {
        for (auto* s : { &c.splitLow.first, &c.splitLow.low, &c.splitLow.high,
                         &c.splitHigh.first, &c.splitHigh.low, &c.splitHigh.high, &c.lowAtHigh })
        {
            fn (s->ic1eq);
            fn (s->ic2eq);
        }
        for (auto& v : c.denseGr) fn (v);
        forEachFloatState (c.upWet, fn);
        forEachFloatState (c.upRef, fn);
        forEachFloatState (c.downWet, fn);
        forEachFloatState (c.downRef, fn);
        float px = static_cast<float> (c.prevX), pr = static_cast<float> (c.prevR);
        fn (px);
        fn (pr);
        fn (c.fast);
        fn (c.slow);
        fn (c.gainDb);
        for (auto& v : c.box) fn (v);
    }
    fn (lim.teFast);
    fn (lim.teSlow);
}

/** Returns true if any state had to be reset (raise the UI flag; never log here). */
inline bool resetIfNotFinite (LimitLayer& lim) noexcept
{
    bool bad = ! std::isfinite (lim.teFast) || ! std::isfinite (lim.teSlow);
    for (auto& c : lim.ch)
    {
        // A NaN that reached the delay lines also poisoned the IIR oversampler states, so the
        // checks below catch it and resetChannel() clears the lines (no O(capacity) scan here).
        bad |= ! std::isfinite (c.boxSum);
        bad |= ! std::isfinite (c.prevX) || ! std::isfinite (c.prevR);
        bad |= resetIfNotFinite (c.upWet) | resetIfNotFinite (c.upRef)
             | resetIfNotFinite (c.downWet) | resetIfNotFinite (c.downRef);
    }
    forEachFloatState (lim, [&] (float v) { bad |= ! std::isfinite (v); });
    if (! bad)
        return false;
    for (auto& c : lim.ch)
        limdetail::resetChannel (c);
    lim.teFast = lim.teSlow = 0.0f;
    return true;
}

inline void flushDenormals (LimitLayer& lim) noexcept
{
    auto flush = [] (float& v) { if (std::abs (v) < kSnapEpsilon) v = 0.0f; };
    for (auto& c : lim.ch)
    {
        for (auto* s : { &c.splitLow.first, &c.splitLow.low, &c.splitLow.high,
                         &c.splitHigh.first, &c.splitHigh.low, &c.splitHigh.high, &c.lowAtHigh })
            flushDenormals (*s);
        for (auto& v : c.denseGr) flush (v);
        flushDenormals (c.upWet);
        flushDenormals (c.upRef);
        flushDenormals (c.downWet);
        flushDenormals (c.downRef);
        if (std::abs (c.prevX) < 1e-15) c.prevX = 0.0;
        if (std::abs (c.prevR) < 1e-30) c.prevR = 0.0;
        flush (c.fast);
        flush (c.slow);
    }
    flush (lim.teFast);
    flush (lim.teSlow);
}

} // namespace strat3::dsp
