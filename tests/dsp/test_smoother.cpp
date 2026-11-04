// Smoother tests: parameter smoothing is what keeps automation click-free, and a smoother
// that decays into subnormals turns a silent plugin into a CPU spike.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

#include "dsp/core/Smoother.h"
#include "support/Analysis.h"

using namespace strat3::dsp;
using Catch::Matchers::WithinAbs;
using strat3::test::isSubnormal;

TEST_CASE ("one-pole: holds its initial value when target is unchanged (null)", "[dsp][smoother]")
{
    auto s = makeOnePole (20.0f, 48000.0, 0.75f);
    for (int i = 0; i < 4800; ++i)
        REQUIRE (step (s, 0.75f) == 0.75f);   // bit-exact: neutral smoothing must not drift
}

TEST_CASE ("one-pole: reaches 1 - 1/e of a step after one time constant", "[dsp][smoother]")
{
    for (const double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        INFO ("fs = " << fs);
        auto s = makeOnePole (10.0f, fs, 0.0f);
        const int tauSamples = static_cast<int> (std::lround (0.010 * fs));
        float v = 0.0f;
        for (int i = 0; i < tauSamples; ++i)
            v = step (s, 1.0f);
        CHECK_THAT (v, WithinAbs (1.0 - std::exp (-1.0), 2e-3));
    }
}

TEST_CASE ("one-pole: zero or negative time means jump immediately", "[dsp][smoother]")
{
    auto a = makeOnePole (0.0f, 48000.0, 0.0f);
    CHECK (step (a, 1.0f) == 1.0f);
    auto b = makeOnePole (-5.0f, 48000.0, 0.0f);
    CHECK (step (b, -2.0f) == -2.0f);
    auto c = makeOnePole (10.0f, 0.0, 0.0f);   // unprepared sample rate must not divide by zero
    CHECK (step (c, 3.0f) == 3.0f);
}

TEST_CASE ("one-pole: decaying to zero never produces subnormals", "[dsp][smoother]")
{
    auto s = makeOnePole (50.0f, 48000.0, 1.0f);
    bool sawSubnormal = false;
    float v = 1.0f;
    for (int i = 0; i < 48000 * 10; ++i)
    {
        v = step (s, 0.0f);
        sawSubnormal |= isSubnormal (v);
    }
    CHECK_FALSE (sawSubnormal);
    CHECK (v == 0.0f);   // snaps to target instead of creeping forever
}

TEST_CASE ("one-pole: NaN target is rejected, state stays finite", "[dsp][smoother]")
{
    auto s = makeOnePole (10.0f, 48000.0, 0.5f);
    const float v = step (s, std::nanf (""));
    CHECK (std::isfinite (v));
    CHECK (v == 0.5f);
    CHECK (std::isfinite (step (s, INFINITY)));
}

TEST_CASE ("linear ramp: lands exactly on target after the ramp length", "[dsp][smoother]")
{
    auto r = makeLinearRamp (10.0f, 48000.0, 0.0f);   // 480 samples
    setTarget (r, 1.0f);
    CHECK (isSmoothing (r));

    float v = 0.0f;
    for (int i = 0; i < 480; ++i)
    {
        v = step (r);
        REQUIRE (v <= 1.0f);   // never overshoots
    }
    CHECK (v == 1.0f);
    CHECK_FALSE (isSmoothing (r));
    CHECK (step (r) == 1.0f);
}

TEST_CASE ("linear ramp: retarget mid-ramp restarts from the current value without a jump", "[dsp][smoother]")
{
    auto r = makeLinearRamp (10.0f, 48000.0, 0.0f);
    setTarget (r, 1.0f);
    float v = 0.0f;
    for (int i = 0; i < 240; ++i)
        v = step (r);
    const float midway = v;

    setTarget (r, 0.0f);
    const float next = step (r);
    CHECK (std::abs (next - midway) < 1.0f / 240.0f);   // continuity: one step, not a jump
}

TEST_CASE ("linear ramp: re-sending the same target every block does not restart the ramp", "[dsp][smoother]")
{
    // Hosts and the processor push every parameter each block. If an unchanged target
    // restarted the ramp, it would approach geometrically and never land: depth 100% would
    // never be bit-exact, and every ramp would take several times its nominal length.
    auto r = makeLinearRamp (10.0f, 48000.0, 0.0f);   // 480 samples
    int samples = 0;
    while (samples < 4800 && r.current != 1.0f)
    {
        setTarget (r, 1.0f);   // once per 32-sample block
        for (int i = 0; i < 32; ++i, ++samples)
            step (r);
    }
    CHECK (r.current == 1.0f);
    CHECK (samples <= 480 + 32);
}

TEST_CASE ("linear ramp: block size 1 and block size 512 produce identical output", "[dsp][smoother]")
{
    auto a = makeLinearRamp (5.0f, 48000.0, -1.0f);
    auto b = a;
    setTarget (a, 2.0f);
    setTarget (b, 2.0f);

    // a: sample by sample; b: processed in 512-blocks via stepBlock.
    std::vector<float> outA (2048), outB (2048);
    for (auto& v : outA)
        v = step (a);
    for (size_t off = 0; off < outB.size(); off += 512)
        stepBlock (b, outB.data() + off, 512);

    CHECK (outA == outB);
}

TEST_CASE ("one-pole: converges exactly even when float rounding would stall it", "[dsp][smoother]")
{
    // Regression: with coeff ~0.9999, target + coeff*d rounded back to the previous value
    // while still ~1e-5 off target, so a smoothed coefficient never arrived.
    auto s = makeOnePole (100.0f, 192000.0, 0.0f);
    const float target = 0.54157096f;
    float v = 0.0f;
    for (int i = 0; i < 192000 * 5; ++i)   // 50 time constants
        v = step (s, target);
    CHECK (v == target);
}
