// Band domain tests: split -> per-band layers -> merge, in IIR (TRACK/MIX) and FIR (MASTER)
// modes, including the background redesign + warm-up + crossfade of FIR kernels.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cmath>
#include <numbers>
#include <thread>

#include "dsp/core/DelayLine.h"
#include "dsp/crossover/BandDomain.h"
#include "support/Analysis.h"

using namespace strat3::dsp;
using namespace strat3::test;

namespace {

constexpr double kFs = 48000.0;

struct StereoDelay
{
    DelayLine l, r;
};

StereoDelay makeStereoDelay (int samples)
{
    StereoDelay d;
    for (auto* line : { &d.l, &d.r })
    {
        prepare (*line, static_cast<size_t> (samples));
        setDelay (*line, static_cast<size_t> (samples));
    }
    return d;
}

} // namespace

TEST_CASE ("IIR band domain: split + merge with neutral bands is flat (all-pass only)", "[dsp][band][iir]")
{
    BandDomain domain;
    prepare (domain, SplitMode::Iir, kDefaultCrossovers, kFs, 256);
    CHECK (latencySamples (domain) == 0);

    std::vector<float> irL (65536), irR (65536);
    for (size_t n = 0; n < irL.size(); ++n)
    {
        const float x = n == 0 ? 1.0f : 0.0f;
        auto bands = split (domain, x, -x);
        merge (bands, irL[n], irR[n]);
    }
    for (const double f : { 30.0, 120.0, 1000.0, 5000.0, 15000.0 })
    {
        INFO ("f = " << f);
        CHECK (std::abs (magnitudeDb (irL, f, kFs)) < 0.01);
        CHECK (std::abs (magnitudeDb (irR, f, kFs)) < 0.01);
    }
}

TEST_CASE ("FIR band domain: neutral split + merge nulls against the delayed input", "[dsp][band][fir][null]")
{
    BandDomain domain;
    prepare (domain, SplitMode::Fir, kDefaultCrossovers, kFs, 256);
    const int latency = latencySamples (domain);
    REQUIRE (latency > 0);
    auto ref = makeStereoDelay (latency);

    Noise noise;
    double maxErr = 0.0;
    for (int i = 0; i < latency + 20000; ++i)
    {
        const float l = noise.next(), r = noise.next();
        float outL, outR;
        merge (split (domain, l, r), outL, outR);
        const float refL = process (ref.l, l), refR = process (ref.r, r);
        if (i >= latency)
            maxErr = std::max ({ maxErr, static_cast<double> (std::abs (outL - refL)), static_cast<double> (std::abs (outR - refR)) });
    }
    CHECK (maxErr < 2e-5);
}

TEST_CASE ("FIR crossover swap keeps the neutral null through warm-up and crossfade", "[dsp][band][fir][swap]")
{
    BandDomain domain;
    prepare (domain, SplitMode::Fir, kDefaultCrossovers, kFs, 256);
    const int latency = latencySamples (domain);
    auto ref = makeStereoDelay (latency);
    Noise noise;
    double maxErr = 0.0;
    bool sawSwap = false;

    for (int i = 0; i < 48000 * 3; ++i)
    {
        // "Background" redesign done synchronously here; the concurrency test covers threads.
        if (i == latency + 1000)
            REQUIRE (designStandby (domain, { 200.0f, 2000.0f, 8000.0f }));

        beginBlock (domain);
        sawSwap |= isSwapping (domain);

        const float l = noise.next(), r = noise.next();
        float outL, outR;
        merge (split (domain, l, r), outL, outR);
        const float refL = process (ref.l, l), refR = process (ref.r, r);
        if (i >= latency)
            maxErr = std::max ({ maxErr, static_cast<double> (std::abs (outL - refL)), static_cast<double> (std::abs (outR - refR)) });
    }
    CHECK (sawSwap);
    CHECK_FALSE (isSwapping (domain));
    CHECK (activeCrossovers (domain).f2 == 2000.0f);
    CHECK (maxErr < 2e-5);   // warm-up too short would break the null mid-fade
}

TEST_CASE ("FIR crossover swap with non-neutral bands is click-free", "[dsp][band][fir][swap]")
{
    // Band 2 attenuated by 12 dB: moving f1/f2 changes the output, but only smoothly.
    BandDomain domain;
    prepare (domain, SplitMode::Fir, kDefaultCrossovers, kFs, 256);
    const int latency = latencySamples (domain);

    const double freq = 700.0;   // inside band 2 before the move, band 1/2 edge after
    const double w = 2.0 * std::numbers::pi * freq / kFs;
    float previous = 0.0f;
    double maxStep = 0.0;
    for (int i = 0; i < 48000 * 3; ++i)
    {
        if (i == latency * 2)
            REQUIRE (designStandby (domain, { 600.0f, 3000.0f, 8000.0f }));
        beginBlock (domain);

        const float x = static_cast<float> (0.5 * std::sin (w * i));
        auto bands = split (domain, x, x);
        bands.l[1] *= 0.25f;
        bands.r[1] *= 0.25f;
        float outL, outR;
        merge (bands, outL, outR);
        if (i > latency * 2)
            maxStep = std::max (maxStep, static_cast<double> (std::abs (outL - previous)));
        previous = outL;
    }
    // A 0.5-amplitude sine moves at most 0.5 * w per sample; a click would far exceed it.
    INFO ("max step = " << maxStep << " sine bound = " << 0.5 * w);
    CHECK (maxStep < 0.5 * w * 1.05);
}

TEST_CASE ("designStandby refuses while a swap is in progress", "[dsp][band][fir][swap]")
{
    BandDomain domain;
    prepare (domain, SplitMode::Fir, kDefaultCrossovers, kFs, 256);
    REQUIRE (designStandby (domain, { 200.0f, 2000.0f, 8000.0f }));
    CHECK_FALSE (designStandby (domain, { 300.0f, 3000.0f, 9000.0f }));   // standby already Ready
    beginBlock (domain);
    CHECK (isSwapping (domain));
    CHECK_FALSE (designStandby (domain, { 300.0f, 3000.0f, 9000.0f }));   // warming / fading
}

TEST_CASE ("IIR mode applies crossover changes directly and never swaps", "[dsp][band][iir]")
{
    BandDomain domain;
    prepare (domain, SplitMode::Iir, kDefaultCrossovers, kFs, 256);
    CHECK_FALSE (designStandby (domain, { 200.0f, 2000.0f, 8000.0f }));   // FIR-only path
    setIirCrossovers (domain, { 200.0f, 2000.0f, 8000.0f });
    CHECK (activeCrossovers (domain).f1 == 200.0f);
    CHECK_FALSE (isSwapping (domain));
}

TEST_CASE ("concurrent redesigns from a background thread stay null and finite", "[dsp][band][fir][concurrency]")
{
    BandDomain domain;
    prepare (domain, SplitMode::Fir, kDefaultCrossovers, kFs, 256);
    const int latency = latencySamples (domain);
    auto ref = makeStereoDelay (latency);

    std::atomic<bool> stop { false };
    std::atomic<int> designs { 0 };
    std::thread designer ([&]
    {
        float f = 150.0f;
        while (! stop.load (std::memory_order_relaxed))
        {
            if (designStandby (domain, { f, f * 8.0f, f * 40.0f }))
            {
                designs.fetch_add (1, std::memory_order_relaxed);
                f = f > 400.0f ? 150.0f : f * 1.3f;
            }
            std::this_thread::yield();
        }
    });

    Noise noise;
    double maxErr = 0.0;
    for (int i = 0; i < 48000 * 6; ++i)
    {
        beginBlock (domain);
        const float l = noise.next(), r = noise.next();
        float outL, outR;
        merge (split (domain, l, r), outL, outR);
        REQUIRE (std::isfinite (outL));
        const float refL = process (ref.l, l), refR = process (ref.r, r);
        if (i >= latency)
            maxErr = std::max ({ maxErr, static_cast<double> (std::abs (outL - refL)), static_cast<double> (std::abs (outR - refR)) });
    }
    stop = true;
    designer.join();

    INFO ("completed designs = " << designs.load());
    CHECK (designs.load() >= 2);
    CHECK (maxErr < 2e-5);
}
