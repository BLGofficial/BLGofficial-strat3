// Depth blend tests. Every layer's output is (1-k)*delayedDry + k*wet; if the dry delay does
// not match the layer latency, depth < 100% becomes a comb filter on the whole master.
#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "dsp/core/DelayLine.h"
#include "dsp/layers/DepthBlend.h"
#include "support/Analysis.h"

using namespace strat3::dsp;
using namespace strat3::test;

namespace {

constexpr double kFs = 48000.0;

/** Stand-in for a layer: a pure delay of `latency` samples times `gain`. */
struct FakeLayer
{
    DelayLine delay;
    float gain = 1.0f;
};

FakeLayer makeFakeLayer (int latency, float gain)
{
    FakeLayer f;
    prepare (f.delay, static_cast<size_t> (latency));
    setDelay (f.delay, static_cast<size_t> (latency));
    f.gain = gain;
    return f;
}

float runOne (DepthBlend<1>& blend, FakeLayer& layer, float x)
{
    const float wet = layer.gain * process (layer.delay, x);
    float out = 0.0f;
    process (blend, &x, &wet, &out);
    return out;
}

} // namespace

TEST_CASE ("depth 0 outputs the delayed dry signal bit-exactly (null)", "[dsp][depth][null]")
{
    for (const int latency : { 0, 1, 64, 2304 })
    {
        INFO ("latency = " << latency);
        DepthBlend<1> blend;
        prepare (blend, kFs, 4096);
        setLatency (blend, latency);
        setDepth (blend, 0.0f, true);
        snapToTarget (blend);

        auto layer = makeFakeLayer (latency, 0.25f);   // wet clearly different from dry
        DelayLine reference;
        prepare (reference, static_cast<size_t> (latency));
        setDelay (reference, static_cast<size_t> (latency));

        Noise noise;
        for (int i = 0; i < 10000; ++i)
        {
            const float x = noise.next();
            REQUIRE (runOne (blend, layer, x) == process (reference, x));
        }
    }
}

TEST_CASE ("depth 100 outputs the wet signal bit-exactly", "[dsp][depth]")
{
    DepthBlend<1> blend;
    prepare (blend, kFs, 4096);
    setLatency (blend, 64);
    setDepth (blend, 100.0f, true);
    snapToTarget (blend);
    auto layer = makeFakeLayer (64, 0.3f);
    auto mirror = makeFakeLayer (64, 0.3f);

    Noise noise;
    for (int i = 0; i < 10000; ++i)
    {
        const float x = noise.next();
        REQUIRE (runOne (blend, layer, x) == mirror.gain * process (mirror.delay, x));
    }
}

TEST_CASE ("matched dry delay: 50% depth of an identity layer is exactly the delayed input", "[dsp][depth][latency]")
{
    // With a unity-gain layer, any dry/wet misalignment shows up as comb filtering.
    for (const int latency : { 0, 1, 7, 64, 2304 })
    {
        INFO ("latency = " << latency);
        DepthBlend<1> blend;
        prepare (blend, kFs, 4096);
        setLatency (blend, latency);
        setDepth (blend, 50.0f, true);
        snapToTarget (blend);

        auto layer = makeFakeLayer (latency, 1.0f);
        auto reference = makeFakeLayer (latency, 1.0f);

        Noise noise;
        double maxErr = 0.0;
        for (int i = 0; i < 20000; ++i)
        {
            const float x = noise.next();
            maxErr = std::max (maxErr, static_cast<double> (std::abs (runOne (blend, layer, x) - process (reference.delay, x))));
        }
        CHECK (maxErr < 1e-6);
    }
}

TEST_CASE ("depth changes ramp without steps", "[dsp][depth][smoothing]")
{
    DepthBlend<1> blend;
    prepare (blend, kFs, 0);
    setDepth (blend, 0.0f, true);
    snapToTarget (blend);
    setDepth (blend, 150.0f, true);

    // dry = 0, wet = 1: output equals k, so each step shows the ramp increment.
    const int rampSamples = static_cast<int> (std::lround (static_cast<double> (kDepthRampMs) * 0.001 * kFs));
    float previous = 0.0f, maxStep = 0.0f;
    for (int i = 0; i < rampSamples + 10; ++i)
    {
        const float dry = 0.0f, wet = 1.0f;
        float out = 0.0f;
        process (blend, &dry, &wet, &out);
        maxStep = std::max (maxStep, std::abs (out - previous));
        previous = out;
    }
    CHECK (previous == 1.5f);
    CHECK (maxStep <= 1.5f / static_cast<float> (rampSamples) * 1.001f);
}

TEST_CASE ("depth 150 extrapolates: -0.5 * dry + 1.5 * wet", "[dsp][depth][extreme]")
{
    DepthBlend<1> blend;
    prepare (blend, kFs, 0);
    setDepth (blend, 150.0f, true);
    snapToTarget (blend);
    const float dry = 0.4f, wet = 0.2f;
    float out = 0.0f;
    process (blend, &dry, &wet, &out);
    CHECK (std::abs (out - (-0.5f * 0.4f + 1.5f * 0.2f)) < 1e-7f);
}

TEST_CASE ("off fades to dry, reports bypassed once silent, and fades back in", "[dsp][depth][onoff]")
{
    DepthBlend<1> blend;
    prepare (blend, kFs, 0);
    setDepth (blend, 100.0f, true);
    snapToTarget (blend);
    CHECK_FALSE (canSkipWet (blend));

    setDepth (blend, 100.0f, false);   // layer switched off
    CHECK_FALSE (canSkipWet (blend));  // still fading: wet must keep running
    const float dry = 1.0f, wet = 0.0f;
    float out = 0.0f;
    for (int i = 0; i < 48000; ++i)
        process (blend, &dry, &wet, &out);
    CHECK (out == 1.0f);
    CHECK (canSkipWet (blend));

    setDepth (blend, 100.0f, true);
    CHECK_FALSE (canSkipWet (blend));
    process (blend, &dry, &wet, &out);
    CHECK (out > 0.99f);   // first sample after re-enable is still almost all dry: no jump
}

TEST_CASE ("delta: identity layer at full delta is silent", "[dsp][depth][delta]")
{
    DepthBlend<1> blend;
    prepare (blend, kFs, 64);
    setLatency (blend, 64);
    setDepth (blend, 100.0f, true);
    setDelta (blend, true);
    snapToTarget (blend);

    auto layer = makeFakeLayer (64, 1.0f);
    Noise noise;
    for (int i = 0; i < 5000; ++i)
        REQUIRE (std::abs (runOne (blend, layer, noise.next())) < 1e-7f);
}

TEST_CASE ("delta: output is exactly what the layer added", "[dsp][depth][delta]")
{
    DepthBlend<1> blend;
    prepare (blend, kFs, 0);
    setDepth (blend, 100.0f, true);
    setDelta (blend, true);
    snapToTarget (blend);
    const float dry = 0.5f, wet = 0.8f;
    float out = 0.0f;
    process (blend, &dry, &wet, &out);
    CHECK (std::abs (out - 0.3f) < 1e-7f);
}

TEST_CASE ("non-finite depth is ignored; latency is clamped to capacity", "[dsp][depth][extreme]")
{
    DepthBlend<2> blend;
    prepare (blend, kFs, 100);
    setDepth (blend, 40.0f, true);
    snapToTarget (blend);
    setDepth (blend, NAN, true);
    snapToTarget (blend);
    CHECK (currentGain (blend) == 0.4f);

    setLatency (blend, 1'000'000);
    CHECK (latencySamples (blend) == 100);
}

TEST_CASE ("multi-channel blend treats every channel identically", "[dsp][depth]")
{
    DepthBlend<8> blend;
    prepare (blend, kFs, 16);
    setLatency (blend, 16);
    setDepth (blend, 70.0f, true);
    snapToTarget (blend);

    std::array<float, 8> dry {}, wet {}, out {};
    Noise noise;
    for (int i = 0; i < 1000; ++i)
    {
        const float x = noise.next();
        dry.fill (x);
        wet.fill (0.5f * x);
        process (blend, dry.data(), wet.data(), out.data());
        for (size_t c = 1; c < 8; ++c)
            REQUIRE (out[c] == out[0]);
    }
}
