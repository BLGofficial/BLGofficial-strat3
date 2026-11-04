#pragma once
// SATURATE layer (band domain). Per band and channel:
//   v   = up(x)                                   (oversampler, 1x / 2x / 4x)
//   ref = down(v)                                 dry through the same chain: depth/mix use it
//   res = down(ADAA[r](k * E(v)) / k), D after the shaper for tape, DC-blocked if asymmetric
//   out = ref + mix * fade * res
// k = 10^(drive/20) - 1, so drive 0 dB is exactly linear and every model has unity small-signal
// gain; r(u) = f(u) - u is the model's distortion residual. [INFERRED] own convention: loud
// material loses level as drive rises (it is saturation); global gain match compensates.
//
// Why ADAA on the residual and not on f: first-order ADAA of a function averages its argument
// over one sample, which on the linear part is a half-sample delay plus HF droop. Applying it
// to r only keeps the dry part exact, so drive 0 / mix 0 is bit-identical to the reference and
// mix has no comb filtering. The residual itself carries the half (high-rate) sample delay.
//
// Models (curve constants are our own tuning [INFERRED]):
//   warm-even  (tanh(u + b) - tanh b) / sech^2 b         b = 0.35, mostly 2nd harmonic
//   odd        tanh u
//   tape       tanh between pre-emphasis E (+12 dB shelf, 2 -> 8 kHz) and its exact inverse D:
//              residual = D(r(k E v)) / k, so highs saturate earlier. No hysteresis (CPU).
//   triode     cubic soft clip c * s(u / c), s(v) = v - 4v^3/27 up to |v| = 1.5, then +-1;
//              c = 0.6 positive, 1.0 negative (positive half clips first, even harmonics)
// Model switches fade the residual out (10 ms), swap, and fade back in.
// The oversampling factor and kind are prepare-time: they change latency.
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

#include "dsp/core/CubicClip.h"
#include "dsp/core/Oversampler.h"
#include "dsp/core/Smoother.h"
#include "dsp/crossover/BandDomain.h"

namespace strat3::dsp {

enum class SatModel { WarmEven, Odd, Tape, Triode };

inline constexpr float  kSatMaxDriveDb = 24.0f;
inline constexpr float  kSatRampMs = 20.0f;
inline constexpr float  kSatModelFadeMs = 10.0f;
inline constexpr double kWarmEvenBias = 0.35;
inline constexpr double kTriodePosClip = 0.6;
inline constexpr double kTriodeNegClip = 1.0;
inline constexpr double kTapeZeroHz = 2000.0;
inline constexpr double kTapePoleHz = 8000.0;
inline constexpr double kSatDcHz = 5.0;
inline constexpr double kAdaaMinDelta = 1e-5;   // below this the quotient is ill-conditioned

struct SatBand
{
    SatModel model = SatModel::WarmEven;
    float driveDb = 0.0f;      // 0..24
    float mixPercent = 100.0f; // 0..100
    bool operator== (const SatBand&) const = default;
};

struct SatSettings
{
    std::array<SatBand, 4> bands {};
    bool operator== (const SatSettings&) const = default;
};

struct SatChannel
{
    UpChain up {};
    DownChain downRef {}, downRes {};
    double prevV = 0.0;     // ADAA memory: previous shaper input (before k)
    double prevR = 0.0;     // residual integral at k * prevV, evaluated with prevK
    float prevK = -1.0f;    // -1 forces re-evaluation
    float eX1 = 0.0f, eY1 = 0.0f;   // pre-emphasis
    float dX1 = 0.0f, dY1 = 0.0f;   // de-emphasis
    float dcX1 = 0.0f, dcY1 = 0.0f; // DC blocker (base rate)
};

struct SatBandState
{
    std::array<SatChannel, 2> ch {};
    LinearRamp k {}, mix {}, fade {};
    SatModel model = SatModel::WarmEven;   // running
    bool resDirty = false;                 // residual chain holds signal
};

/** ~50 KB (FIR rings): allocate once, not on the audio thread's stack. */
struct SaturateLayer
{
    double sampleRate = 48000.0;
    OsConfig os {};
    SatSettings settings {};
    std::array<SatBandState, 4> bands {};
    float eB0 = 1.0f, eB1 = 0.0f, eA1 = 0.0f;   // E(z) = (b0 + b1 z^-1) / (1 + a1 z^-1)
    float dB0 = 1.0f, dB1 = 0.0f, dA1 = 0.0f;   // D = 1 / E
    float dcR = 0.0f;
};

/** Drive in dB -> shaper gain k. 0 dB -> exactly 0 (linear). */
inline float satDriveToK (float driveDb) noexcept
{
    return driveDb > 0.0f ? std::pow (10.0f, driveDb / 20.0f) - 1.0f : 0.0f;
}

namespace satdetail {

/** log cosh v, accurate for small |v| (no cancellation) and for huge |v| (no overflow). */
inline double logCosh (double v) noexcept
{
    const double a = std::abs (v);
    if (a <= 1.0)
    {
        const double s = std::sinh (0.5 * a);
        return std::log1p (2.0 * s * s);
    }
    return a + std::log1p (std::exp (-2.0 * a)) - std::numbers::ln2;
}

inline bool isAsymmetric (SatModel m) noexcept { return m == SatModel::WarmEven || m == SatModel::Triode; }

struct WarmEvenConsts
{
    double t, s, logCoshB;
};

// Namespace-scope constant: initialised at load time, so the audio thread never hits a
// function-local static's init guard.
inline const WarmEvenConsts kWarmEven { std::tanh (kWarmEvenBias), 1.0 - std::tanh (kWarmEvenBias) * std::tanh (kWarmEvenBias),
                                        logCosh (kWarmEvenBias) };

/** r(u) = f(u) - u. */
inline double residual (SatModel m, double u) noexcept
{
    switch (m)
    {
        case SatModel::WarmEven:
        {
            const auto& c = kWarmEven;
            return (std::tanh (u + kWarmEvenBias) - c.t) / c.s - u;
        }
        case SatModel::Triode:
            return cubicClipResidual (u, u >= 0.0 ? kTriodePosClip : kTriodeNegClip);
        case SatModel::Odd:
        case SatModel::Tape:
        default:
        {
            const double a = std::abs (u);
            const double r = std::tanh (a) - a;   // built from |u|: exactly odd
            return u >= 0.0 ? r : -r;
        }
    }
}

/** R(u) = integral of r from 0, R(0) = 0. */
inline double residualIntegral (SatModel m, double u) noexcept
{
    switch (m)
    {
        case SatModel::WarmEven:
        {
            const auto& c = kWarmEven;
            double lc;   // log cosh(u + b) - log cosh(b)
            if (std::abs (u) <= 1.0)
            {
                const double sh = std::sinh (0.5 * u);
                lc = std::log1p (c.t * std::sinh (u) + 2.0 * sh * sh);
            }
            else
                lc = logCosh (u + kWarmEvenBias) - c.logCoshB;
            return (lc - c.t * u) / c.s - 0.5 * u * u;
        }
        case SatModel::Triode:
            return cubicClipIntegral (u, u >= 0.0 ? kTriodePosClip : kTriodeNegClip);
        case SatModel::Odd:
        case SatModel::Tape:
        default:
            return logCosh (u) - 0.5 * u * u;
    }
}

/** First-order ADAA of r at k * v. Returns r / k (x-domain residual). k > 0. */
inline float adaa (SatChannel& c, SatModel m, float k, double v) noexcept
{
    const double kd = k;
    if (k != c.prevK)
    {
        c.prevR = residualIntegral (m, kd * c.prevV);   // k moved: both ends must use the same k
        c.prevK = k;
    }
    const double u0 = kd * c.prevV, u1 = kd * v;
    const double r1 = residualIntegral (m, u1);
    const double du = u1 - u0;
    const double r = std::abs (du) > kAdaaMinDelta ? (r1 - c.prevR) / du : residual (m, 0.5 * (u0 + u1));
    c.prevV = v;
    c.prevR = r1;
    return static_cast<float> (r / kd);
}

inline float finiteOr (float v, float keep) noexcept { return std::isfinite (v) ? v : keep; }

inline void resetShaper (SatChannel& c) noexcept
{
    c.prevR = 0.0;
    c.prevK = -1.0f;
    c.eX1 = c.eY1 = c.dX1 = c.dY1 = 0.0f;
    c.dcX1 = c.dcY1 = 0.0f;
}

/** Residual-side state only (the dry chain keeps running). */
inline void resetResidual (SatChannel& c) noexcept
{
    reset (c.downRes);
    c.dX1 = c.dY1 = 0.0f;
    c.dcX1 = c.dcY1 = 0.0f;
}

inline void bilinearShelf (double fs, double zeroHz, double poleHz, float& b0, float& b1, float& a1) noexcept
{
    const double K = 2.0 * fs;
    const double wz = 2.0 * std::numbers::pi * zeroHz, wp = 2.0 * std::numbers::pi * poleHz;
    const double a0 = 1.0 + K / wp;
    b0 = static_cast<float> ((1.0 + K / wz) / a0);
    b1 = static_cast<float> ((1.0 - K / wz) / a0);
    a1 = static_cast<float> ((1.0 - K / wp) / a0);
}

} // namespace satdetail

inline int latencySamples (const SaturateLayer& s) noexcept { return latencySamples (s.os); }

/** Audio thread, once per block. Clamps; non-finite fields keep their previous value. */
inline void setSettings (SaturateLayer& s, const SatSettings& in) noexcept
{
    using satdetail::finiteOr;
    for (std::size_t b = 0; b < 4; ++b)
    {
        auto& t = s.settings.bands[b];
        t.driveDb = std::clamp (finiteOr (in.bands[b].driveDb, t.driveDb), 0.0f, kSatMaxDriveDb);
        t.mixPercent = std::clamp (finiteOr (in.bands[b].mixPercent, t.mixPercent), 0.0f, 100.0f);
        t.model = in.bands[b].model;
        setTarget (s.bands[b].k, satDriveToK (t.driveDb));
        setTarget (s.bands[b].mix, t.mixPercent * 0.01f);
    }
}

/** Jump ramps and models to their targets: after prepare / state load only. */
inline void snapToTargets (SaturateLayer& s) noexcept
{
    for (std::size_t b = 0; b < 4; ++b)
    {
        auto& st = s.bands[b];
        const auto& t = s.settings.bands[b];
        st.k = makeLinearRamp (kSatRampMs, s.sampleRate, satDriveToK (t.driveDb));
        st.mix = makeLinearRamp (kSatRampMs, s.sampleRate, t.mixPercent * 0.01f);
        st.fade = makeLinearRamp (kSatModelFadeMs, s.sampleRate, 1.0f);
        if (st.model != t.model)
        {
            st.model = t.model;
            for (auto& c : st.ch)
                satdetail::resetShaper (c);
        }
    }
}

/** prepareToPlay (non-RT: designs the oversampler). Resets all state, keeps the settings.
 *  @param osFactor 1, 2 or 4 (anything else: 1). */
inline void prepare (SaturateLayer& s, double sampleRate, int osFactor, OsKind kind)
{
    s.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    s.os = makeOsConfig (osFactor, kind, s.sampleRate);
    const double fsOs = s.sampleRate * s.os.factor;
    satdetail::bilinearShelf (fsOs, kTapeZeroHz, kTapePoleHz, s.eB0, s.eB1, s.eA1);
    s.dB0 = 1.0f / s.eB0;          // D = 1/E: swap numerator and denominator, renormalise
    s.dB1 = s.eA1 / s.eB0;
    s.dA1 = s.eB1 / s.eB0;
    s.dcR = static_cast<float> (std::exp (-2.0 * std::numbers::pi * kSatDcHz / s.sampleRate));
    for (std::size_t b = 0; b < 4; ++b)
    {
        s.bands[b] = {};
        s.bands[b].model = s.settings.bands[b].model;
    }
    snapToTargets (s);
}

/** One base-rate sample of all four bands. `ref` receives the dry signal through the same
 *  oversampling chain: the depth blend must use it as dry (the chain has latency and, in IIR
 *  mode, phase shift). */
inline void process (SaturateLayer& s, StereoBands& io, StereoBands& ref) noexcept
{
    const int n = s.os.factor;
    for (std::size_t b = 0; b < 4; ++b)
    {
        auto& st = s.bands[b];
        const SatModel wanted = s.settings.bands[b].model;
        const float k = step (st.k);
        const float mix = step (st.mix);

        if (wanted != st.model)
        {
            if (k == 0.0f)          // silent residual: swap at once
            {
                st.model = wanted;
                for (auto& c : st.ch)
                    satdetail::resetShaper (c);
            }
            else
            {
                setTarget (st.fade, 0.0f);
                if (st.fade.current == 0.0f)
                {
                    st.model = wanted;
                    for (auto& c : st.ch)
                        satdetail::resetShaper (c);
                    setTarget (st.fade, 1.0f);
                }
            }
        }
        else
            setTarget (st.fade, 1.0f);
        const float fade = step (st.fade);
        const SatModel m = st.model;
        const bool tape = m == SatModel::Tape;
        const bool active = k > 0.0f;

        for (std::size_t c = 0; c < 2; ++c)
        {
            auto& ch = st.ch[c];
            float& x = c == 0 ? io.l[b] : io.r[b];
            float v[4], res[4];
            upsample (s.os, ch.up, x, v);
            const float dry = downsample (s.os, ch.downRef, v);
            (c == 0 ? ref.l[b] : ref.r[b]) = dry;

            for (int i = 0; i < n; ++i)
            {
                float e = v[i];
                if (tape)   // pre-emphasis always runs, so its state is warm when drive rises
                {
                    const float y = s.eB0 * e + s.eB1 * ch.eX1 - s.eA1 * ch.eY1;
                    ch.eX1 = e;
                    ch.eY1 = y;
                    e = y;
                }
                if (! active)
                {
                    ch.prevV = e;
                    ch.prevK = -1.0f;
                    continue;
                }
                float r = satdetail::adaa (ch, m, k, e);
                if (tape)
                {
                    const float y = s.dB0 * r + s.dB1 * ch.dX1 - s.dA1 * ch.dY1;
                    ch.dX1 = r;
                    ch.dY1 = y;
                    r = y;
                }
                res[i] = r;
            }

            if (! active)
            {
                // Residual at k -> 0 is ~k^2 x^3: cutting its tail is inaudible, and this keeps
                // a parked band bit-exact.
                if (st.resDirty)
                    satdetail::resetResidual (ch);
                x = dry;
                continue;
            }
            float r = downsample (s.os, ch.downRes, res);
            if (satdetail::isAsymmetric (m))
            {
                const float y = r - ch.dcX1 + s.dcR * ch.dcY1;
                ch.dcX1 = r;
                ch.dcY1 = y;
                r = y;
            }
            x = dry + (mix * fade) * r;
        }
        st.resDirty = active;
    }
}

template <typename Fn>
void forEachFloatState (SaturateLayer& s, Fn&& fn)
{
    for (auto& st : s.bands)
    {
        for (auto& c : st.ch)
        {
            forEachFloatState (c.up, fn);
            forEachFloatState (c.downRef, fn);
            forEachFloatState (c.downRes, fn);
            fn (c.eX1); fn (c.eY1); fn (c.dX1); fn (c.dY1); fn (c.dcX1); fn (c.dcY1);
            float pv = static_cast<float> (c.prevV), pr = static_cast<float> (c.prevR);
            fn (pv);
            fn (pr);
        }
        fn (st.k.current);
        fn (st.mix.current);
        fn (st.fade.current);
    }
}

/** Returns true if any state had to be reset (raise the UI flag; never log here). */
inline bool resetIfNotFinite (SaturateLayer& s) noexcept
{
    bool any = false;
    for (auto& st : s.bands)
        for (auto& c : st.ch)
        {
            bool bad = resetIfNotFinite (c.up);
            bad |= resetIfNotFinite (c.downRef);
            bad |= resetIfNotFinite (c.downRes);
            for (float v : { c.eX1, c.eY1, c.dX1, c.dY1, c.dcX1, c.dcY1 })
                bad |= ! std::isfinite (v);
            bad |= ! std::isfinite (c.prevV) || ! std::isfinite (c.prevR);
            if (bad)
            {
                reset (c.up);
                reset (c.downRef);
                reset (c.downRes);
                c.prevV = 0.0;
                satdetail::resetShaper (c);
            }
            any |= bad;
        }
    return any;
}

inline void flushDenormals (SaturateLayer& s) noexcept
{
    auto flush = [] (float& v) { if (std::abs (v) < kSnapEpsilon) v = 0.0f; };
    for (auto& st : s.bands)
        for (auto& c : st.ch)
        {
            flushDenormals (c.up);
            flushDenormals (c.downRef);
            flushDenormals (c.downRes);
            flush (c.eX1); flush (c.eY1); flush (c.dX1); flush (c.dY1); flush (c.dcX1); flush (c.dcY1);
            if (std::abs (c.prevV) < 1e-15) c.prevV = 0.0;
            if (std::abs (c.prevR) < 1e-30) c.prevR = 0.0;
        }
}

} // namespace strat3::dsp
