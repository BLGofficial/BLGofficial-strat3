#pragma once
// CEILING layer: always last, depth-exempt. True-peak safety limiter, then dither.
//
// Limiter (MIX/MASTER): tp = 4x true peak per channel (analysis/TruePeak.h, D = 8 samples of
// future), linked by max. need (dB) -> sliding minimum over L + 2S + 1 -> instant attack,
// fixed release -> box over L + 1 -> applied to audio delayed by D + L + S.
// The extra 2S in the minimum covers the interpolator support (S = D = 8): every output
// sample that feeds an inter-sample peak is already turned down when that peak is rebuilt.
// It is not an exact bound (the gain still varies across the 16-tap support and the taps have
// mixed signs), so the tests measure the result instead of assuming it.
// TRACK: no look-ahead and no interpolator: sample peak only, zero latency; true peak is
// unguarded (the delivery bar shows "TP UNGUARDED").
//
// Dither (16 bit only; 24 bit and Off pass the float through untouched):
//   y = Q16(v + d),  v = x + sum_k h_k e[n-k],  e = y - v,  d = TPDF +-1 LSB
//   Noise transfer function NTF(z) = 1 + sum h_k z^-k (9 taps). h is designed per sample rate:
//   target |NTF| (dB) = an inverted hearing-threshold curve (Terhardt), flat below 1 kHz and
//   clamped to [-5, +25] dB [INFERRED own design], made zero-mean in log so the minimum-phase
//   result (real cepstrum) is monic, then truncated to 9 taps.
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <numbers>
#include <vector>

#include "analysis/TruePeak.h"
#include "dsp/core/DelayLine.h"
#include "dsp/core/Smoother.h"
#include "dsp/layers/LowLayer.h"         // SlidingMax
#include "dsp/layers/MultibandLayer.h"   // mbdetail::follow, coeffFor

namespace strat3::dsp {

enum class DitherMode { Off, Tpdf, TpdfShaped };

inline constexpr float kCeilingLookaheadMs = 1.5f;
inline constexpr float kCeilingReleaseMs = 50.0f;
inline constexpr float kCeilingRampMs = 20.0f;
inline constexpr int kNoiseShaperTaps = 9;
inline constexpr float kDitherErrorClamp = 16.0f;   // LSBs: bounds the feedback after a clip at full scale

struct CeilingSettings
{
    float ceilingDb = -1.0f;   // -3..0 dBTP
    DitherMode dither = DitherMode::Off;
    int bits = 24;             // 16 or 24
    bool operator== (const CeilingSettings&) const = default;
};

struct DitherChannel
{
    std::array<float, kNoiseShaperTaps> e {};   // e[0] newest
    std::uint64_t rng = 0;
};

struct CeilingLayer
{
    double sampleRate = 48000.0;
    CeilingSettings settings {};
    bool lookahead = true;
    int boxLength = 1;
    int latency = 0;
    std::array<analysis::TruePeakDetector, 2> tp {};
    SlidingMax hold {};
    float fast = 0.0f;
    float releaseCoeff = 0.0f;
    std::vector<float> box {};
    double boxSum = 0.0;
    int boxPos = 0;
    float gainDb = 0.0f;
    std::array<DelayLine, 2> delay {};
    LinearRamp ceiling {};
    float lastCeiling = 1.0f, ceilingLin = 1.0f;
    std::array<float, kNoiseShaperTaps> shaper {};
    std::array<DitherChannel, 2> dither {};
};

/** Current limiter gain (dB, <= 0). Linked: the same for both channels. */
inline float ceilingGainDb (const CeilingLayer& c, std::size_t /*channel*/) noexcept { return c.gainDb; }

inline int latencySamples (const CeilingLayer& c) noexcept { return c.latency; }

namespace ceildetail {

inline float dbToGain (float db) noexcept { return std::exp (db * 0.11512925f); }
inline float gainToDb (float g) noexcept { return 20.0f * std::log10 (std::max (g, 1e-12f)); }

/** Terhardt's threshold in quiet, dB SPL. */
inline double hearingThresholdDb (double hz) noexcept
{
    const double f = std::max (hz, 20.0) / 1000.0;
    return 3.64 * std::pow (f, -0.8) - 6.5 * std::exp (-0.6 * (f - 3.3) * (f - 3.3)) + 1e-3 * f * f * f * f;
}

/** Uniform in [-0.5, 0.5): xorshift64*. */
inline float uniform (std::uint64_t& s) noexcept
{
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    const std::uint64_t r = s * 0x2545F4914F6CDD1Dull;
    return static_cast<float> (r >> 40) * (1.0f / 16777216.0f) - 0.5f;
}

} // namespace ceildetail

/** Non-RT (prepare). h_1..h_9 of NTF(z) = 1 + sum h_k z^-k. */
inline std::array<float, kNoiseShaperTaps> designNoiseShaper (double sampleRate)
{
    constexpr int n = 1024;
    const double fs = sampleRate > 0.0 ? sampleRate : 48000.0;
    std::vector<double> logMag (n);
    double mean = 0.0;
    for (int k = 0; k < n; ++k)
    {
        const int kk = k <= n / 2 ? k : n - k;
        const double hz = static_cast<double> (kk) * fs / n;
        const double target = std::clamp (ceildetail::hearingThresholdDb (std::max (hz, 1000.0)), -5.0, 25.0);
        logMag[static_cast<size_t> (k)] = target * std::numbers::ln10 / 20.0;
        mean += logMag[static_cast<size_t> (k)];
    }
    mean /= n;
    for (auto& v : logMag)
        v -= mean;   // zero mean log: the minimum-phase result starts with exactly 1

    // Real cepstrum (logMag is real and even, so the DFT is a cosine sum).
    std::vector<double> cep (n);
    for (int m = 0; m < n; ++m)
    {
        double s = 0.0;
        for (int k = 0; k < n; ++k)
            s += logMag[static_cast<size_t> (k)] * std::cos (2.0 * std::numbers::pi * k * m / n);
        cep[static_cast<size_t> (m)] = s / n;
    }
    // Fold to the causal (minimum-phase) cepstrum.
    std::vector<double> fold (n, 0.0);
    fold[0] = cep[0];
    for (int m = 1; m < n / 2; ++m)
        fold[static_cast<size_t> (m)] = 2.0 * cep[static_cast<size_t> (m)];
    fold[n / 2] = cep[n / 2];

    // H = exp(DFT(fold)); h = IDFT(H), first 10 samples.
    std::vector<std::complex<double>> spec (n);
    for (int k = 0; k < n; ++k)
    {
        std::complex<double> s { 0.0, 0.0 };
        for (int m = 0; m <= n / 2; ++m)
            s += fold[static_cast<size_t> (m)] * std::polar (1.0, -2.0 * std::numbers::pi * k * m / n);
        spec[static_cast<size_t> (k)] = std::exp (s);
    }
    std::array<float, kNoiseShaperTaps> h {};
    for (int t = 1; t <= kNoiseShaperTaps; ++t)
    {
        std::complex<double> s { 0.0, 0.0 };
        for (int k = 0; k < n; ++k)
            s += spec[static_cast<size_t> (k)] * std::polar (1.0, 2.0 * std::numbers::pi * k * t / n);
        h[static_cast<size_t> (t - 1)] = static_cast<float> (s.real() / n);
    }
    return h;
}

/** Audio thread, once per block. Non-finite ceiling keeps the previous value. */
inline void setSettings (CeilingLayer& c, const CeilingSettings& in) noexcept
{
    auto& t = c.settings;
    if (std::isfinite (in.ceilingDb))
        t.ceilingDb = std::clamp (in.ceilingDb, -3.0f, 0.0f);
    t.dither = in.dither;
    t.bits = in.bits == 16 ? 16 : 24;
    setTarget (c.ceiling, t.ceilingDb);
}

inline void snapToTargets (CeilingLayer& c) noexcept
{
    c.ceiling = makeLinearRamp (kCeilingRampMs, c.sampleRate, c.settings.ceilingDb);
}

namespace ceildetail {

inline void resetState (CeilingLayer& c) noexcept
{
    for (auto& d : c.tp)
        analysis::reset (d);
    reset (c.hold);
    c.fast = 0.0f;
    std::fill (c.box.begin(), c.box.end(), 0.0f);
    c.boxSum = 0.0;
    c.boxPos = 0;
    c.gainDb = 0.0f;
    for (auto& d : c.delay)
        reset (d);
    for (auto& d : c.dither)
        d.e = {};
}

} // namespace ceildetail

/** prepareToPlay (allocates, designs the noise shaper). Resets state, keeps settings.
 *  @param lookahead false in TRACK mode (sample peak only, zero latency). */
inline void prepare (CeilingLayer& c, double sampleRate, bool lookahead)
{
    c.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    c.lookahead = lookahead;
    const int s = analysis::kTruePeakLookahead;
    const int l = lookahead ? static_cast<int> (std::lround (static_cast<double> (kCeilingLookaheadMs) * 0.001 * c.sampleRate)) : 0;
    c.boxLength = l + 1;
    const int holdWindow = lookahead ? l + 2 * s + 1 : 1;
    c.latency = lookahead ? s + l + s : 0;   // detector delay + look-ahead + support
    prepare (c.hold, holdWindow);
    c.box.assign (static_cast<std::size_t> (c.boxLength), 0.0f);
    for (auto& d : c.delay)
    {
        prepare (d, static_cast<std::size_t> (c.latency));
        setDelay (d, static_cast<std::size_t> (c.latency));
    }
    c.releaseCoeff = mbdetail::coeffFor (kCeilingReleaseMs, c.sampleRate);
    c.shaper = designNoiseShaper (c.sampleRate);
    c.dither[0].rng = 0x9E3779B97F4A7C15ull;   // fixed, distinct seeds: renders are repeatable
    c.dither[1].rng = 0xD1B54A32D192ED03ull;
    ceildetail::resetState (c);
    c.lastCeiling = 1.0f;   // impossible dB value: force the first conversion
    snapToTargets (c);
}

inline void process (CeilingLayer& c, float& l, float& r) noexcept
{
    using ceildetail::gainToDb;

    const float ceilDb = step (c.ceiling);
    if (ceilDb != c.lastCeiling)
    {
        c.lastCeiling = ceilDb;
        c.ceilingLin = ceildetail::dbToGain (ceilDb);
    }

    const float peak = c.lookahead ? std::max (analysis::push (c.tp[0], l), analysis::push (c.tp[1], r))
                                   : std::max (std::abs (l), std::abs (r));
    const float need = peak > c.ceilingLin ? ceilDb - gainToDb (peak) : 0.0f;
    const float m = -push (c.hold, -need);
    c.fast = m < c.fast ? m : mbdetail::follow (c.fast, m, c.releaseCoeff);

    auto& slot = c.box[static_cast<std::size_t> (c.boxPos)];
    c.boxSum += static_cast<double> (c.fast) - static_cast<double> (slot);
    slot = c.fast;
    if (++c.boxPos == c.boxLength)
    {
        c.boxPos = 0;
        double s = 0.0;   // exact resum once per box length: the running sum cannot drift
        for (float v : c.box)
            s += static_cast<double> (v);
        c.boxSum = s;
    }
    c.gainDb = static_cast<float> (c.boxSum / static_cast<double> (c.boxLength));
    const float g = c.gainDb == 0.0f ? 1.0f : ceildetail::dbToGain (c.gainDb);

    std::array<float*, 2> io { &l, &r };
    for (std::size_t ch = 0; ch < 2; ++ch)
    {
        float y = process (c.delay[ch], *io[ch]);
        if (c.gainDb != 0.0f)
            y *= g;

        if (c.settings.bits == 16 && c.settings.dither != DitherMode::Off)
        {
            constexpr float q = 1.0f / 32768.0f;
            auto& d = c.dither[ch];
            float fb = 0.0f;
            if (c.settings.dither == DitherMode::TpdfShaped)
                for (int k = 0; k < kNoiseShaperTaps; ++k)
                    fb += c.shaper[static_cast<std::size_t> (k)] * d.e[static_cast<std::size_t> (k)];
            const float v = y + fb;
            const float tpdf = (ceildetail::uniform (d.rng) + ceildetail::uniform (d.rng)) * q;
            float out = std::floor ((v + tpdf) * 32768.0f + 0.5f) * q;
            out = std::clamp (out, -1.0f, 1.0f - q);
            for (int k = kNoiseShaperTaps - 1; k > 0; --k)
                d.e[static_cast<std::size_t> (k)] = d.e[static_cast<std::size_t> (k - 1)];
            d.e[0] = std::clamp (out - v, -kDitherErrorClamp * q, kDitherErrorClamp * q);
            y = out;
        }
        *io[ch] = y;
    }
}

template <typename Fn>
void forEachFloatState (CeilingLayer& c, Fn&& fn)
{
    fn (c.fast);
    fn (c.gainDb);
    for (auto& v : c.box) fn (v);
    for (auto& d : c.dither)
        for (auto& v : d.e) fn (v);
}

/** Returns true if any state had to be reset (raise the UI flag; never log here). */
inline bool resetIfNotFinite (CeilingLayer& c) noexcept
{
    bool bad = ! std::isfinite (c.boxSum);
    for (const auto& d : c.tp)
        bad |= ! analysis::isFinite (d);
    for (auto& d : c.delay)
        bad |= sanitise (d);   // O(latency) = ~90 samples: cheap enough to run per block here
    forEachFloatState (c, [&] (float v) { bad |= ! std::isfinite (v); });
    if (bad)
        ceildetail::resetState (c);
    return bad;
}

inline void flushDenormals (CeilingLayer& c) noexcept
{
    if (std::abs (c.fast) < kSnapEpsilon)
        c.fast = 0.0f;
}

} // namespace strat3::dsp
