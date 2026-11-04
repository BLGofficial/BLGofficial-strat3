// Linear-phase FIR crossover (MASTER mode). Contract: the four bands sum to the input
// delayed by the reported latency, to float precision, so a neutral band domain nulls.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

#include "dsp/crossover/FirCrossover.h"
#include "dsp/crossover/FirDesign.h"
#include "support/Analysis.h"

using namespace strat3::dsp;
using namespace strat3::test;
using Catch::Matchers::WithinAbs;

TEST_CASE ("FIR low-pass design: symmetric, unity DC, -6 dB at fc, deep stopband", "[dsp][fir][design]")
{
    const double fs = 48000.0;
    const size_t taps = firCrossoverTaps (fs);
    REQUIRE (taps % 2 == 1);   // odd length: integer group delay, so bands align to the sample

    for (const float fc : { 120.0f, 1000.0f, 5000.0f })
    {
        INFO ("fc = " << fc);
        const auto h = designLinearPhaseLowpass (fc, fs, taps);
        REQUIRE (h.size() == taps);
        for (size_t n = 0; n < taps / 2; ++n)
            REQUIRE (h[n] == h[taps - 1 - n]);

        double dc = 0.0;
        for (const float v : h)
            dc += static_cast<double> (v);
        const double f = fc;
        CHECK_THAT (dc, WithinAbs (1.0, 1e-6));
        CHECK_THAT (magnitudeDb (h, f, fs), WithinAbs (-6.0206, 0.1));
        CHECK (magnitudeDb (h, f * 4.0, fs) < -60.0);
    }
}

TEST_CASE ("FIR tap count scales with sample rate (constant latency in ms)", "[dsp][fir][design]")
{
    const double ms48  = static_cast<double> (firCrossoverTaps (48000.0) / 2) / 48.0;
    const double ms192 = static_cast<double> (firCrossoverTaps (192000.0) / 2) / 192.0;
    const double ms441 = static_cast<double> (firCrossoverTaps (44100.0) / 2) / 44.1;
    CHECK_THAT (ms192, WithinAbs (ms48, 0.05));
    CHECK_THAT (ms441, WithinAbs (ms48, 0.05));
}

TEST_CASE ("FIR 4-band: bands sum to the input delayed by the reported latency (null)", "[dsp][fir][null]")
{
    for (const double fs : { 44100.0, 48000.0, 96000.0 })
    {
        INFO ("fs = " << fs);
        FirSplit4 split;
        prepare (split, { 120.0f, 1000.0f, 5000.0f }, fs, 256);
        const int latency = latencySamples (split);
        CHECK (latency == static_cast<int> (firCrossoverTaps (fs) / 2) + 256);

        Noise noise;
        std::vector<float> x (static_cast<size_t> (latency) + 20000);
        for (auto& v : x)
            v = noise.next();

        double maxErr = 0.0;
        for (size_t n = 0; n < x.size(); ++n)
        {
            const auto b = process (split, x[n]);
            const float sum = b[0] + b[1] + b[2] + b[3];
            if (n >= static_cast<size_t> (latency))
                maxErr = std::max (maxErr, static_cast<double> (std::abs (sum - x[n - static_cast<size_t> (latency)])));
            else
                REQUIRE (std::abs (sum) < 1e-6f);
        }
        INFO ("max null error = " << maxErr);
        CHECK (maxErr < 2e-5);   // about -94 dBFS
    }
}

TEST_CASE ("FIR 4-band: each band passes its centre and crosses at -6 dB", "[dsp][fir][response]")
{
    const double fs = 48000.0;
    FirSplit4 split;
    prepare (split, { 120.0f, 1000.0f, 5000.0f }, fs, 256);

    const size_t length = static_cast<size_t> (latencySamples (split)) * 2 + 4096;
    std::array<std::vector<float>, 4> irs;
    for (auto& ir : irs)
        ir.resize (length);
    for (size_t n = 0; n < length; ++n)
    {
        const auto b = process (split, n == 0 ? 1.0f : 0.0f);
        for (size_t k = 0; k < 4; ++k)
            irs[k][n] = b[k];
    }

    CHECK (magnitudeDb (irs[0], 40.0, fs) > -0.1);
    CHECK (magnitudeDb (irs[1], std::sqrt (120.0 * 1000.0), fs) > -0.5);
    CHECK (magnitudeDb (irs[2], std::sqrt (1000.0 * 5000.0), fs) > -0.5);
    CHECK (magnitudeDb (irs[3], 15000.0, fs) > -0.1);
    CHECK_THAT (magnitudeDb (irs[1], 1000.0, fs), WithinAbs (-6.0206, 0.15));
    CHECK_THAT (magnitudeDb (irs[2], 1000.0, fs), WithinAbs (-6.0206, 0.15));
}

TEST_CASE ("FIR 4-band: unprepared split passes audio through on the sum", "[dsp][fir][extreme]")
{
    FirSplit4 split;
    const auto b = process (split, 0.75f);
    CHECK (b[0] + b[1] + b[2] + b[3] == 0.75f);
    CHECK (latencySamples (split) == 0);
}
