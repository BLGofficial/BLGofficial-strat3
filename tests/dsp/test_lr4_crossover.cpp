// Linkwitz-Riley (LR4) IIR crossover tests. The multiband, width and saturation layers all
// sit on this split: if the bands do not sum flat, every one of them colours the mix even at
// neutral settings.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

#include "dsp/crossover/CrossoverPoints.h"
#include "dsp/crossover/Lr4Crossover.h"
#include "dsp/core/SvfTpt.h"
#include "support/Analysis.h"

using namespace strat3::dsp;
using namespace strat3::test;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double kFs = 48000.0;
constexpr size_t kIrLength = 65536;   // long enough for the 20 Hz all-pass tail to decay

std::array<std::vector<float>, 4> bandIrs (Lr4Split4 split)
{
    std::array<std::vector<float>, 4> irs;
    for (auto& ir : irs)
        ir.resize (kIrLength);
    for (size_t n = 0; n < kIrLength; ++n)
    {
        const auto b = process (split, n == 0 ? 1.0f : 0.0f);
        for (size_t k = 0; k < 4; ++k)
            irs[k][n] = b[k];
    }
    return irs;
}

std::vector<float> sumOf (const std::array<std::vector<float>, 4>& irs)
{
    std::vector<float> sum (irs[0].size(), 0.0f);
    for (const auto& ir : irs)
        for (size_t n = 0; n < ir.size(); ++n)
            sum[n] += ir[n];
    return sum;
}

std::vector<double> logSweepHz (double lo, double hi, int points)
{
    std::vector<double> f;
    for (int i = 0; i < points; ++i)
        f.push_back (lo * std::pow (hi / lo, static_cast<double> (i) / (points - 1)));
    return f;
}

} // namespace

TEST_CASE ("all-pass SVF mode is flat in magnitude", "[dsp][svf][allpass]")
{
    SvfState st {};
    const auto c = makeSvfCoeffs (SvfType::AllPass, 1000.0f, 0.70710678f, 0.0f, kFs);
    const auto ir = impulseResponse ([&] (float x) { return processSample (st, c, x); }, kIrLength);
    for (const double f : logSweepHz (20.0, 20000.0, 40))
    {
        INFO ("f = " << f);
        CHECK_THAT (magnitudeDb (ir, f, kFs), WithinAbs (0.0, 0.005));
    }
}

TEST_CASE ("LR4 2-way: low + high equals a 2nd-order all-pass at fc", "[dsp][xover][lr4]")
{
    for (const float fc : { 40.0f, 100.0f, 2000.0f })
    {
        INFO ("fc = " << fc);
        auto split = makeLr4Split2 (fc, kFs);
        SvfState ap {};
        const auto apCoeffs = makeSvfCoeffs (SvfType::AllPass, fc, 0.70710678f, 0.0f, kFs);
        Noise noise;
        double maxErr = 0.0;
        for (int i = 0; i < 48000; ++i)
        {
            const float x = noise.next();
            const auto out = process (split, x);
            const float reference = processSample (ap, apCoeffs, x);
            maxErr = std::max (maxErr, static_cast<double> (std::abs ((out.low + out.high) - reference)));
        }
        // Two float structures computing the same transfer function differ by rounding only.
        // At 40 Hz (g ~ 0.0026) the states are large relative to the output, so allow
        // 5e-5 (-86 dBFS on unit noise); measured 1.4e-5. The magnitude tests prove exactness.
        CHECK (maxErr < 5e-5);
    }
}

TEST_CASE ("LR4 2-way: each band is -6.02 dB at fc and rolls off at 24 dB/oct", "[dsp][xover][lr4]")
{
    const float fc = 1000.0f;
    auto split = makeLr4Split2 (fc, kFs);
    std::vector<float> lowIr (kIrLength), highIr (kIrLength);
    for (size_t n = 0; n < kIrLength; ++n)
    {
        const auto out = process (split, n == 0 ? 1.0f : 0.0f);
        lowIr[n] = out.low;
        highIr[n] = out.high;
    }
    CHECK_THAT (magnitudeDb (lowIr,  1000.0, kFs), WithinAbs (-6.0206, 0.02));
    CHECK_THAT (magnitudeDb (highIr, 1000.0, kFs), WithinAbs (-6.0206, 0.02));
    CHECK (magnitudeDb (lowIr,  4000.0, kFs) < -40.0);   // 2 octaves above: ~-48 dB analog
    CHECK (magnitudeDb (highIr,  250.0, kFs) < -40.0);
    CHECK_THAT (magnitudeDb (lowIr,  50.0,    kFs), WithinAbs (0.0, 0.01));
    CHECK_THAT (magnitudeDb (highIr, 15000.0, kFs), WithinAbs (0.0, 0.05));
}

TEST_CASE ("LR4 4-band: bands sum flat across 20 Hz - 20 kHz (phase compensation)", "[dsp][xover][lr4][sum]")
{
    for (const double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        INFO ("fs = " << fs);
        const auto irs = bandIrs (makeLr4Split4 ({ 120.0f, 1000.0f, 5000.0f }, fs));
        const auto sum = sumOf (irs);
        for (const double f : logSweepHz (20.0, 20000.0, 60))
        {
            INFO ("f = " << f);
            CHECK_THAT (magnitudeDb (sum, f, fs), WithinAbs (0.0, 0.01));
        }
    }
}

TEST_CASE ("LR4 4-band: adjacent bands cross at -6.02 dB and pass their own centre", "[dsp][xover][lr4]")
{
    const CrossoverPoints p { 120.0f, 1000.0f, 5000.0f };
    const auto irs = bandIrs (makeLr4Split4 (p, kFs));

    CHECK_THAT (magnitudeDb (irs[0], p.f1, kFs), WithinAbs (-6.0206, 0.05));
    CHECK_THAT (magnitudeDb (irs[1], p.f1, kFs), WithinAbs (-6.0206, 0.05));
    CHECK_THAT (magnitudeDb (irs[1], p.f2, kFs), WithinAbs (-6.0206, 0.05));
    CHECK_THAT (magnitudeDb (irs[2], p.f2, kFs), WithinAbs (-6.0206, 0.05));
    CHECK_THAT (magnitudeDb (irs[2], p.f3, kFs), WithinAbs (-6.0206, 0.05));
    CHECK_THAT (magnitudeDb (irs[3], p.f3, kFs), WithinAbs (-6.0206, 0.05));

    CHECK (magnitudeDb (irs[0], 30.0, kFs) > -0.1);
    CHECK (magnitudeDb (irs[1], std::sqrt (120.0 * 1000.0), kFs) > -1.0);
    CHECK (magnitudeDb (irs[2], std::sqrt (1000.0 * 5000.0), kFs) > -1.0);
    CHECK (magnitudeDb (irs[3], 15000.0, kFs) > -0.1);
}

TEST_CASE ("crossover sanitiser: enforces order, spacing and Nyquist", "[dsp][xover][sanitise]")
{
    SECTION ("valid points untouched")
    {
        const auto p = sanitiseCrossovers ({ 120.0f, 1000.0f, 5000.0f }, kFs);
        CHECK (p.f1 == 120.0f);
        CHECK (p.f2 == 1000.0f);
        CHECK (p.f3 == 5000.0f);
    }
    SECTION ("reversed and colliding points become ordered with minimum spacing")
    {
        for (const CrossoverPoints in : { CrossoverPoints { 5000.0f, 1000.0f, 120.0f },
                                          CrossoverPoints { 500.0f, 500.0f, 500.0f },
                                          CrossoverPoints { 20.0f, 20.0f, 20.0f },     // all at the floor
                                          CrossoverPoints { 1.0f, 2.0f, 3.0f } })
        {
            const auto p = sanitiseCrossovers (in, kFs);
            CHECK (p.f2 >= p.f1 * kMinCrossoverRatio * 0.9999f);
            CHECK (p.f3 >= p.f2 * kMinCrossoverRatio * 0.9999f);
        }
    }
    SECTION ("points above Nyquist are pushed down, keeping spacing")
    {
        const auto p = sanitiseCrossovers ({ 15000.0f, 16000.0f, 16000.0f }, 22050.0);
        CHECK (p.f3 <= 0.45f * 22050.0f);
        CHECK (p.f2 <= p.f3 / kMinCrossoverRatio * 1.0001f);
        CHECK (p.f1 <= p.f2 / kMinCrossoverRatio * 1.0001f);
        CHECK (p.f1 >= 20.0f);
    }
    SECTION ("non-finite input falls back to defaults")
    {
        const auto p = sanitiseCrossovers ({ NAN, 1000.0f, INFINITY }, kFs);
        CHECK (p.f1 == kDefaultCrossovers.f1);
        CHECK (p.f2 == kDefaultCrossovers.f2);
        CHECK (p.f3 == kDefaultCrossovers.f3);
    }
}

TEST_CASE ("LR4 4-band: crossover sweeps while running stay finite and bounded", "[dsp][xover][lr4][stability]")
{
    for (const double fs : { 44100.0, 192000.0 })
    {
        auto split = makeLr4Split4 (kDefaultCrossovers, fs);
        Noise noise, mod;
        float peak = 0.0f;
        for (int i = 0; i < static_cast<int> (fs) * 2; ++i)
        {
            if (i % 32 == 0)
                setCrossovers (split, { 40.0f * std::pow (12.0f, 0.5f + 0.5f * mod.next()),
                                        200.0f * std::pow (25.0f, 0.5f + 0.5f * mod.next()),
                                        1000.0f * std::pow (16.0f, 0.5f + 0.5f * mod.next()) });
            const auto b = process (split, 0.5f * noise.next());
            for (const float v : b)
            {
                REQUIRE (std::isfinite (v));
                peak = std::max (peak, std::abs (v));
            }
        }
        INFO ("fs = " << fs << " peak = " << peak);
        CHECK (peak < 4.0f);
    }
}

TEST_CASE ("LR4 4-band: NaN recovery and denormal flush", "[dsp][xover][lr4][nan]")
{
    auto split = makeLr4Split4 (kDefaultCrossovers, kFs);
    process (split, NAN);
    CHECK (resetIfNotFinite (split));
    for (const float v : process (split, 0.5f))
        CHECK (std::isfinite (v));
    CHECK_FALSE (resetIfNotFinite (split));

    process (split, 1.0f);
    bool sawSubnormal = false;
    for (int block = 0; block < 48000 * 10 / 256; ++block)
    {
        for (int i = 0; i < 256; ++i)
            for (const float v : process (split, 0.0f))
                sawSubnormal |= isSubnormal (v);
        flushDenormals (split);
    }
    CHECK_FALSE (sawSubnormal);
}
