// WIDTH layer (band domain): per-band side scaling S * (1 + w), stereoize (mid through a
// 4-stage all-pass cascade, high-passed at 300 Hz, added to the side) and mono guard
// (stereoize backs off while output correlation is below 0.2). Plus the correlation meter.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <numbers>
#include <vector>

#include "analysis/Correlation.h"
#include "dsp/core/DelayLine.h"
#include "dsp/crossover/BandDomain.h"
#include "dsp/layers/WidthLayer.h"
#include "support/Analysis.h"

using namespace strat3::dsp;
using namespace strat3::analysis;
using namespace strat3::test;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double kFs = 48000.0;

WidthLayer makeWidth (const WidthSettings& s, double fs = kFs)
{
    WidthLayer w;
    prepare (w, fs);
    setSettings (w, s);
    snapToTargets (w);
    return w;
}

WidthSettings stereoize (float percent, bool guard)
{
    WidthSettings s;
    s.stereoizePercent = percent;
    s.monoGuard = guard;
    return s;
}

StereoBands noiseBands (WhiteNoise& n, bool mono)
{
    StereoBands b;
    for (size_t i = 0; i < 4; ++i)
    {
        // 0.3, not 0.25: dyadic values make the M/S round trip exact by accident.
        b.l[i] = 0.3f * n.next();
        b.r[i] = mono ? b.l[i] : 0.3f * n.next();
    }
    return b;
}

float sumL (const StereoBands& b) { return (b.l[0] + b.l[1]) + (b.l[2] + b.l[3]); }
float sumR (const StereoBands& b) { return (b.r[0] + b.r[1]) + (b.r[2] + b.r[3]); }

} // namespace

// ------------------------------------------------------------------- correlation meter

TEST_CASE ("correlation meter: +1 for mono, -1 for inverted, ~0 for independent", "[analysis][correlation]")
{
    auto measure = [] (int kind)
    {
        CorrelationMeter m;
        prepare (m, kFs);
        WhiteNoise a, b;
        b.counter = 0xBEEFu;
        for (int i = 0; i < 48000 * 2; ++i)
        {
            const float l = a.next();
            const float r = kind == 0 ? l : kind == 1 ? -l : b.next();
            push (m, l, r);
        }
        return value (m);
    };
    CHECK_THAT (measure (0), WithinAbs (1.0, 1e-4));
    CHECK_THAT (measure (1), WithinAbs (-1.0, 1e-4));
    CHECK (std::abs (measure (2)) < 0.05f);
}

TEST_CASE ("correlation meter: silence reads +1, one silent channel reads 0", "[analysis][correlation]")
{
    CorrelationMeter m;
    prepare (m, kFs);
    for (int i = 0; i < 48000; ++i)
        push (m, 0.0f, 0.0f);
    CHECK (value (m) == 1.0f);   // nothing to guard against: silence must not pull stereoize down

    WhiteNoise n;
    for (int i = 0; i < 48000 * 2; ++i)
        push (m, n.next(), 0.0f);
    CHECK (std::abs (value (m)) < 0.01f);
}

TEST_CASE ("correlation meter: 300 ms time constant", "[analysis][correlation]")
{
    // Averages go from +P to -P: the ratio crosses zero after tau * ln 2.
    CorrelationMeter m;
    prepare (m, kFs);
    WhiteNoise n;
    for (int i = 0; i < 48000 * 2; ++i)
    {
        const float x = n.next();
        push (m, x, x);
    }
    int crossing = -1;
    for (int i = 0; i < 48000 && crossing < 0; ++i)
    {
        const float x = n.next();
        push (m, x, -x);
        if (value (m) <= 0.0f)
            crossing = i;
    }
    const double expected = 0.3 * std::numbers::ln2 * kFs;
    INFO ("crossing at " << crossing << " expected " << expected);
    CHECK (std::abs (crossing - expected) < expected * 0.05);
}

// ------------------------------------------------------------------- width

TEST_CASE ("WIDTH neutral is bit-exact passthrough", "[dsp][width][null]")
{
    auto w = makeWidth (WidthSettings {});
    WhiteNoise n;
    for (int i = 0; i < 48000; ++i)
    {
        const auto in = noiseBands (n, false);
        auto out = in;
        process (w, out);
        REQUIRE (out.l == in.l);
        REQUIRE (out.r == in.r);
    }
}

TEST_CASE ("width -100% makes a band mono; other bands stay bit-exact", "[dsp][width]")
{
    WidthSettings s;
    s.widthPercent[2] = -100.0f;
    auto w = makeWidth (s);
    WhiteNoise n;
    for (int i = 0; i < 24000; ++i)
    {
        const auto in = noiseBands (n, false);
        auto out = in;
        process (w, out);
        REQUIRE (out.l[2] == out.r[2]);
        REQUIRE (std::abs (out.l[2] - 0.5f * (in.l[2] + in.r[2])) < 1e-7f);
        for (const size_t b : { 0u, 1u, 3u })
        {
            REQUIRE (out.l[b] == in.l[b]);
            REQUIRE (out.r[b] == in.r[b]);
        }
    }
}

TEST_CASE ("width +100% doubles the side and leaves the mid alone", "[dsp][width]")
{
    WidthSettings s;
    s.widthPercent = { 100.0f, 100.0f, 100.0f, 100.0f };
    auto w = makeWidth (s);
    WhiteNoise n;
    for (int i = 0; i < 24000; ++i)
    {
        const auto in = noiseBands (n, false);
        auto out = in;
        process (w, out);
        for (size_t b = 0; b < 4; ++b)
        {
            REQUIRE (std::abs ((out.l[b] + out.r[b]) - (in.l[b] + in.r[b])) < 1e-6f);
            REQUIRE (std::abs ((out.l[b] - out.r[b]) - 2.0f * (in.l[b] - in.r[b])) < 1e-6f);
        }
    }
}

TEST_CASE ("mono sum is preserved for any width and stereoize setting", "[dsp][width][mono]")
{
    // The point of M/S width and side-only stereoize: folding to mono never changes level.
    WhiteNoise n, automation;
    auto w = makeWidth (WidthSettings {});
    double worst = 0.0;
    for (int i = 0; i < 48000 * 4; ++i)
    {
        if (i % 4800 == 0)
        {
            WidthSettings s;
            for (auto& v : s.widthPercent)
                v = 100.0f * automation.next();
            s.stereoizePercent = 50.0f * (automation.next() + 1.0f);
            s.monoGuard = automation.next() > 0.0f;
            setSettings (w, s);
        }
        const auto in = noiseBands (n, i % 2 == 0);
        auto out = in;
        process (w, out);
        for (size_t b = 0; b < 4; ++b)
            worst = std::max (worst, static_cast<double> (std::abs ((out.l[b] + out.r[b]) - (in.l[b] + in.r[b]))));
    }
    CHECK (worst < 1e-6);
}

TEST_CASE ("width changes glide (no step in the side signal)", "[dsp][width]")
{
    auto w = makeWidth (WidthSettings {});
    WidthSettings mono;
    mono.widthPercent = { -100.0f, -100.0f, -100.0f, -100.0f };
    setSettings (w, mono);
    StereoBands x;
    x.l[1] = 0.5f;
    x.r[1] = -0.5f;   // pure side, constant
    auto first = x;
    process (w, first);
    CHECK (first.l[1] > 0.49f);   // one sample in: barely moved

    float sideAt100ms = 1.0f;
    for (int i = 1; i < static_cast<int> (0.1 * kFs); ++i)
    {
        auto y = x;
        process (w, y);
        sideAt100ms = y.l[1];
    }
    CHECK (std::abs (sideAt100ms) < 1e-6f);   // fully mono well within 100 ms
}

// ------------------------------------------------------------------- stereoize

TEST_CASE ("stereoize adds side only above ~300 Hz", "[dsp][width][stereoize]")
{
    for (const double fs : { 44100.0, 48000.0, 192000.0 })
    {
        auto w = makeWidth (stereoize (100.0f, false), fs);
        // Mono impulse in every band: the side output is the stereoize response of the mid.
        std::vector<float> side (static_cast<size_t> (fs));
        for (size_t i = 0; i < side.size(); ++i)
        {
            StereoBands x;
            const float v = i == 0 ? 0.25f : 0.0f;   // 4 bands x 0.25 = unit mid
            x.l = x.r = { v, v, v, v };
            process (w, x);
            side[i] = 0.5f * (sumL (x) - sumR (x));
        }
        INFO ("fs = " << fs);
        CHECK (magnitudeDb (side, 100.0, fs) < -30.0);
        CHECK (magnitudeDb (side, 50.0, fs) < -40.0);
        for (const double f : { 1000.0, 3000.0, 10000.0 })
        {
            INFO ("f = " << f);
            CHECK_THAT (magnitudeDb (side, f, fs), WithinAbs (0.0, 1.0));   // all-pass: full level
        }
    }
}

TEST_CASE ("stereoize spreads frequencies across the image instead of panning them", "[dsp][width][stereoize]")
{
    // Side-only stereoize places each frequency between L and R by the all-pass phase. Without
    // the all-passes (D in phase with M) every frequency would lean the same way: a pan.
    auto w = makeWidth (stereoize (100.0f, false));
    std::vector<float> irL (48000), irR (48000);
    for (size_t i = 0; i < irL.size(); ++i)
    {
        StereoBands x;
        const float v = i == 0 ? 0.25f : 0.0f;
        x.l = x.r = { v, v, v, v };
        process (w, x);
        irL[i] = sumL (x);
        irR[i] = sumR (x);
    }
    int left = 0, right = 0;
    for (double f = 1000.0; f <= 16000.0; f *= 1.05)
    {
        const double balance = magnitudeDb (irL, f, kFs) - magnitudeDb (irR, f, kFs);
        left += balance > 3.0;
        right += balance < -3.0;
    }
    INFO ("bins leaning left " << left << " right " << right);
    CHECK (left >= 5);
    CHECK (right >= 5);

    // Broadband: no overall lean on mono noise.
    WhiteNoise n;
    double el = 0.0, er = 0.0;
    for (int i = 0; i < 48000 * 2; ++i)
    {
        auto x = noiseBands (n, true);
        process (w, x);
        if (i >= 48000)
        {
            el += static_cast<double> (sumL (x)) * static_cast<double> (sumL (x));
            er += static_cast<double> (sumR (x)) * static_cast<double> (sumR (x));
        }
    }
    INFO ("broadband L/R balance " << 10.0 * std::log10 (el / er) << " dB");
    CHECK (std::abs (10.0 * std::log10 (el / er)) < 1.5);
}

TEST_CASE ("stereoize 100% without guard decorrelates mono noise", "[dsp][width][stereoize]")
{
    auto w = makeWidth (stereoize (100.0f, false));
    WhiteNoise n;
    for (int i = 0; i < 48000 * 3; ++i)
    {
        auto x = noiseBands (n, true);
        process (w, x);
    }
    INFO ("correlation " << outputCorrelation (w));
    CHECK (outputCorrelation (w) < 0.2f);
    CHECK (stereoizeGuard (w) == 1.0f);   // guard off: never engages
}

TEST_CASE ("mono guard holds correlation near 0.2 instead of letting it fall", "[dsp][width][guard]")
{
    auto w = makeWidth (stereoize (100.0f, true));
    WhiteNoise n;
    float minCorr = 1.0f;
    double sumCorr = 0.0;
    int count = 0;
    for (int i = 0; i < 48000 * 8; ++i)
    {
        auto x = noiseBands (n, true);
        process (w, x);
        if (i >= 48000 * 3)
        {
            minCorr = std::min (minCorr, outputCorrelation (w));
            sumCorr += static_cast<double> (outputCorrelation (w));
            ++count;
        }
    }
    const double meanCorr = sumCorr / count;
    INFO ("after settling: min corr " << minCorr << " mean corr " << meanCorr << " guard " << stereoizeGuard (w));
    CHECK (minCorr > 0.1f);
    CHECK (meanCorr > 0.18);
    CHECK (meanCorr < 0.5);           // still stereoizing, not shut off
    CHECK (stereoizeGuard (w) < 1.0f);
    CHECK (stereoizeGuard (w) > 0.2f);
}

TEST_CASE ("mono guard parks instead of hunting", "[dsp][width][guard]")
{
    // Hysteresis between 0.2 and 0.3: once correlation sits inside the band the guard holds
    // still. Without it the amount would step up and down every control tick (AM on the side).
    auto w = makeWidth (stereoize (100.0f, true));
    WhiteNoise n;
    float prev = stereoizeGuard (w), lastDelta = 0.0f;
    int reversals = 0;
    for (int i = 0; i < 48000 * 8; ++i)
    {
        auto x = noiseBands (n, true);
        process (w, x);
        const float g = stereoizeGuard (w), delta = g - prev;
        if (i >= 48000 * 4 && delta != 0.0f)
        {
            if (lastDelta != 0.0f && (delta > 0.0f) != (lastDelta > 0.0f))
                ++reversals;
            lastDelta = delta;
        }
        prev = g;
    }
    INFO ("direction reversals in the last 4 s: " << reversals);
    CHECK (reversals <= 8);
}

TEST_CASE ("mono guard releases once correlation recovers", "[dsp][width][guard]")
{
    auto w = makeWidth (stereoize (100.0f, true));
    WhiteNoise n;
    for (int i = 0; i < 48000 * 4; ++i)
    {
        auto x = noiseBands (n, true);
        process (w, x);
    }
    REQUIRE (stereoizeGuard (w) < 0.95f);
    setSettings (w, stereoize (20.0f, true));   // mild: correlation well above 0.3
    for (int i = 0; i < 48000 * 3; ++i)
    {
        auto x = noiseBands (n, true);
        process (w, x);
    }
    CHECK (stereoizeGuard (w) == 1.0f);
}

TEST_CASE ("mono guard stays out of the way when correlation is healthy", "[dsp][width][guard]")
{
    auto w = makeWidth (stereoize (30.0f, true));   // mild: correlation stays ~0.8
    WhiteNoise n;
    float minGuard = 1.0f;
    for (int i = 0; i < 48000 * 4; ++i)
    {
        auto x = noiseBands (n, true);
        process (w, x);
        minGuard = std::min (minGuard, stereoizeGuard (w));
    }
    CHECK (minGuard == 1.0f);
    CHECK (outputCorrelation (w) > 0.5f);
}

TEST_CASE ("mono guard never touches the width controls", "[dsp][width][guard]")
{
    // Independent L/R (correlation ~0) with width +100%: the guard must not narrow it.
    WidthSettings s;
    s.widthPercent = { 100.0f, 100.0f, 100.0f, 100.0f };
    s.monoGuard = true;
    auto w = makeWidth (s);
    WhiteNoise n;
    double worst = 0.0;
    for (int i = 0; i < 48000 * 2; ++i)
    {
        const auto in = noiseBands (n, false);
        auto out = in;
        process (w, out);
        for (size_t b = 0; b < 4; ++b)
            worst = std::max (worst, static_cast<double> (std::abs ((out.l[b] - out.r[b]) - 2.0f * (in.l[b] - in.r[b]))));
    }
    CHECK (worst < 1e-6);
}

TEST_CASE ("stereoize amount changes glide", "[dsp][width][stereoize]")
{
    auto w = makeWidth (WidthSettings {});
    const double om = 2.0 * std::numbers::pi * 2000.0 / kFs;
    float prev = 0.0f;
    double maxStep = 0.0;
    for (int i = 0; i < 48000; ++i)
    {
        if (i == 12000)
            setSettings (w, stereoize (100.0f, false));
        StereoBands x;
        const float v = static_cast<float> (0.25 * std::sin (om * i));
        x.l[3] = x.r[3] = v;
        process (w, x);
        if (i > 100)
            maxStep = std::max (maxStep, static_cast<double> (std::abs (x.l[3] - prev)));
        prev = x.l[3];
    }
    // |L| <= 0.25 * (1 + 1): a 2 kHz sine of that amplitude moves at most 0.5 * om per sample.
    INFO ("max step " << maxStep << " bound " << 0.5 * om);
    CHECK (maxStep < 0.5 * om * 1.05);
}

// ------------------------------------------------------------------- integration / robustness

TEST_CASE ("WIDTH in the FIR band domain: neutral layer keeps the band-domain null", "[dsp][width][band]")
{
    BandDomain domain;
    prepare (domain, SplitMode::Fir, kDefaultCrossovers, kFs, 256);
    const int latency = latencySamples (domain);
    auto w = makeWidth (WidthSettings {});
    DelayLine ref;
    prepare (ref, static_cast<size_t> (latency));
    setDelay (ref, static_cast<size_t> (latency));
    WhiteNoise n;
    double maxErr = 0.0;
    for (int i = 0; i < latency + 20000; ++i)
    {
        const float l = n.next(), r = n.next();
        auto bands = split (domain, l, r);
        process (w, bands);
        float ol, orr;
        merge (bands, ol, orr);
        const float d = process (ref, l);
        if (i >= latency)
            maxErr = std::max (maxErr, static_cast<double> (std::abs (ol - d)));
    }
    CHECK (maxErr < 2e-5);
}

TEST_CASE ("WIDTH is stable under random automation", "[dsp][width][stability]")
{
    auto w = makeWidth (WidthSettings {});
    WhiteNoise n, automation;
    bool finite = true;
    float inPeak = 0.0f, outPeak = 0.0f;
    for (int i = 0; i < 48000 * 10; ++i)
    {
        if (i % 64 == 0)
        {
            WidthSettings s;
            for (auto& v : s.widthPercent)
                v = 100.0f * automation.next();
            s.stereoizePercent = 50.0f * (automation.next() + 1.0f);
            s.monoGuard = automation.next() > 0.0f;
            setSettings (w, s);
        }
        auto x = noiseBands (n, (i / 4800) % 2 == 0);
        for (size_t b = 0; b < 4; ++b)
            inPeak = std::max ({ inPeak, std::abs (x.l[b]), std::abs (x.r[b]) });
        process (w, x);
        for (size_t b = 0; b < 4; ++b)
        {
            finite &= std::isfinite (x.l[b]) && std::isfinite (x.r[b]);
            outPeak = std::max ({ outPeak, std::abs (x.l[b]), std::abs (x.r[b]) });
        }
    }
    INFO ("in peak " << inPeak << " out peak " << outPeak);
    CHECK (finite);
    CHECK (outPeak < inPeak * 6.0f);   // |M| + 2|S| + all-pass/HP overshoot of the stereoize path
}

TEST_CASE ("WIDTH ignores non-finite settings, clamps ranges, recovers from NaN input", "[dsp][width][nan]")
{
    WidthLayer w;
    prepare (w, kFs);
    WidthSettings s;
    s.widthPercent = { 150.0f, -150.0f, std::nanf (""), 20.0f };
    s.stereoizePercent = 200.0f;
    setSettings (w, s);
    CHECK (w.settings.widthPercent[0] == 100.0f);
    CHECK (w.settings.widthPercent[1] == -100.0f);
    CHECK (w.settings.widthPercent[2] == 0.0f);   // kept previous (default)
    CHECK (w.settings.stereoizePercent == 100.0f);
    s.stereoizePercent = INFINITY;
    setSettings (w, s);
    CHECK (w.settings.stereoizePercent == 100.0f);

    StereoBands x;
    x.l[3] = std::nanf ("");
    process (w, x);
    CHECK (resetIfNotFinite (w));
    CHECK_FALSE (resetIfNotFinite (w));
    for (int i = 0; i < 2000; ++i)
    {
        StereoBands z;
        process (w, z);
        for (size_t b = 0; b < 4; ++b)
            REQUIRE ((z.l[b] == 0.0f && z.r[b] == 0.0f));
    }
}

TEST_CASE ("WIDTH leaves no subnormal state after loud input then long silence", "[dsp][width][denormal]")
{
    WidthSettings s = stereoize (100.0f, true);
    s.widthPercent = { 50.0f, -50.0f, 100.0f, 0.0f };
    auto w = makeWidth (s);
    WhiteNoise n;
    for (int i = 0; i < 48000; ++i)
    {
        auto x = noiseBands (n, false);
        process (w, x);
    }
    for (int block = 0; block < 48000 * 30 / 256; ++block)
    {
        for (int i = 0; i < 256; ++i)
        {
            StereoBands z;
            process (w, z);
        }
        flushDenormals (w);
    }
    bool subnormal = false;
    forEachFloatState (w, [&] (float v) { subnormal |= isSubnormal (v); });
    CHECK_FALSE (subnormal);
}
