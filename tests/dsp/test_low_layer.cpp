// LOW layer: LR4 split at low.xover, punch/sustain shaper (fast vs slow envelope), tight
// expander and mono-below on the low band. The layer returns its own phase-matched
// reference (the same all-passes, no processing) for the depth blend.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <numbers>
#include <vector>

#include "dsp/layers/LowLayer.h"
#include "support/Analysis.h"

using namespace strat3::dsp;
using namespace strat3::test;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double kFs = 48000.0;

LowSettings neutral() { return {}; }

LowSettings with (float punch, float sustain, float tight)
{
    LowSettings s;
    s.punch = punch;
    s.sustain = sustain;
    s.tight = tight;
    return s;
}

int ms (double milliseconds, double fs = kFs) { return static_cast<int> (milliseconds * 0.001 * fs); }

struct Run
{
    std::vector<float> l, r, refL, refR, gainDb;
};

/** Stereo-identical input from `amplitude(n)` * sin at `hz`. */
template <typename Amp>
Run runSine (LowLayer& low, double hz, int samples, Amp&& amplitude, double fs = kFs)
{
    Run out;
    const double w = 2.0 * std::numbers::pi * hz / fs;
    for (int n = 0; n < samples; ++n)
    {
        const float x = static_cast<float> (amplitude (n) * std::sin (w * n));
        const auto f = process (low, x, x);
        out.l.push_back (f.l);
        out.r.push_back (f.r);
        out.refL.push_back (f.refL);
        out.refR.push_back (f.refR);
        out.gainDb.push_back (lowGainDb (low));
    }
    return out;
}

LowLayer makeLow (const LowSettings& s, double fs = kFs)
{
    LowLayer low;
    prepare (low, fs);
    setSettings (low, s);
    snapToTargets (low);
    return low;
}

float maxIn (const std::vector<float>& v, int from, int to)
{
    float m = -1e9f;
    for (int i = from; i < to; ++i)
        m = std::max (m, v[static_cast<size_t> (i)]);
    return m;
}

float minIn (const std::vector<float>& v, int from, int to)
{
    float m = 1e9f;
    for (int i = from; i < to; ++i)
        m = std::min (m, v[static_cast<size_t> (i)]);
    return m;
}

} // namespace

// ------------------------------------------------------------------- null / reference

TEST_CASE ("LOW neutral: output is bit-identical to the reference path", "[dsp][low][null]")
{
    auto low = makeLow (neutral());
    Noise noise;
    for (int i = 0; i < 48000; ++i)
    {
        const auto f = process (low, noise.next(), noise.next());
        REQUIRE (f.l == f.refL);
        REQUIRE (f.r == f.refR);
    }
}

TEST_CASE ("LOW reference path has a flat magnitude (all-pass only)", "[dsp][low][response]")
{
    for (const bool mono : { false, true })
    {
        LowSettings s;
        s.monoOn = mono;     // the reference never changes with mono: it only carries the phase
        s.monoHz = 150.0f;
        auto low = makeLow (s);
        std::vector<float> irL (65536), irR (65536);
        for (size_t n = 0; n < irL.size(); ++n)
        {
            const float x = n == 0 ? 1.0f : 0.0f;
            const auto f = process (low, x, -0.5f * x);
            irL[n] = f.refL;
            irR[n] = f.refR;
        }
        for (const double hz : { 25.0, 60.0, 100.0, 150.0, 1000.0, 10000.0 })
        {
            INFO ("mono = " << mono << " f = " << hz);
            CHECK (std::abs (magnitudeDb (irL, hz, kFs)) < 0.01);
            CHECK (std::abs (magnitudeDb (irR, hz, kFs) - 20.0 * std::log10 (0.5)) < 0.01);
        }
    }
}

TEST_CASE ("LOW latency is zero", "[dsp][low]")
{
    auto low = makeLow (with (100.0f, 100.0f, 100.0f));
    CHECK (latencySamples (low) == 0);
}

// ------------------------------------------------------------------- shaper

TEST_CASE ("steady low sine gets 0 dB from punch, sustain and tight", "[dsp][low][shaper]")
{
    // Ripple-free detector: a 1 ms envelope on a 40 Hz waveform would otherwise modulate
    // the gain every cycle (distortion) and read as a constant transient.
    for (const double hz : { 40.0, 60.0, 90.0 })
        for (const float sign : { 1.0f, -1.0f })
        {
            auto low = makeLow (with (100.0f * sign, 100.0f * sign, 100.0f));
            const auto run = runSine (low, hz, 48000, [] (int) { return 0.5; });
            INFO ("f = " << hz << " sign = " << sign);
            CHECK (maxIn (run.gainDb, 24000, 48000) < 0.05f);
            CHECK (minIn (run.gainDb, 24000, 48000) > -0.05f);
        }
}

TEST_CASE ("punch boosts (or cuts) the onset of a low burst, then returns to 0 dB", "[dsp][low][shaper]")
{
    const int onset = ms (200);
    auto burst = [&] (int n) { return n < onset ? 0.0 : 0.5; };

    auto up = makeLow (with (100.0f, 0.0f, 0.0f));
    const auto a = runSine (up, 60.0, onset + ms (600), burst);
    CHECK (maxIn (a.gainDb, onset, onset + ms (20)) > 6.0f);
    CHECK (std::abs (a.gainDb[static_cast<size_t> (onset + ms (550))]) < 0.1f);

    auto down = makeLow (with (-100.0f, 0.0f, 0.0f));
    const auto b = runSine (down, 60.0, onset + ms (600), burst);
    CHECK (minIn (b.gainDb, onset, onset + ms (20)) < -6.0f);
    CHECK (std::abs (b.gainDb[static_cast<size_t> (onset + ms (550))]) < 0.1f);
}

TEST_CASE ("sustain boosts (or cuts) the decay; punch ignores it", "[dsp][low][shaper]")
{
    const int drop = ms (500);
    auto stepDown = [&] (int n) { return n < drop ? 0.5 : 0.05; };   // -20 dB step
    const int from = drop + ms (40), to = drop + ms (120);

    auto up = makeLow (with (0.0f, 100.0f, 0.0f));
    CHECK (minIn (runSine (up, 60.0, drop + ms (200), stepDown).gainDb, from, to) > 3.0f);

    auto down = makeLow (with (0.0f, -100.0f, 0.0f));
    CHECK (maxIn (runSine (down, 60.0, drop + ms (200), stepDown).gainDb, from, to) < -3.0f);

    auto punchOnly = makeLow (with (100.0f, 0.0f, 0.0f));
    const auto p = runSine (punchOnly, 60.0, drop + ms (200), stepDown);
    CHECK (maxIn (p.gainDb, drop, drop + ms (200)) < 0.01f);
    CHECK (minIn (p.gainDb, drop, drop + ms (200)) > -0.01f);
}

TEST_CASE ("shaper gain is clamped to +-12 dB", "[dsp][low][shaper]")
{
    const int onset = ms (100), drop = ms (700);
    auto hitThenSilence = [&] (int n) { return n < onset ? 0.0 : n < drop ? 1.0 : 1e-4; };
    for (const float sign : { 1.0f, -1.0f })
    {
        auto low = makeLow (with (100.0f * sign, 100.0f * sign, 0.0f));
        const auto run = runSine (low, 60.0, drop + ms (500), hitThenSilence);
        const int size = static_cast<int> (run.gainDb.size());
        const float hi = maxIn (run.gainDb, 0, size), lo = minIn (run.gainDb, 0, size);
        INFO ("sign = " << sign << " max = " << hi << " min = " << lo);
        CHECK (hi <= kLowShapeMaxDb + 1e-4f);
        CHECK (lo >= -kLowShapeMaxDb - 1e-4f);
        // ...and actually reaches the clamp, otherwise the bound proves nothing.
        CHECK ((sign > 0.0f ? hi : -lo) > kLowShapeMaxDb - 0.1f);
    }
}

// ------------------------------------------------------------------- tight

TEST_CASE ("tight pushes down the low band after it falls well below its recent peak", "[dsp][low][tight]")
{
    const int drop = ms (500);
    auto stepDown = [&] (int n) { return n < drop ? 0.5 : 0.0158; };   // -30 dB step

    auto tight = makeLow (with (0.0f, 0.0f, 100.0f));
    const auto t = runSine (tight, 60.0, drop + ms (3000), stepDown);
    CHECK (t.gainDb[static_cast<size_t> (drop + ms (100))] < -12.0f);
    CHECK (minIn (t.gainDb, 0, static_cast<int> (t.gainDb.size())) >= -kLowTightMaxDb - 1e-4f);
    CHECK (t.gainDb[static_cast<size_t> (drop + ms (2900))] > -0.5f);   // recovers: not a gate

    auto off = makeLow (neutral());
    const auto o = runSine (off, 60.0, drop + ms (500), stepDown);
    CHECK (minIn (o.gainDb, 0, static_cast<int> (o.gainDb.size())) == 0.0f);
}

TEST_CASE ("tight ignores level drops smaller than its knee", "[dsp][low][tight]")
{
    // Phrasing and small level changes must pass; only tails well below the hit expand.
    const int drop = ms (500);
    auto smallDrop = [&] (int n) { return n < drop ? 0.5 : 0.5 * std::pow (10.0, -4.0 / 20.0); };   // -4 dB
    auto low = makeLow (with (0.0f, 0.0f, 100.0f));
    const auto run = runSine (low, 60.0, drop + ms (1000), smallDrop);
    CHECK (minIn (run.gainDb, drop, drop + ms (1000)) > -0.01f);
}

// ------------------------------------------------------------------- band isolation

TEST_CASE ("processing stays below the crossover: highs match the reference", "[dsp][low][isolation]")
{
    // 60 Hz bursts drive the shaper hard while a steady 3 kHz tone rides on top.
    auto low = makeLow (with (100.0f, -100.0f, 100.0f));
    const double wLow = 2.0 * std::numbers::pi * 60.0 / kFs;
    const double wHigh = 2.0 * std::numbers::pi * 3000.0 / kFs;
    std::vector<float> diff;
    for (int n = 0; n < 96000; ++n)
    {
        const double gate = (n / ms (250)) % 2 == 0 ? 0.5 : 0.0;
        const float x = static_cast<float> (gate * std::sin (wLow * n) + 0.25 * std::sin (wHigh * n));
        const auto f = process (low, x, x);
        if (n >= 48000)
            diff.push_back (f.l - f.refL);
    }
    // Level of the difference at 3 kHz, relative to the 0.25 tone: DFT over whole cycles.
    const double a = std::abs (dtft (diff, 3000.0, kFs)) * 2.0 / static_cast<double> (diff.size());
    INFO ("3 kHz component of (out - ref) = " << 20.0 * std::log10 (a / 0.25 + 1e-30) << " dB re tone");
    CHECK (a / 0.25 < 1e-3);   // below -60 dB
}

// ------------------------------------------------------------------- mono below

TEST_CASE ("mono below removes the side channel under the set frequency", "[dsp][low][mono]")
{
    LowSettings s;
    s.monoOn = true;
    s.monoHz = 160.0f;

    auto sideAt = [&] (double hz)
    {
        auto low = makeLow (s);
        const double w = 2.0 * std::numbers::pi * hz / kFs;
        float peak = 0.0f;
        for (int n = 0; n < 48000; ++n)
        {
            const float x = static_cast<float> (0.5 * std::sin (w * n));
            const auto f = process (low, x, -x);   // pure side
            if (n >= 24000)
                peak = std::max (peak, std::abs (f.l));
        }
        return 20.0 * std::log10 (static_cast<double> (peak) / 0.5);
    };
    CHECK (sideAt (40.0) < -40.0);                        // LR4: (40/160)^4 -> -48 dB
    CHECK_THAT (sideAt (1000.0), WithinAbs (0.0, 0.05));  // side above passes

    // Mid is untouched at any frequency.
    auto low = makeLow (s);
    const double w = 2.0 * std::numbers::pi * 40.0 / kFs;
    float maxErr = 0.0f;
    for (int n = 0; n < 48000; ++n)
    {
        const float x = static_cast<float> (0.5 * std::sin (w * n));
        const auto f = process (low, x, x);
        if (n >= 24000)
            maxErr = std::max (maxErr, std::abs (f.l - f.refL));
    }
    CHECK (maxErr < 1e-5f);
}

TEST_CASE ("mono below keeps mid and side phase-aligned above the cutoff", "[dsp][low][mono]")
{
    // L-only 1 kHz tone = equal mid and side. If side went through the LR4 high-pass while
    // mid stayed dry, their phases would differ and the image would smear near the cutoff.
    LowSettings s;
    s.monoOn = true;
    s.monoHz = 160.0f;
    auto low = makeLow (s);
    const double w = 2.0 * std::numbers::pi * 1000.0 / kFs;
    float maxErr = 0.0f;
    for (int n = 0; n < 48000; ++n)
    {
        const float x = static_cast<float> (0.5 * std::sin (w * n));
        const auto f = process (low, x, 0.0f);
        if (n >= 24000)
            maxErr = std::max ({ maxErr, std::abs (f.l - f.refL), std::abs (f.r - f.refR) });
    }
    CHECK (maxErr < 0.5f * 3e-3f);   // HP magnitude at 1 kHz is -0.006 dB: residue below -50 dB
}

TEST_CASE ("toggling mono below is click-free", "[dsp][low][mono]")
{
    auto low = makeLow (neutral());
    const double w = 2.0 * std::numbers::pi * 50.0 / kFs;
    float previous = 0.0f;
    double maxStep = 0.0;
    for (int n = 0; n < 48000; ++n)
    {
        if (n == 12000 || n == 30000)
        {
            LowSettings s;
            s.monoOn = n == 12000;
            setSettings (low, s);
        }
        const float x = static_cast<float> (0.5 * std::sin (w * n));
        const auto f = process (low, x, 0.0f);
        if (n > 1000)
            maxStep = std::max (maxStep, static_cast<double> (std::abs (f.l - previous)));
        previous = f.l;
    }
    // An instant switch would jump by up to the full low side (0.25).
    INFO ("max step = " << maxStep << " sine bound = " << 0.5 * w);
    CHECK (maxStep < 0.5 * w * 1.1);
}

// ------------------------------------------------------------------- robustness

TEST_CASE ("LOW is stable under random automation at the extremes", "[dsp][low][stability]")
{
    auto low = makeLow (neutral());
    Noise noise, automation;
    bool finite = true;
    float peak = 0.0f;
    for (int n = 0; n < 48000 * 10; ++n)
    {
        if (n % 64 == 0)
        {
            auto u = [&] { return 0.5f * (automation.next() + 1.0f); };   // [0, 1]
            LowSettings s;
            s.xoverHz = 40.0f + 160.0f * u();
            s.punch = 200.0f * u() - 100.0f;
            s.sustain = 200.0f * u() - 100.0f;
            s.tight = 100.0f * u();
            s.monoOn = u() > 0.5f;
            s.monoHz = 40.0f + 160.0f * u();
            setSettings (low, s);
        }
        const auto f = process (low, noise.next(), noise.next());
        finite &= std::isfinite (f.l) && std::isfinite (f.r) && std::isfinite (f.refL) && std::isfinite (f.refR);
        peak = std::max ({ peak, std::abs (f.l), std::abs (f.r) });
    }
    INFO ("peak = " << peak);
    CHECK (finite);
    CHECK (peak < 8.0f);   // +12 dB on a full-scale low band plus LR4 overshoot, never runaway
}

TEST_CASE ("LOW ignores non-finite settings and recovers from non-finite input", "[dsp][low][nan]")
{
    auto low = makeLow (with (50.0f, 0.0f, 0.0f));
    LowSettings bad = with (std::nanf (""), INFINITY, -INFINITY);
    bad.xoverHz = std::nanf ("");
    bad.monoHz = INFINITY;
    setSettings (low, bad);
    CHECK (low.settings.punch == 50.0f);
    CHECK (low.settings.sustain == 0.0f);
    CHECK (low.settings.xoverHz == 100.0f);

    process (low, std::nanf (""), 0.0f);
    CHECK (resetIfNotFinite (low));
    CHECK_FALSE (resetIfNotFinite (low));
    for (int n = 0; n < 1000; ++n)
    {
        const auto f = process (low, 0.0f, 0.0f);
        REQUIRE (f.l == 0.0f);
        REQUIRE (f.r == 0.0f);
    }
    CHECK (lowGainDb (low) == 0.0f);
}

TEST_CASE ("LOW settings are clamped to the parameter ranges", "[dsp][low]")
{
    LowLayer low;
    prepare (low, kFs);
    LowSettings s;
    s.xoverHz = 5.0f;
    s.monoHz = 1e6f;
    s.punch = 400.0f;
    s.sustain = -400.0f;
    s.tight = -5.0f;
    setSettings (low, s);
    CHECK (low.settings.xoverHz == kLowMinHz);
    CHECK (low.settings.monoHz == kLowMaxHz);
    CHECK (low.settings.punch == 100.0f);
    CHECK (low.settings.sustain == -100.0f);
    CHECK (low.settings.tight == 0.0f);
}

TEST_CASE ("LOW leaves no subnormal state after loud input then long silence", "[dsp][low][denormal]")
{
    auto low = makeLow (with (100.0f, 100.0f, 100.0f));
    Noise noise;
    for (int n = 0; n < 48000; ++n)
        process (low, noise.next(), noise.next());
    for (int block = 0; block < 48000 * 30 / 256; ++block)   // 30 s: a bare release multiply goes subnormal at ~26 s
    {
        for (int n = 0; n < 256; ++n)
            process (low, 0.0f, 0.0f);
        flushDenormals (low);
    }
    bool subnormal = false;
    forEachFloatState (low, [&] (float v) { subnormal |= isSubnormal (v); });
    CHECK_FALSE (subnormal);
}

TEST_CASE ("LOW behaves the same at 44.1 and 192 kHz", "[dsp][low][rate]")
{
    for (const double fs : { 44100.0, 192000.0 })
    {
        INFO ("fs = " << fs);
        auto steady = makeLow (with (100.0f, 100.0f, 100.0f), fs);
        const auto s = runSine (steady, 40.0, static_cast<int> (fs), [] (int) { return 0.5; }, fs);
        CHECK (std::abs (maxIn (s.gainDb, static_cast<int> (fs / 2), static_cast<int> (fs))) < 0.05f);

        const int onset = ms (200, fs);
        auto hit = makeLow (with (100.0f, 0.0f, 0.0f), fs);
        const auto h = runSine (hit, 60.0, onset + ms (100, fs), [&] (int n) { return n < onset ? 0.0 : 0.5; }, fs);
        CHECK (maxIn (h.gainDb, onset, onset + ms (20, fs)) > 6.0f);

        auto n0 = makeLow (neutral(), fs);
        Noise noise;
        for (int i = 0; i < 4096; ++i)
        {
            const auto f = process (n0, noise.next(), noise.next());
            REQUIRE (f.l == f.refL);
        }
    }
}
