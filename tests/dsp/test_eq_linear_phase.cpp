// Linear-phase EQ (MASTER mode, eq.phase = 100): FIR designed from the same band magnitudes
// the IIR EQ uses. Phase blend and runtime redesign are in test_eq_fir.cpp.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

#include "dsp/eq/EqFir.h"
#include "dsp/eq/EqResponse.h"
#include "support/Analysis.h"

using namespace strat3::dsp;
using namespace strat3::test;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double kFs = 48000.0;

EqSettings mixedSettings()
{
    EqSettings s;
    s[0].on = true; s[0].type = EqType::Bell;      s[0].freq = 300.0f;  s[0].gainDb = 4.0f;  s[0].q = 1.0f;
    s[1].on = true; s[1].type = EqType::Bell;      s[1].freq = 2000.0f; s[1].gainDb = -5.0f; s[1].q = 2.0f;
    s[2].on = true; s[2].type = EqType::HighShelf; s[2].freq = 6000.0f; s[2].gainDb = -3.0f; s[2].q = 0.707f;
    s[3].on = true; s[3].type = EqType::Bell;      s[3].freq = 800.0f;  s[3].gainDb = 6.0f;  s[3].q = 1.0f;
    s[3].channel = EqChannel::Side;
    return s;
}

} // namespace

TEST_CASE ("linear-phase EQ kernel is symmetric with the expected length", "[dsp][eq][lp]")
{
    const auto taps = firCrossoverTaps (kFs);
    const auto h = designLinearPhaseEq (mixedSettings(), kFs, taps, EqChannel::Mid);
    REQUIRE (h.size() == taps);
    for (size_t n = 0; n < taps / 2; ++n)
        REQUIRE (h[n] == h[taps - 1 - n]);
}

TEST_CASE ("linear-phase EQ: neutral settings give a pure delay", "[dsp][eq][lp][null]")
{
    FirEq eq;
    prepare (eq, EqSettings {}, kLinearPhasePercent, kFs, 256);
    const int latency = latencySamples (eq);
    CHECK (latency == static_cast<int> (firCrossoverTaps (kFs) / 2) + 256);

    Noise noise;
    std::vector<float> xl (static_cast<size_t> (latency) + 10000), xr (xl.size());
    for (size_t i = 0; i < xl.size(); ++i) { xl[i] = noise.next(); xr[i] = noise.next(); }

    float maxErr = 0.0f;
    for (size_t i = 0; i < xl.size(); ++i)
    {
        float l = xl[i], r = xr[i];
        process (eq, l, r);
        if (i >= static_cast<size_t> (latency))
            maxErr = std::max ({ maxErr, std::abs (l - xl[i - static_cast<size_t> (latency)]),
                                         std::abs (r - xr[i - static_cast<size_t> (latency)]) });
    }
    CHECK (maxErr < 1e-6f);
}

TEST_CASE ("linear-phase EQ magnitude matches the IIR design (200 Hz - 16 kHz)", "[dsp][eq][lp][response]")
{
    const auto s = mixedSettings();
    const auto taps = firCrossoverTaps (kFs);
    for (const auto ch : { EqChannel::Mid, EqChannel::Side })
    {
        const auto h = designLinearPhaseEq (s, kFs, taps, ch);
        for (double f = 200.0; f <= 16000.0; f *= 1.12)
        {
            INFO ("channel = " << static_cast<int> (ch) << " f = " << f);
            CHECK_THAT (magnitudeDb (h, f, kFs), WithinAbs (settingsMagnitudeDb (s, f, kFs, ch), 0.1));
        }
    }
}

TEST_CASE ("linear-phase EQ excludes dynamic bands (they stay IIR in MASTER)", "[dsp][eq][lp]")
{
    EqSettings s;
    s[0].on = true; s[0].type = EqType::Bell; s[0].freq = 1000.0f; s[0].gainDb = 6.0f; s[0].q = 1.0f;
    s[0].dyn = EqDynMode::Down;
    const auto h = designLinearPhaseEq (s, kFs, firCrossoverTaps (kFs), EqChannel::Mid);
    CHECK_THAT (magnitudeDb (h, 1000.0, kFs), WithinAbs (0.0, 0.01));
}

TEST_CASE ("linear-phase EQ: mono input is unaffected by a side-only band", "[dsp][eq][lp][ms]")
{
    EqSettings s;
    s[0].on = true; s[0].type = EqType::Bell; s[0].freq = 1000.0f; s[0].gainDb = -12.0f; s[0].q = 1.0f;
    s[0].channel = EqChannel::Side;
    FirEq eq;
    prepare (eq, s, kLinearPhasePercent, kFs, 256);
    const int latency = latencySamples (eq);
    Noise noise;
    std::vector<float> x (static_cast<size_t> (latency) + 8000);
    for (auto& v : x) v = noise.next();
    float maxErr = 0.0f;
    for (size_t i = 0; i < x.size(); ++i)
    {
        float l = x[i], r = x[i];
        process (eq, l, r);
        if (i >= static_cast<size_t> (latency))
            maxErr = std::max (maxErr, std::abs (l - x[i - static_cast<size_t> (latency)]));
    }
    CHECK (maxErr < 1e-6f);
}
