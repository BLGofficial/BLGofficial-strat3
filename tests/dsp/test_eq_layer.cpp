// EQ layer tests: 8 bands of TPT SVF sections (static + dynamic), stereo or mid/side.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <numbers>

#include "dsp/core/SvfTpt.h"
#include "dsp/eq/EqLayer.h"
#include "dsp/eq/EqResponse.h"
#include "support/Analysis.h"

using namespace strat3::dsp;
using namespace strat3::test;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double kFs = 48000.0;

EqBand bell (float hz, float gainDb, float q, EqChannel ch = EqChannel::Stereo)
{
    EqBand b;
    b.on = true;
    b.type = EqType::Bell;
    b.freq = hz;
    b.gainDb = gainDb;
    b.q = q;
    b.channel = ch;
    return b;
}

/** Impulse response of the left channel (right fed the same impulse). */
std::vector<float> leftIr (EqLayer& eq, size_t length = 32768)
{
    std::vector<float> ir (length);
    for (size_t n = 0; n < length; ++n)
    {
        float l = n == 0 ? 1.0f : 0.0f, r = l;
        process (eq, l, r);
        ir[n] = l;
    }
    return ir;
}

void prepareWith (EqLayer& eq, const EqSettings& s, double fs = kFs)
{
    prepare (eq, fs);
    setBands (eq, s);
    snapToTargets (eq);
}

double sineLevelDb (EqLayer& eq, double hz, float amplitude, int settleSamples, int measureSamples, bool sideOnly = false)
{
    const double w = 2.0 * std::numbers::pi * hz / kFs;
    double peak = 0.0;
    for (int i = 0; i < settleSamples + measureSamples; ++i)
    {
        const float x = static_cast<float> (static_cast<double> (amplitude) * std::sin (w * i));
        float l = x, r = sideOnly ? -x : x;
        process (eq, l, r);
        if (i >= settleSamples)
            peak = std::max (peak, static_cast<double> (std::abs (l)));
    }
    return 20.0 * std::log10 (peak / static_cast<double> (amplitude));
}

} // namespace

TEST_CASE ("eq: all bands off is bit-exact passthrough", "[dsp][eq][null]")
{
    EqLayer eq;
    prepareWith (eq, EqSettings {});
    Noise noise;
    for (int i = 0; i < 20000; ++i)
    {
        const float xl = noise.next(), xr = noise.next();
        float l = xl, r = xr;
        process (eq, l, r);
        REQUIRE (l == xl);
        REQUIRE (r == xr);
    }
}

TEST_CASE ("eq: switched-off bands with non-zero settings are bit-exact passthrough", "[dsp][eq][null]")
{
    EqSettings s;
    s[0] = bell (1000.0f, 9.0f, 2.0f);
    s[1] = bell (300.0f, -6.0f, 1.0f, EqChannel::Side);
    s[2].type = EqType::PassiveShelf; s[2].gainDb = 6.0f; s[2].freq = 100.0f;
    for (auto& b : s)
        b.on = false;
    EqLayer eq;
    prepareWith (eq, s);
    Noise noise;
    for (int i = 0; i < 20000; ++i)
    {
        const float xl = noise.next(), xr = noise.next();
        float l = xl, r = xr;
        process (eq, l, r);
        REQUIRE (l == xl);
        REQUIRE (r == xr);
    }
}

TEST_CASE ("eq: eight 0 dB stereo bells are bit-exact passthrough", "[dsp][eq][null]")
{
    EqSettings s;
    for (int b = 0; b < kEqBands; ++b)
        s[static_cast<size_t> (b)] = bell (100.0f * static_cast<float> (b + 1), 0.0f, 1.0f);
    EqLayer eq;
    prepareWith (eq, s);
    Noise noise;
    for (int i = 0; i < 20000; ++i)
    {
        const float xl = noise.next(), xr = noise.next();
        float l = xl, r = xr;
        process (eq, l, r);
        REQUIRE (l == xl);
        REQUIRE (r == xr);
    }
}

TEST_CASE ("eq: 0 dB mid band is transparent to M/S rounding (< -126 dB)", "[dsp][eq][null]")
{
    EqSettings s;
    s[0] = bell (1000.0f, 0.0f, 1.0f, EqChannel::Mid);
    EqLayer eq;
    prepareWith (eq, s);
    Noise noise;
    float maxErr = 0.0f;
    for (int i = 0; i < 20000; ++i)
    {
        const float xl = noise.next(), xr = noise.next();
        float l = xl, r = xr;
        process (eq, l, r);
        maxErr = std::max ({ maxErr, std::abs (l - xl), std::abs (r - xr) });
    }
    // Encode/decode costs ~2 ulp at |x| <= 1 (measured 2.4e-7).
    CHECK (maxErr < 5e-7f);
}

TEST_CASE ("analytic SVF magnitude matches the measured impulse response", "[dsp][eq][response]")
{
    for (const auto type : { SvfType::LowPass, SvfType::HighPass, SvfType::BandPass, SvfType::Bell,
                             SvfType::LowShelf, SvfType::HighShelf, SvfType::Notch, SvfType::AllPass })
    {
        const auto c = makeSvfCoeffs (type, 800.0f, 1.3f, 7.0f, kFs);
        SvfState st {};
        const auto ir = impulseResponse ([&] (float x) { return processSample (st, c, x); }, 32768);
        for (const double f : { 40.0, 300.0, 800.0, 2000.0, 9000.0, 20000.0 })
        {
            INFO ("type = " << static_cast<int> (type) << " f = " << f);
            const double measured = magnitudeDb (ir, f, kFs);
            if (measured > -80.0)   // notch centre is -inf; skip unmeasurable points
                CHECK_THAT (svfMagnitudeDb (c, f, kFs), WithinAbs (measured, 0.01));
        }
    }
}

TEST_CASE ("eq: combined static bands follow the analytic response", "[dsp][eq][response]")
{
    EqSettings s;
    s[0] = bell (300.0f, 4.0f, 1.0f);
    s[1] = bell (2000.0f, -5.0f, 2.0f);
    s[2].on = true; s[2].type = EqType::HighShelf; s[2].freq = 6000.0f; s[2].gainDb = -3.0f; s[2].q = 0.707f;
    s[3].on = true; s[3].type = EqType::HighPass;  s[3].freq = 30.0f;   s[3].q = 0.707f;
    EqLayer eq;
    prepareWith (eq, s);
    const auto ir = leftIr (eq, 65536);
    for (const double f : { 25.0, 100.0, 300.0, 1000.0, 2000.0, 6000.0, 15000.0 })
    {
        INFO ("f = " << f);
        CHECK_THAT (magnitudeDb (ir, f, kFs), WithinAbs (settingsMagnitudeDb (s, f, kFs, EqChannel::Stereo), 0.02));
    }
    CHECK_THAT (magnitudeDb (ir, 300.0, kFs), WithinAbs (4.0, 0.15));   // neighbours barely touch 300 Hz
}

TEST_CASE ("eq: passive shelf boosts the lows with a dip just above the corner", "[dsp][eq][passive]")
{
    EqSettings s;
    s[0].on = true; s[0].type = EqType::PassiveShelf; s[0].freq = 100.0f; s[0].gainDb = 6.0f; s[0].q = 0.707f;
    EqLayer eq;
    prepareWith (eq, s);
    const auto ir = leftIr (eq, 65536);

    CHECK (magnitudeDb (ir, 20.0, kFs) > 5.0);
    double minDb = 1e9;
    for (double f = 100.0; f < 800.0; f *= 1.05)
        minDb = std::min (minDb, magnitudeDb (ir, f, kFs));
    CHECK (minDb < -0.5);                                     // the dip is what makes it "passive"
    CHECK_THAT (magnitudeDb (ir, 10000.0, kFs), WithinAbs (0.0, 0.1));
}

TEST_CASE ("eq: side band leaves mono untouched and cuts pure side", "[dsp][eq][ms]")
{
    EqSettings s;
    s[0] = bell (1000.0f, -12.0f, 1.0f, EqChannel::Side);
    {
        EqLayer eq;
        prepareWith (eq, s);
        CHECK_THAT (sineLevelDb (eq, 1000.0, 0.5f, 4800, 4800, false), WithinAbs (0.0, 1e-4));
    }
    {
        EqLayer eq;
        prepareWith (eq, s);
        CHECK_THAT (sineLevelDb (eq, 1000.0, 0.5f, 4800, 4800, true), WithinAbs (-12.0, 0.05));
    }
}

TEST_CASE ("eq: abrupt gain change glides instead of stepping", "[dsp][eq][smoothing]")
{
    EqSettings s;
    s[0] = bell (1000.0f, 0.0f, 1.0f);
    EqLayer eq;
    prepareWith (eq, s);

    const double w = 2.0 * std::numbers::pi * 1000.0 / kFs;
    float previous = 0.0f;
    double maxStep = 0.0;
    for (int i = 0; i < 9600; ++i)
    {
        if (i == 4800)
        {
            s[0].gainDb = 12.0f;
            setBands (eq, s);
        }
        float l = static_cast<float> (0.25 * std::sin (w * i)), r = l;
        process (eq, l, r);
        if (i > 0)
            maxStep = std::max (maxStep, static_cast<double> (std::abs (l - previous)));
        previous = l;
    }
    // +12 dB is x4: a 0.25 sine becomes 1.0, max slope 1.0 * w. A click would exceed it.
    CHECK (maxStep < 1.0 * w * 1.05);
}

TEST_CASE ("dynamic down: cuts loud in-band material by the full range, ignores quiet and off-band", "[dsp][eq][dynamic]")
{
    EqSettings s;
    s[0] = bell (1000.0f, 0.0f, 1.0f);
    s[0].dyn = EqDynMode::Down;
    s[0].threshDb = -20.0f;
    s[0].rangeDb = -6.0f;

    auto run = [&] (double hz, float amp)
    {
        EqLayer eq;
        prepareWith (eq, s);
        sineLevelDb (eq, hz, amp, 24000, 4800);
        return dynamicGainDb (eq, 0);
    };
    CHECK_THAT (run (1000.0, 0.5f),   WithinAbs (-6.0, 0.3));    // -6 dBFS: 14 dB over threshold
    CHECK_THAT (run (1000.0, 0.03f),  WithinAbs (0.0, 0.05));    // -30 dBFS: below threshold
    CHECK_THAT (run (10000.0, 0.5f),  WithinAbs (0.0, 0.3));     // loud but outside the detector band
}

TEST_CASE ("dynamic up: boosts quiet in-band material, leaves loud alone", "[dsp][eq][dynamic]")
{
    EqSettings s;
    s[0] = bell (1000.0f, 0.0f, 1.0f);
    s[0].dyn = EqDynMode::Up;
    s[0].threshDb = -20.0f;
    s[0].rangeDb = 6.0f;

    auto run = [&] (float amp)
    {
        EqLayer eq;
        prepareWith (eq, s);
        sineLevelDb (eq, 1000.0, amp, 24000, 4800);
        return dynamicGainDb (eq, 0);
    };
    CHECK_THAT (run (0.01f), WithinAbs (6.0, 0.3));   // -40 dBFS: 20 dB under threshold
    CHECK_THAT (run (0.5f),  WithinAbs (0.0, 0.05));
}

TEST_CASE ("dynamic down: attack and release follow their time constants", "[dsp][eq][dynamic][timing]")
{
    EqSettings s;
    s[0] = bell (1000.0f, 0.0f, 1.0f);
    s[0].dyn = EqDynMode::Down;
    s[0].threshDb = -20.0f;
    s[0].rangeDb = -6.0f;
    s[0].attackMs = 10.0f;
    s[0].releaseMs = 100.0f;
    EqLayer eq;
    prepareWith (eq, s);

    const double w = 2.0 * std::numbers::pi * 1000.0 / kFs;
    auto ms = [] (double t) { return static_cast<int> (t * 48.0); };
    float at3ms = 0, at40ms = 0, at80msAfterStop = 0, at400msAfterStop = 0;

    for (int i = 0; i < ms (900); ++i)
    {
        const bool on = i < ms (500);
        float l = on ? static_cast<float> (0.5 * std::sin (w * i)) : 0.0f, r = l;
        process (eq, l, r);
        if (i == ms (3))   at3ms = dynamicGainDb (eq, 0);
        if (i == ms (40))  at40ms = dynamicGainDb (eq, 0);
        if (i == ms (580)) at80msAfterStop = dynamicGainDb (eq, 0);
        if (i == ms (899)) at400msAfterStop = dynamicGainDb (eq, 0);
    }
    INFO (at3ms << " " << at40ms << " " << at80msAfterStop << " " << at400msAfterStop);
    CHECK (at3ms > -5.4f);              // not instantaneous
    CHECK (at40ms < -5.7f);             // fully engaged within a few attack constants
    CHECK (at80msAfterStop < -1.0f);    // release is still holding
    CHECK (at400msAfterStop > -0.05f);  // and has let go
}

TEST_CASE ("eq: random automation at extreme settings stays finite and recovers", "[dsp][eq][stability]")
{
    // Stability criterion: no non-finite output, and once automation stops (all bands off)
    // the output returns to an exact copy of the input. A fixed peak bound is meaningless
    // here: 8 stacked +18 dB bands at Q 18, jumping every 64 samples, legitimately reach
    // 1e7..1e9 (measured), but do not grow without bound and recover within ~130 ms.
    for (const double fs : { 44100.0, 192000.0 })
    {
        INFO ("fs = " << fs);
        EqLayer eq;
        prepare (eq, fs);
        Noise noise, mod;
        const int automationBlocks = static_cast<int> (fs) * 2 / 64;
        const int recoveryBlocks   = static_cast<int> (fs) / 64;   // 1 s with everything off

        for (int block = 0; block < automationBlocks + recoveryBlocks; ++block)
        {
            EqSettings s;
            if (block < automationBlocks)
                for (auto& b : s)
                {
                    b.on = mod.next() > -0.5f;
                    b.type = static_cast<EqType> (static_cast<int> ((mod.next() + 1.0f) * 2.99f));
                    b.channel = static_cast<EqChannel> (static_cast<int> ((mod.next() + 1.0f) * 1.49f));
                    b.dyn = static_cast<EqDynMode> (static_cast<int> ((mod.next() + 1.0f) * 1.49f));
                    b.freq = 20.0f * std::pow (1000.0f, 0.5f + 0.5f * mod.next());
                    b.gainDb = 18.0f * mod.next();
                    b.q = 0.1f * std::pow (180.0f, 0.5f + 0.5f * mod.next());
                    b.rangeDb = 12.0f * mod.next();
                    b.attackMs = 0.1f;
                }
            setBands (eq, s);

            const bool lastHalfSecond = block >= automationBlocks + recoveryBlocks / 2;
            for (int i = 0; i < 64; ++i)
            {
                const float xl = 0.1f * noise.next(), xr = 0.1f * noise.next();
                float l = xl, r = xr;
                process (eq, l, r);
                REQUIRE (std::isfinite (l));
                REQUIRE (std::isfinite (r));
                if (lastHalfSecond)
                {
                    REQUIRE (l == xl);   // glides have settled: neutral is bit-exact again
                    REQUIRE (r == xr);
                }
            }
            flushDenormals (eq);
        }
    }
}

TEST_CASE ("eq: NaN input is recovered by resetIfNotFinite", "[dsp][eq][nan]")
{
    EqSettings s;
    s[0] = bell (1000.0f, 6.0f, 1.0f);
    s[0].dyn = EqDynMode::Down;
    EqLayer eq;
    prepareWith (eq, s);
    float l = NAN, r = 0.0f;
    process (eq, l, r);
    CHECK (resetIfNotFinite (eq));
    l = 0.5f; r = 0.5f;
    process (eq, l, r);
    CHECK (std::isfinite (l));
    CHECK (std::isfinite (dynamicGainDb (eq, 0)));
    CHECK_FALSE (resetIfNotFinite (eq));
}

TEST_CASE ("eq: output does not depend on block size with constant settings", "[dsp][eq][block]")
{
    EqSettings s;
    s[0] = bell (1000.0f, 6.0f, 1.0f);
    s[0].dyn = EqDynMode::Down;
    s[1] = bell (300.0f, -4.0f, 2.0f, EqChannel::Mid);
    EqLayer a, b;
    prepareWith (a, s);
    prepareWith (b, s);
    Noise noise;
    for (int block = 0; block < 400; ++block)
    {
        // a: settings re-sent every sample (block size 1); b: once per 256-sample block.
        setBands (b, s);
        for (int i = 0; i < 256; ++i)
        {
            setBands (a, s);
            const float xl = noise.next(), xr = noise.next();
            float al = xl, ar = xr, bl = xl, br = xr;
            process (a, al, ar);
            process (b, bl, br);
            REQUIRE (al == bl);
            REQUIRE (ar == br);
        }
    }
}
