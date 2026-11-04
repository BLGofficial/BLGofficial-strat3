// TPT state-variable filter tests (Simper/Zavalishin form).
// Covers: null at neutral, analytic response at fc, far-band behaviour, stability under
// per-sample modulation, extreme params, NaN/Inf recovery and denormal decay.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

#include "dsp/core/SvfTpt.h"
#include "support/Analysis.h"

using namespace strat3::dsp;
using namespace strat3::test;
using Catch::Matchers::WithinAbs;

namespace {

std::vector<float> irOf (const SvfCoeffs& c, size_t length = 32768)
{
    SvfState st {};
    return impulseResponse ([&] (float x) { return processSample (st, c, x); }, length);
}

constexpr double kFs = 48000.0;

} // namespace

TEST_CASE ("svf: 0 dB bell and 0 dB shelves are bit-exact passthrough", "[dsp][svf][null]")
{
    for (const auto type : { SvfType::Bell, SvfType::LowShelf, SvfType::HighShelf })
    {
        const auto c = makeSvfCoeffs (type, 1000.0f, 0.707f, 0.0f, kFs);
        SvfState st {};
        Noise noise;
        for (int i = 0; i < 48000; ++i)
        {
            const float x = noise.next();
            REQUIRE (processSample (st, c, x) == x);
        }
    }
}

TEST_CASE ("svf: low/high-pass are -3.01 dB at fc for Q = 1/sqrt(2)", "[dsp][svf][response]")
{
    for (const float fc : { 50.0f, 1000.0f, 10000.0f })
    {
        INFO ("fc = " << fc);
        const auto lp = irOf (makeSvfCoeffs (SvfType::LowPass,  fc, 0.70710678f, 0.0f, kFs));
        const auto hp = irOf (makeSvfCoeffs (SvfType::HighPass, fc, 0.70710678f, 0.0f, kFs));
        CHECK_THAT (magnitudeDb (lp, fc, kFs), WithinAbs (-3.0103, 0.02));
        CHECK_THAT (magnitudeDb (hp, fc, kFs), WithinAbs (-3.0103, 0.02));
        const double f = fc;
        CHECK_THAT (magnitudeDb (lp, f / 20.0, kFs), WithinAbs (0.0, 0.05));   // passband flat
        CHECK (magnitudeDb (lp, f * 8.0 < kFs / 2 ? f * 8.0 : kFs / 2 - 1, kFs) < -30.0);
    }
}

TEST_CASE ("svf: bell hits exactly its gain at fc and returns to 0 dB far away", "[dsp][svf][response]")
{
    for (const float gain : { -18.0f, -6.0f, 3.0f, 18.0f })
    {
        INFO ("gain = " << gain);
        const auto ir = irOf (makeSvfCoeffs (SvfType::Bell, 1000.0f, 1.0f, gain, kFs));
        CHECK_THAT (magnitudeDb (ir, 1000.0, kFs), WithinAbs (gain, 0.02));
        CHECK_THAT (magnitudeDb (ir, 20.0, kFs), WithinAbs (0.0, 0.1));
        CHECK_THAT (magnitudeDb (ir, 20000.0, kFs), WithinAbs (0.0, 0.2));
    }
}

TEST_CASE ("svf: shelves are half their gain at fc and full gain in the shelf band", "[dsp][svf][response]")
{
    const auto low  = irOf (makeSvfCoeffs (SvfType::LowShelf,  200.0f,  0.707f, 6.0f, kFs));
    const auto high = irOf (makeSvfCoeffs (SvfType::HighShelf, 5000.0f, 0.707f, -6.0f, kFs));

    CHECK_THAT (magnitudeDb (low, 200.0, kFs),   WithinAbs (3.0, 0.05));
    CHECK_THAT (magnitudeDb (low, 20.0, kFs),    WithinAbs (6.0, 0.1));
    CHECK_THAT (magnitudeDb (low, 10000.0, kFs), WithinAbs (0.0, 0.1));

    CHECK_THAT (magnitudeDb (high, 5000.0, kFs),  WithinAbs (-3.0, 0.05));
    CHECK_THAT (magnitudeDb (high, 20000.0, kFs), WithinAbs (-6.0, 0.2));
    CHECK_THAT (magnitudeDb (high, 100.0, kFs),   WithinAbs (0.0, 0.1));
}

TEST_CASE ("svf: unity-peak band-pass is 0 dB at fc (detector use)", "[dsp][svf][response]")
{
    for (const float q : { 0.5f, 2.0f, 10.0f })
    {
        INFO ("q = " << q);
        const auto ir = irOf (makeSvfCoeffs (SvfType::BandPass, 2000.0f, q, 0.0f, kFs));
        CHECK_THAT (magnitudeDb (ir, 2000.0, kFs), WithinAbs (0.0, 0.02));
    }
}

TEST_CASE ("svf: cutoff at or above Nyquist is clamped, output stays finite", "[dsp][svf][extreme]")
{
    for (const double fs : { 44100.0, 192000.0 })
    {
        for (const float fc : { 0.0f, -5.0f, static_cast<float> (fs / 2), static_cast<float> (fs) })
        {
            INFO ("fs = " << fs << " fc = " << fc);
            const auto c = makeSvfCoeffs (SvfType::Bell, fc, 0.0f, 18.0f, fs);   // Q = 0 is also invalid
            CHECK (std::isfinite (c.g));
            CHECK (std::isfinite (c.k));
            SvfState st {};
            Noise noise;
            for (int i = 0; i < 10000; ++i)
                REQUIRE (std::isfinite (processSample (st, c, noise.next())));
        }
    }
}

TEST_CASE ("svf: stable under per-sample random modulation at extreme settings", "[dsp][svf][stability]")
{
    // Stability = no growth over time, not a fixed output ceiling: gliding between filter
    // types at extreme Q/gain legitimately produces large transients (measured ~85 at
    // 44.1 kHz), but a truly unstable filter grows without bound or goes non-finite.
    for (const double fs : { 44100.0, 192000.0 })
    {
        ModulatedSvf filter = makeModulatedSvf (fs, 5.0f, makeSvfCoeffs (SvfType::Bell, 1000.0f, 0.707f, 0.0f, fs));
        Noise noise, mod;
        const int secondLength = static_cast<int> (fs);
        float firstSecondPeak = 0.0f, lastSecondPeak = 0.0f, statePeak = 0.0f;

        for (int i = 0; i < secondLength * 10; ++i)
        {
            // Jump the target every 64 samples across the full range: worst case for automation.
            if (i % 64 == 0)
            {
                const float fc   = 20.0f * std::pow (1000.0f, 0.5f + 0.5f * mod.next());
                const float q    = 0.1f * std::pow (180.0f, 0.5f + 0.5f * mod.next());
                const float gain = 18.0f * mod.next();
                const auto  type = static_cast<SvfType> ((i / 64) % 5);
                setTarget (filter, makeSvfCoeffs (type, fc, q, gain, fs));
            }
            const float y = std::abs (process (filter, 0.25f * noise.next()));
            REQUIRE (std::isfinite (y));
            if (i < secondLength)
                firstSecondPeak = std::max (firstSecondPeak, y);
            if (i >= secondLength * 9)
                lastSecondPeak = std::max (lastSecondPeak, y);
            statePeak = std::max ({ statePeak, std::abs (filter.state.ic1eq), std::abs (filter.state.ic2eq) });
        }
        INFO ("fs = " << fs << " first = " << firstSecondPeak << " last = " << lastSecondPeak << " state = " << statePeak);
        CHECK (lastSecondPeak < 2.0f * firstSecondPeak);
        CHECK (statePeak < 100.0f);
    }
}

TEST_CASE ("svf: NaN input poisons state; resetIfNotFinite recovers it", "[dsp][svf][nan]")
{
    const auto c = makeSvfCoeffs (SvfType::LowPass, 500.0f, 0.707f, 0.0f, kFs);
    SvfState st {};
    processSample (st, c, std::nanf (""));
    CHECK_FALSE (isFinite (st));

    CHECK (resetIfNotFinite (st));          // reports that it had to reset
    CHECK (isFinite (st));
    CHECK (std::isfinite (processSample (st, c, 1.0f)));
    CHECK_FALSE (resetIfNotFinite (st));    // healthy state is left alone
}

TEST_CASE ("svf: impulse decay with per-block flush never leaves subnormal state", "[dsp][svf][denormal]")
{
    const auto c = makeSvfCoeffs (SvfType::LowPass, 40.0f, 0.707f, 0.0f, kFs);
    SvfState st {};
    bool sawSubnormal = false;

    for (int block = 0; block < 48000 * 10 / 256; ++block)
    {
        for (int i = 0; i < 256; ++i)
            processSample (st, c, (block == 0 && i == 0) ? 1.0f : 0.0f);
        flushDenormals (st);
        sawSubnormal |= isSubnormal (st.ic1eq) || isSubnormal (st.ic2eq);
    }
    CHECK_FALSE (sawSubnormal);
}

TEST_CASE ("modulated svf: settles on the target coefficients", "[dsp][svf][smoothing]")
{
    const auto from = makeSvfCoeffs (SvfType::Bell, 200.0f, 1.0f, 0.0f, kFs);
    const auto to   = makeSvfCoeffs (SvfType::Bell, 4000.0f, 2.0f, 9.0f, kFs);
    ModulatedSvf f  = makeModulatedSvf (kFs, 10.0f, from);
    setTarget (f, to);
    for (int i = 0; i < 48000; ++i)   // 100 time constants
        process (f, 0.0f);

    CHECK_THAT (f.current.g,  WithinAbs (to.g,  1e-5));
    CHECK_THAT (f.current.k,  WithinAbs (to.k,  1e-5));
    CHECK_THAT (f.current.m1, WithinAbs (to.m1, 1e-5));
}
