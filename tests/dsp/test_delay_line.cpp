// Delay line tests. This is the dry-path compensation for every layer: an off-by-one here
// turns depth < 100% into a comb filter.
#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "dsp/core/DelayLine.h"
#include "support/Analysis.h"

using namespace strat3::dsp;
using namespace strat3::test;

TEST_CASE ("delay 0 is a wire (null)", "[dsp][delay][null]")
{
    DelayLine d;
    prepare (d, 64);
    setDelay (d, 0);
    Noise noise;
    for (int i = 0; i < 1000; ++i)
    {
        const float x = noise.next();
        REQUIRE (process (d, x) == x);
    }
}

TEST_CASE ("impulse comes out exactly N samples later", "[dsp][delay][impulse]")
{
    for (const size_t n : { size_t { 1 }, size_t { 7 }, size_t { 64 }, size_t { 2048 }, size_t { 4095 } })
    {
        INFO ("delay = " << n);
        DelayLine d;
        prepare (d, 4095);
        setDelay (d, n);
        const auto ir = impulseResponse ([&] (float x) { return process (d, x); }, n + 16);
        for (size_t i = 0; i < ir.size(); ++i)
            REQUIRE (ir[i] == (i == n ? 1.0f : 0.0f));
    }
}

TEST_CASE ("requested delay is clamped to prepared capacity", "[dsp][delay][extreme]")
{
    DelayLine d;
    prepare (d, 100);
    setDelay (d, 1'000'000);
    CHECK (getDelay (d) == 100);
    const auto ir = impulseResponse ([&] (float x) { return process (d, x); }, 128);
    CHECK (ir[100] == 1.0f);
}

TEST_CASE ("unprepared delay line passes audio through instead of crashing", "[dsp][delay][extreme]")
{
    DelayLine d;   // no prepare: e.g. host calls process before prepareToPlay
    setDelay (d, 10);
    CHECK (process (d, 0.5f) == 0.5f);
}

TEST_CASE ("reset clears history", "[dsp][delay]")
{
    DelayLine d;
    prepare (d, 32);
    setDelay (d, 8);
    for (int i = 0; i < 32; ++i)
        process (d, 1.0f);
    reset (d);
    for (int i = 0; i < 32; ++i)
        REQUIRE (process (d, 0.0f) == 0.0f);
}

TEST_CASE ("block processing matches per-sample processing (block size 1 vs 512)", "[dsp][delay][block]")
{
    DelayLine a, b;
    prepare (a, 1000);
    prepare (b, 1000);
    setDelay (a, 333);
    setDelay (b, 333);

    Noise noise;
    std::vector<float> input (4096);
    for (auto& v : input)
        v = noise.next();

    std::vector<float> outA (input.size());
    for (size_t i = 0; i < input.size(); ++i)
        outA[i] = process (a, input[i]);

    std::vector<float> outB = input;
    for (size_t off = 0; off < outB.size(); off += 512)
        processBlock (b, outB.data() + off, 512);   // in place

    CHECK (outA == outB);
}

TEST_CASE ("NaN in the line is flushed by sanitise without touching valid samples", "[dsp][delay][nan]")
{
    DelayLine d;
    prepare (d, 16);
    setDelay (d, 4);
    process (d, std::nanf (""));
    CHECK (sanitise (d));                 // reports the reset
    for (int i = 0; i < 16; ++i)
        REQUIRE (std::isfinite (process (d, 0.0f)));
    CHECK_FALSE (sanitise (d));
}
