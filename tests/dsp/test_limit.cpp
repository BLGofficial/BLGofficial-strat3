// LIMIT layer (broadband). Chain per sample:
//   x -> 3-band LR4 split (120 Hz, 4 kHz) -> [DENSE: per-band half-ratio pre-limit]
//     -> sum * makeup (-thresh) * transient emphasis -> 2x IIR oversampled cubic soft clip
//        (CLIP only, ADAA) -> look-ahead brickwall to the ceiling (sample peak).
//   Brickwall: needed gain (dB) -> sliding minimum over L+1 -> two-stage release
//   (fast: instant attack, release/10; slow: release both ways; take the lower)
//   -> box filter over L+1 -> applied to the input delayed by L.
//   L = 0 / 1.5 / 5 ms (TRACK / MIX / MASTER). The reference is the dry through the same
//   split, oversampler and delay: the depth blend's dry.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>
#include <limits>
#include <memory>
#include <numbers>
#include <vector>

#include "dsp/core/CubicClip.h"
#include "dsp/core/Oversampler.h"
#include "dsp/layers/LimitLayer.h"
#include "support/Analysis.h"

using namespace strat3::dsp;
using namespace strat3::test;

namespace {

constexpr double kFs = 48000.0;
constexpr double kTwoPi = 2.0 * std::numbers::pi;
constexpr std::array kStyles { LimitStyle::Clean, LimitStyle::Dense, LimitStyle::Clip };

std::unique_ptr<LimitLayer> makeLimit (const LimitSettings& s, float lookaheadMs = kLimitLookaheadMs[1], double fs = kFs)
{
    auto lim = std::make_unique<LimitLayer>();
    prepare (*lim, fs, lookaheadMs);
    setSettings (*lim, s);
    snapToTargets (*lim);
    return lim;
}

LimitSettings settings (LimitStyle style, float threshDb, float ceilingDb, float releaseMs = 100.0f, float link = 100.0f, float transient = 0.0f)
{
    LimitSettings s;
    s.style = style;
    s.threshDb = threshDb;
    s.ceilingDb = ceilingDb;
    s.releaseMs = releaseMs;
    s.linkPercent = link;
    s.transientPercent = transient;
    return s;
}

std::vector<LimitFrame> run (LimitLayer& lim, const std::vector<float>& l, const std::vector<float>& r)
{
    std::vector<LimitFrame> out;
    out.reserve (l.size());
    for (size_t i = 0; i < l.size(); ++i)
        out.push_back (process (lim, l[i], r[i]));
    return out;
}

std::vector<float> sine (double hz, double amp, size_t n, double fs = kFs)
{
    std::vector<float> x (n);
    for (size_t i = 0; i < n; ++i)
        x[i] = static_cast<float> (amp * std::sin (kTwoPi * hz * static_cast<double> (i) / fs));
    return x;
}

std::vector<float> noise (float amp, size_t n, std::uint64_t seed = 0)
{
    WhiteNoise w;
    w.counter = seed;
    std::vector<float> x (n);
    for (auto& v : x)
        v = amp * w.next();
    return x;
}

float dbToGain (float db) { return std::pow (10.0f, db / 20.0f); }

double toneAmp (const std::vector<float>& x, size_t from, size_t count, double hz, double fs = kFs)
{
    std::complex<double> sum { 0.0, 0.0 };
    const double w = kTwoPi * hz / fs;
    for (size_t n = 0; n < count; ++n)
        sum += static_cast<double> (x[from + n]) * std::polar (1.0, -w * static_cast<double> (n));
    return 2.0 * std::abs (sum) / static_cast<double> (count);
}

/** Non-harmonic power below 20 kHz relative to the fundamental (coherent window). */
double aliasDb (const std::vector<float>& y, double f0, size_t from, size_t count, double fs = kFs)
{
    const double bin = fs / static_cast<double> (count);
    double p = 0.0;
    for (double f = bin; f < 20000.0; f += bin)
    {
        const double h = f / f0;
        if (std::abs (h - std::round (h)) < 1e-6)
            continue;
        const double a = toneAmp (y, from, count, f, fs);
        p += a * a;
    }
    const double fund = toneAmp (y, from, count, f0, fs);
    return 10.0 * std::log10 (std::max (p, 1e-60)) - 20.0 * std::log10 (fund);
}

std::vector<float> leftOut (const std::vector<LimitFrame>& f)
{
    std::vector<float> v;
    for (const auto& x : f)
        v.push_back (x.l);
    return v;
}

} // namespace

// ------------------------------------------------------------------- neutral, latency

TEST_CASE ("LIMIT below the ceiling (CLEAN, DENSE) is bit-identical to the reference", "[dsp][limit][null]")
{
    const auto style = GENERATE (LimitStyle::Clean, LimitStyle::Dense);
    const float la = GENERATE (kLimitLookaheadMs[0], kLimitLookaheadMs[1], kLimitLookaheadMs[2]);
    auto lim = makeLimit (settings (style, 0.0f, 0.0f), la);
    const auto l = noise (0.3f, 48000, 1), r = noise (0.3f, 48000, 99);
    const auto out = run (*lim, l, r);
    for (const auto& f : out)
    {
        REQUIRE (f.l == f.refL);
        REQUIRE (f.r == f.refR);
    }
}

TEST_CASE ("LIMIT latency: look-ahead plus the clip oversampler, for every mode", "[dsp][limit][latency]")
{
    for (double fs : { 44100.0, 48000.0, 96000.0 })
    {
        const int os = latencySamples (makeOsConfig (2, OsKind::Iir, fs));
        CAPTURE (fs);
        CHECK (latencySamples (*makeLimit ({}, kLimitLookaheadMs[0], fs)) == os);
        CHECK (latencySamples (*makeLimit ({}, kLimitLookaheadMs[1], fs)) == os + static_cast<int> (std::lround (0.0015 * fs)));
        CHECK (latencySamples (*makeLimit ({}, kLimitLookaheadMs[2], fs)) == os + static_cast<int> (std::lround (0.005 * fs)));
    }
}

TEST_CASE ("LIMIT reference path has a flat magnitude", "[dsp][limit][response]")
{
    auto lim = makeLimit ({}, kLimitLookaheadMs[2]);
    std::vector<float> ir;
    for (int i = 0; i < 16384; ++i)
        ir.push_back (process (*lim, i == 0 ? 1.0f : 0.0f, 0.0f).refL);
    for (double hz = 20.0; hz <= 20000.0; hz *= 1.15)
    {
        CAPTURE (hz);
        CHECK (std::abs (magnitudeDb (ir, hz, kFs)) < 0.002);   // tight: a missing low-band all-pass costs 0.008 dB
    }
}

// ------------------------------------------------------------------- brickwall

TEST_CASE ("LIMIT output never exceeds the ceiling (sample peak), any style, mode or link", "[dsp][limit][brickwall]")
{
    const auto style = GENERATE (LimitStyle::Clean, LimitStyle::Dense, LimitStyle::Clip);
    const float la = GENERATE (kLimitLookaheadMs[0], kLimitLookaheadMs[1], kLimitLookaheadMs[2]);
    const float link = GENERATE (0.0f, 50.0f, 100.0f);
    CAPTURE (static_cast<int> (style), la, link);
    auto lim = makeLimit (settings (style, -6.0f, -1.0f, 50.0f, link), la);
    auto l = noise (2.0f, 48000, 3), r = noise (0.5f, 48000, 4);
    const auto bass = sine (50.0, 1.5, 48000);
    for (size_t i = 0; i < l.size(); ++i)
        l[i] += bass[i];
    const float ceiling = dbToGain (-1.0f) * (1.0f + 1e-5f);
    for (const auto& f : run (*lim, l, r))
    {
        REQUIRE (std::abs (f.l) <= ceiling);
        REQUIRE (std::abs (f.r) <= ceiling);
    }
}

TEST_CASE ("LIMIT look-ahead attack is a ramp, not a step", "[dsp][limit][brickwall]")
{
    const float la = GENERATE (kLimitLookaheadMs[1], kLimitLookaheadMs[2]);
    auto lim = makeLimit (settings (LimitStyle::Clean, 0.0f, -6.0206f, 1000.0f), la);
    const int L = static_cast<int> (std::lround (static_cast<double> (la) * 0.001 * kFs));
    auto x = sine (1000.0, 0.1, 24000);
    const auto loud = sine (1000.0, 1.0, 24000);
    x.insert (x.end(), loud.begin(), loud.end());
    float prev = 0.0f, worst = 0.0f, deepest = 0.0f;
    for (float v : x)
    {
        process (*lim, v, v);
        const float g = limitGainDb (*lim, 0);
        worst = std::max (worst, std::abs (g - prev));
        deepest = std::min (deepest, g);
        prev = g;
    }
    // The deepest value is past -6.02 dB: the split/oversampler all-passes ring on the step
    // and overshoot. Whatever the depth, the attack must spread it over L+1 samples.
    CAPTURE (L, worst, deepest);
    CHECK (deepest < -5.9f);
    CHECK (worst <= -deepest / static_cast<float> (L + 1) * 1.02f);
}

// ------------------------------------------------------------------- release

namespace {

/** Samples from the end of a loud burst until the gain is back above -1 dB. */
int recoverySamples (float releaseMs, int burstSamples)
{
    auto lim = makeLimit (settings (LimitStyle::Clean, 0.0f, -12.0f, releaseMs), kLimitLookaheadMs[1]);
    const auto loud = sine (1000.0, 1.0, static_cast<size_t> (burstSamples));
    for (float v : loud)
        process (*lim, v, v);
    for (int i = 0; i < 480000; ++i)
    {
        process (*lim, 0.0f, 0.0f);
        if (limitGainDb (*lim, 0) > -1.0f)
            return i;
    }
    return 480000;
}

} // namespace

TEST_CASE ("LIMIT release scales with the release time", "[dsp][limit][release]")
{
    const int fast = recoverySamples (10.0f, 24000), slow = recoverySamples (100.0f, 24000);
    CAPTURE (fast, slow);
    CHECK (slow > 5 * fast);
    // Sustained: the slow stage governs, from -12 dB to -1 dB in about release * ln 12.
    CHECK (std::abs (slow - 100.0 * 0.001 * kFs * std::log (12.0)) < 0.25 * 100.0 * 0.001 * kFs * std::log (12.0));
}

TEST_CASE ("LIMIT CLEAN two-stage release: a short peak recovers much faster than a sustained one", "[dsp][limit][release]")
{
    const int spike = recoverySamples (100.0f, 240);        // 5 ms burst
    const int sustained = recoverySamples (100.0f, 24000);  // 500 ms
    CAPTURE (spike, sustained);
    CHECK (spike < sustained / 3);
}


TEST_CASE ("LIMIT release changes take effect while running", "[dsp][limit][release]")
{
    auto lim = makeLimit (settings (LimitStyle::Clean, 0.0f, -12.0f, 100.0f), kLimitLookaheadMs[1]);
    setSettings (*lim, settings (LimitStyle::Clean, 0.0f, -12.0f, 10.0f));
    for (float v : sine (1000.0, 1.0, 24000))
        process (*lim, v, v);
    int i = 0;
    for (; i < 480000; ++i)
    {
        process (*lim, 0.0f, 0.0f);
        if (limitGainDb (*lim, 0) > -1.0f)
            break;
    }
    CHECK (i < recoverySamples (100.0f, 24000) / 5);
}

TEST_CASE ("LIMIT DENSE band gains follow the stereo link", "[dsp][limit][dense][link]")
{
    const auto l = sine (60.0, 1.0, 48000), r = sine (60.0, 0.1, 48000);
    {
        auto lim = makeLimit (settings (LimitStyle::Dense, 0.0f, -6.0f, 100.0f, 100.0f));
        run (*lim, l, r);
        CHECK (denseGainDb (*lim, 0, 1) == denseGainDb (*lim, 0, 0));
        CHECK (denseGainDb (*lim, 0, 0) < -1.5f);
    }
    {
        auto lim = makeLimit (settings (LimitStyle::Dense, 0.0f, -6.0f, 100.0f, 0.0f));
        run (*lim, l, r);
        CHECK (denseGainDb (*lim, 0, 1) == 0.0f);
        CHECK (denseGainDb (*lim, 0, 0) < -1.5f);
    }
}

TEST_CASE ("LIMIT gain returns to exactly 0 dB after limiting stops (bit-exact again)", "[dsp][limit][release]")
{
    auto lim = makeLimit (settings (LimitStyle::Clean, 0.0f, -6.0f, 100.0f), kLimitLookaheadMs[1]);
    run (*lim, noise (2.0f, 48000, 5), noise (2.0f, 48000, 6));
    const auto quiet = noise (0.1f, 6 * 48000, 7);
    const auto out = run (*lim, quiet, quiet);
    CHECK (limitGainDb (*lim, 0) == 0.0f);
    CHECK (out.back().l == out.back().refL);
}

// ------------------------------------------------------------------- link, makeup, ceiling

TEST_CASE ("LIMIT link 100% applies one gain to both channels; 0% leaves a quiet channel alone", "[dsp][limit][link]")
{
    const auto l = sine (1000.0, 1.0, 24000), r = sine (1000.0, 0.1, 24000);
    {
        auto lim = makeLimit (settings (LimitStyle::Clean, 0.0f, -6.0f, 100.0f, 100.0f));
        for (size_t i = 0; i < l.size(); ++i)
        {
            process (*lim, l[i], r[i]);
            REQUIRE (limitGainDb (*lim, 0) == limitGainDb (*lim, 1));
        }
        CHECK (limitGainDb (*lim, 1) < -5.0f);
    }
    {
        auto lim = makeLimit (settings (LimitStyle::Clean, 0.0f, -6.0f, 100.0f, 0.0f));
        for (size_t i = 0; i < l.size(); ++i)
        {
            process (*lim, l[i], r[i]);
            REQUIRE (limitGainDb (*lim, 1) == 0.0f);
        }
        CHECK (limitGainDb (*lim, 0) < -5.0f);
    }
}

TEST_CASE ("LIMIT threshold is makeup gain: quiet material comes out -thresh louder", "[dsp][limit][gain]")
{
    auto lim = makeLimit (settings (LimitStyle::Clean, -6.0f, 0.0f));
    const auto x = sine (500.0, 0.05, 24000);
    const float g = dbToGain (6.0f);
    const auto out = run (*lim, x, x);
    for (size_t i = 4800; i < out.size(); ++i)
        REQUIRE (std::abs (out[i].l - g * out[i].refL) <= 1e-6f);
    CHECK (limitGainDb (*lim, 0) == 0.0f);
}

TEST_CASE ("LIMIT ceiling sets the output peak", "[dsp][limit][gain]")
{
    auto lim = makeLimit (settings (LimitStyle::Clean, -12.0f, -3.0f));
    const auto out = run (*lim, noise (0.5f, 48000, 7), noise (0.5f, 48000, 8));
    float peak = 0.0f;
    for (size_t i = 24000; i < out.size(); ++i)
        peak = std::max (peak, std::abs (out[i].l));
    CHECK (peak <= dbToGain (-3.0f) * (1.0f + 1e-5f));
    CHECK (peak > dbToGain (-3.5f));   // and it actually gets there
}

// ------------------------------------------------------------------- transient emphasis

TEST_CASE ("LIMIT transient emphasis lifts onsets by up to 6 dB, leaves sustain alone", "[dsp][limit][transient]")
{
    auto lim = makeLimit (settings (LimitStyle::Clean, 0.0f, 0.0f, 100.0f, 100.0f, 100.0f));
    std::vector<float> x (4800, 0.0f);
    const auto tone = sine (200.0, 0.1, 48000);
    x.insert (x.end(), tone.begin(), tone.end());
    const auto out = run (*lim, x, x);
    auto peakRatioDb = [&] (size_t from, size_t to) {
        float o = 0.0f, r = 0.0f;
        for (size_t i = from; i < to; ++i)
        {
            o = std::max (o, std::abs (out[i].l));
            r = std::max (r, std::abs (out[i].refL));
        }
        return 20.0f * std::log10 (o / r);
    };
    const float onset = peakRatioDb (4800, 4800 + 480);           // first 10 ms
    const float sustain = peakRatioDb (4800 + 38400, 4800 + 48000); // 0.8 .. 1 s
    CAPTURE (onset, sustain);
    CHECK (onset > 3.0f);
    CHECK (onset <= 6.05f);
    CHECK (std::abs (sustain) < 0.2f);
}

// ------------------------------------------------------------------- styles

namespace {

float meanMainGrDb (LimitStyle style, const std::vector<float>& x, float ceilingDb)
{
    auto lim = makeLimit (settings (style, 0.0f, ceilingDb, 100.0f));
    double sum = 0.0;
    int n = 0;
    for (size_t i = 0; i < x.size(); ++i)
    {
        process (*lim, x[i], x[i]);
        if (i >= x.size() / 2)
        {
            sum += static_cast<double> (limitGainDb (*lim, 0));
            ++n;
        }
    }
    return static_cast<float> (sum / n);
}

} // namespace

TEST_CASE ("LIMIT DENSE: the band pre-limiter takes about half of the reduction", "[dsp][limit][dense]")
{
    const auto bass = sine (60.0, 1.0, 96000);
    const float clean = meanMainGrDb (LimitStyle::Clean, bass, -6.0206f);
    const float dense = meanMainGrDb (LimitStyle::Dense, bass, -6.0206f);
    CAPTURE (clean, dense);
    CHECK (clean < -5.5f);
    CHECK (dense / clean > 0.35f);
    CHECK (dense / clean < 0.65f);

    auto lim = makeLimit (settings (LimitStyle::Dense, 0.0f, -6.0206f));
    for (float v : bass)
        process (*lim, v, v);
    CHECK (std::abs (denseGainDb (*lim, 0, 0) + 3.0f) < 0.75f);   // low band: half of 6 dB
    CHECK (denseGainDb (*lim, 2, 0) == 0.0f);                      // high band untouched
}

TEST_CASE ("LIMIT CLIP: the clipper takes most of the peaks off before the limiter", "[dsp][limit][clip]")
{
    // Program-like crest: white noise through a 2-pole 3 kHz low-pass. (Full-band white noise
    // is the clipper's worst case: the 2x down-sampler rebuilds overshoot from the clipped HF.)
    auto x = noise (1.0f, 96000, 11);
    {
        const float a = std::exp (-2.0f * std::numbers::pi_v<float> * 3000.0f / static_cast<float> (kFs));
        float s1 = 0.0f, s2 = 0.0f;
        for (auto& v : x)
        {
            s1 = (1.0f - a) * v + a * s1;
            s2 = (1.0f - a) * s1 + a * s2;
            v = 2.0f * s2;
        }
    }
    const float clean = meanMainGrDb (LimitStyle::Clean, x, -6.0f);
    const float clip = meanMainGrDb (LimitStyle::Clip, x, -6.0f);
    CAPTURE (clean, clip);
    CHECK (clean < -2.0f);
    CHECK (clip > 0.5f * clean);
}

TEST_CASE ("LIMIT CLIP aliases far less than a naive 1x cubic clip", "[dsp][limit][clip][alias]")
{
    const double f0 = 5000.0;
    const size_t settle = 9600, count = 4800;
    const auto x = sine (f0, 1.0, settle + count);
    const float t = 0.5f;
    auto lim = makeLimit (settings (LimitStyle::Clip, 0.0f, 20.0f * std::log10 (t)), kLimitLookaheadMs[1]);
    const double ours = aliasDb (leftOut (run (*lim, x, x)), f0, settle, count);

    std::vector<float> naive (x.size());
    for (size_t i = 0; i < x.size(); ++i)
        naive[i] = t * static_cast<float> (static_cast<double> (x[i] / t) + cubicClipResidual (static_cast<double> (x[i] / t), 1.0));
    const double ref = aliasDb (naive, f0, settle, count);
    // Same 2x oversampler without ADAA: isolates what ADAA itself buys.
    const OsConfig os = makeOsConfig (2, OsKind::Iir, kFs);
    UpChain up;
    DownChain down;
    reset (up);
    reset (down);
    std::vector<float> naive2x (x.size());
    for (size_t i = 0; i < x.size(); ++i)
    {
        float v[4] {};
        upsample (os, up, x[i], v);
        for (int p = 0; p < 2; ++p)
            v[p] = t * static_cast<float> (static_cast<double> (v[p] / t) + cubicClipResidual (static_cast<double> (v[p] / t), 1.0));
        naive2x[i] = downsample (os, down, v);
    }
    const double ref2x = aliasDb (naive2x, f0, settle, count);
    CAPTURE (ours, ref, ref2x);
    CHECK (ours < ref - 15.0);
    CHECK (ours < ref2x - 6.0);
}

TEST_CASE ("LIMIT style switch does not click", "[dsp][limit][smoothing]")
{
    auto lim = makeLimit (settings (LimitStyle::Clean, 0.0f, -6.0f));
    const auto x = sine (1000.0, 0.8, 48000 + 9600);
    std::vector<float> y;
    for (size_t i = 0; i < x.size(); ++i)
    {
        if (i == 48000)
            setSettings (*lim, settings (LimitStyle::Clip, 0.0f, -6.0f));
        y.push_back (process (*lim, x[i], x[i]).l);
    }
    auto maxD2 = [&] (size_t from, size_t to) {
        float m = 0.0f;
        for (size_t i = from; i < to; ++i)
            m = std::max (m, std::abs (y[i] - 2.0f * y[i - 1] + y[i - 2]));
        return m;
    };
    const float before = maxD2 (38400, 48000), after = maxD2 (52800, 57600), during = maxD2 (48000, 52800);
    CAPTURE (before, after, during);
    CHECK (during <= 1.5f * std::max (before, after));
}

// ------------------------------------------------------------------- shared cubic clip

TEST_CASE ("cubic clip: integral differentiates to the residual, unity slope, ceiling c", "[dsp][limit][clip][shaper]")
{
    for (double c : { 0.6, 1.0 })
        for (double u = -5.0; u <= 5.0; u += 0.0131)
        {
            const double h = 1e-6;
            const double d = (cubicClipIntegral (u + h, c) - cubicClipIntegral (u - h, c)) / (2.0 * h);
            REQUIRE (std::abs (d - cubicClipResidual (u, c)) < 1e-6);
            REQUIRE (std::abs (u + cubicClipResidual (u, c)) <= c + 1e-12);
        }
    CHECK (cubicClipResidual (0.0, 1.0) == 0.0);
    CHECK (std::abs (cubicClipResidual (1e-4, 1.0)) < 1e-12);
}

// ------------------------------------------------------------------- settings, stability

TEST_CASE ("LIMIT settings clamp, and non-finite values keep the previous value", "[dsp][limit][params]")
{
    auto lim = makeLimit (settings (LimitStyle::Dense, -3.0f, -1.0f, 50.0f, 40.0f, 20.0f));
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    setSettings (*lim, settings (LimitStyle::Dense, nan, inf, nan, -inf, nan));
    const auto& s = lim->settings;
    CHECK (s.threshDb == -3.0f);
    CHECK (s.ceilingDb == -1.0f);
    CHECK (s.releaseMs == 50.0f);
    CHECK (s.linkPercent == 40.0f);
    CHECK (s.transientPercent == 20.0f);

    setSettings (*lim, settings (LimitStyle::Clean, -40.0f, 3.0f, 5000.0f, 140.0f, 140.0f));
    CHECK (s.threshDb == -24.0f);
    CHECK (s.ceilingDb == 0.0f);
    CHECK (s.releaseMs == 1000.0f);
    CHECK (s.linkPercent == 100.0f);
    CHECK (s.transientPercent == 100.0f);
    setSettings (*lim, settings (LimitStyle::Clean, 5.0f, -40.0f, 0.0f, -5.0f, -5.0f));
    CHECK (s.threshDb == 0.0f);
    CHECK (s.ceilingDb == -24.0f);
    CHECK (s.releaseMs == 1.0f);
    CHECK (s.linkPercent == 0.0f);
    CHECK (s.transientPercent == 0.0f);
}

TEST_CASE ("LIMIT stays finite and under the ceiling at extreme settings", "[dsp][limit][stability]")
{
    for (auto style : kStyles)
        for (float la : kLimitLookaheadMs)
        {
            CAPTURE (static_cast<int> (style), la);
            auto lim = makeLimit (settings (style, -24.0f, -1.0f, 1.0f, 0.0f, 100.0f), la);
            const auto out = run (*lim, noise (4.0f, 48000, 21), noise (4.0f, 48000, 22));
            for (const auto& f : out)
            {
                REQUIRE (std::isfinite (f.l));
                REQUIRE (std::isfinite (f.refR));
                REQUIRE (std::abs (f.l) <= dbToGain (-1.0f) * (1.0f + 1e-5f));
                REQUIRE (std::abs (f.r) <= dbToGain (-1.0f) * (1.0f + 1e-5f));
            }
        }
}

TEST_CASE ("LIMIT recovers from a NaN input", "[dsp][limit][stability]")
{
    auto lim = makeLimit (settings (LimitStyle::Dense, -6.0f, -1.0f, 100.0f, 100.0f, 50.0f), kLimitLookaheadMs[2]);
    process (*lim, std::numeric_limits<float>::quiet_NaN(), 0.0f);
    CHECK (resetIfNotFinite (*lim));
    CHECK_FALSE (resetIfNotFinite (*lim));
    const auto x = sine (1000.0, 0.9, 9600);
    for (const auto& f : run (*lim, x, x))
    {
        REQUIRE (std::isfinite (f.l));
        REQUIRE (std::isfinite (f.refL));
    }
}

TEST_CASE ("LIMIT state never goes subnormal in silence when flushed per block", "[dsp][limit][stability]")
{
    const auto style = GENERATE (LimitStyle::Clean, LimitStyle::Dense, LimitStyle::Clip);
    auto lim = makeLimit (settings (style, -12.0f, -1.0f, 100.0f, 50.0f, 100.0f), kLimitLookaheadMs[2]);
    WhiteNoise n;
    for (int i = 0; i < 480000; ++i)   // 0.5 s noise, then 9.5 s silence
    {
        const float v = i < 24000 ? 0.5f * n.next() : 0.0f;
        process (*lim, v, -v);
        if (i % 64 == 63)
            flushDenormals (*lim);
    }
    bool sub = false;
    forEachFloatState (*lim, [&] (float v) { sub |= isSubnormal (v); });
    CHECK_FALSE (sub);
}
