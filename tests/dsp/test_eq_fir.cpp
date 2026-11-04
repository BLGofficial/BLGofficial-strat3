// FIR EQ (MASTER mode): eq.phase blend between minimum phase (0) and linear phase (100),
// and runtime redesign through the shared FirSwap handover.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <atomic>
#include <cmath>
#include <numbers>
#include <thread>

#include "dsp/eq/EqFir.h"
#include "dsp/eq/EqLayer.h"
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

EqSettings bellAt (float hz, float gainDb)
{
    EqSettings s;
    s[0].on = true; s[0].type = EqType::Bell; s[0].freq = hz; s[0].gainDb = gainDb; s[0].q = 1.0f;
    return s;
}

double energy (const std::vector<float>& h, size_t from, size_t to)
{
    double e = 0.0;
    for (size_t n = from; n < to; ++n)
        e += static_cast<double> (h[n]) * static_cast<double> (h[n]);
    return e;
}

/** Mono impulse through the IIR EQ: the left output is the stereo + mid band response. */
std::vector<float> iirMidImpulse (const EqSettings& s, size_t length, size_t delay)
{
    EqLayer eq;
    prepare (eq, kFs);
    setBands (eq, s);
    snapToTargets (eq);
    std::vector<float> ir (length + delay, 0.0f);
    for (size_t n = 0; n < length; ++n)
    {
        float l = n == 0 ? 1.0f : 0.0f, r = l;
        process (eq, l, r);
        ir[n + delay] = l;
    }
    return ir;
}

} // namespace

TEST_CASE ("eq.phase 100 gives exactly the linear-phase kernel", "[dsp][eq][fir][phase]")
{
    const auto taps = firCrossoverTaps (kFs);
    CHECK (designEqKernel (mixedSettings(), kFs, taps, EqChannel::Mid, kLinearPhasePercent)
           == designLinearPhaseEq (mixedSettings(), kFs, taps, EqChannel::Mid));
}

TEST_CASE ("eq.phase 0 matches the IIR EQ (minimum phase) delayed by the FIR latency", "[dsp][eq][fir][phase][response]")
{
    // The SVF bells/shelves are minimum phase, so the cepstral design must reproduce the IIR
    // EQ's complex response, not just its magnitude.
    const auto s = mixedSettings();
    const auto taps = firCrossoverTaps (kFs);
    const auto h = designEqKernel (s, kFs, taps, EqChannel::Mid, kMinimumPhasePercent);
    const auto iir = iirMidImpulse (s, 32768, taps / 2);

    double worst = 0.0;
    for (double f = 200.0; f <= 16000.0; f *= 1.12)
    {
        const auto a = dtft (h, f, kFs), b = dtft (iir, f, kFs);
        const double rel = std::abs (a - b) / std::abs (b);
        worst = std::max (worst, rel);
        INFO ("f = " << f << " rel err = " << rel);
        CHECK (rel < 0.02);   // 0.02 = ~0.17 dB / ~1.1 degrees
    }
    INFO ("worst relative error = " << worst);
    SUCCEED();
}

TEST_CASE ("eq.phase 0 has no pre-ringing; the blend moves energy smoothly", "[dsp][eq][fir][phase]")
{
    const auto taps = firCrossoverTaps (kFs);
    const size_t centre = taps / 2;
    auto preRatio = [&] (float phase)
    {
        const auto h = designEqKernel (mixedSettings(), kFs, taps, EqChannel::Mid, phase);
        return energy (h, 0, centre) / energy (h, 0, taps);
    };
    const double minPre = preRatio (kMinimumPhasePercent);
    const double midPre = preRatio (50.0f);
    const double linPre = preRatio (kLinearPhasePercent);
    INFO ("pre-ring energy ratio: min " << minPre << " blend " << midPre << " lin " << linPre);
    CHECK (minPre < 1e-6);
    CHECK (linPre > 1e-3);   // a symmetric kernel rings before its peak
    CHECK (midPre > minPre);
    CHECK (midPre < linPre);
}

TEST_CASE ("magnitude does not depend on the phase blend", "[dsp][eq][fir][phase][response]")
{
    const auto s = mixedSettings();
    const auto taps = firCrossoverTaps (kFs);
    for (const float phase : { 0.0f, 25.0f, 50.0f, 75.0f })
        for (const auto ch : { EqChannel::Mid, EqChannel::Side })
        {
            const auto h = designEqKernel (s, kFs, taps, ch, phase);
            for (double f = 200.0; f <= 16000.0; f *= 1.12)
            {
                INFO ("phase = " << phase << " channel = " << static_cast<int> (ch) << " f = " << f);
                CHECK_THAT (magnitudeDb (h, f, kFs), WithinAbs (settingsMagnitudeDb (s, f, kFs, ch), 0.1));
            }
        }
}

TEST_CASE ("neutral settings are a pure delay at every phase blend", "[dsp][eq][fir][null]")
{
    for (const float phase : { 0.0f, 50.0f, 100.0f })
    {
        FirEq eq;
        prepare (eq, EqSettings {}, phase, kFs, 256);
        const int latency = latencySamples (eq);
        Noise noise;
        std::vector<float> xl (static_cast<size_t> (latency) + 8000), xr (xl.size());
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
        INFO ("phase = " << phase);
        CHECK (maxErr < 1e-6f);
    }
}

TEST_CASE ("latency is the same at every phase blend", "[dsp][eq][fir]")
{
    // Constant latency lets eq.phase be automated without a host latency change.
    const int expected = static_cast<int> (firCrossoverTaps (kFs) / 2) + 256;
    for (const float phase : { 0.0f, 33.0f, 100.0f })
    {
        FirEq eq;
        prepare (eq, mixedSettings(), phase, kFs, 256);
        CHECK (latencySamples (eq) == expected);
    }
}

TEST_CASE ("FIR EQ design stays finite at parameter extremes", "[dsp][eq][fir][stability]")
{
    EqSettings s;
    s[0].on = true; s[0].type = EqType::HighPass;     s[0].freq = 20.0f;    s[0].q = 18.0f;
    s[1].on = true; s[1].type = EqType::Bell;         s[1].freq = 20000.0f; s[1].gainDb = 18.0f;  s[1].q = 18.0f;
    s[2].on = true; s[2].type = EqType::LowPass;      s[2].freq = 20000.0f; s[2].q = 0.1f;
    s[3].on = true; s[3].type = EqType::LowShelf;     s[3].freq = 20.0f;    s[3].gainDb = -18.0f; s[3].q = 0.1f;
    s[4].on = true; s[4].type = EqType::PassiveShelf; s[4].freq = 30.0f;    s[4].gainDb = 18.0f;  s[4].q = 1.0f;
    s[5].on = true; s[5].type = EqType::Bell;         s[5].freq = 1000.0f;  s[5].gainDb = -18.0f; s[5].q = 0.1f;

    const auto taps = firCrossoverTaps (kFs);
    for (const float phase : { 0.0f, 37.0f, 100.0f })
    {
        const auto h = designEqKernel (s, kFs, taps, EqChannel::Mid, phase);
        bool finite = true;
        for (const float v : h)
            finite &= std::isfinite (v);
        INFO ("phase = " << phase);
        REQUIRE (finite);
        CHECK_THAT (magnitudeDb (h, 1000.0, kFs), WithinAbs (settingsMagnitudeDb (s, 1000.0, kFs, EqChannel::Mid), 0.5));
    }
}

TEST_CASE ("unprepared FIR EQ is a passthrough", "[dsp][eq][fir]")
{
    FirEq eq;
    float l = 0.3f, r = -0.7f;
    process (eq, l, r);
    CHECK (l == 0.3f);
    CHECK (r == -0.7f);
    CHECK (latencySamples (eq) == 0);
    CHECK_FALSE (designStandby (eq, mixedSettings(), 0.0f));   // nothing to swap into
}

// ------------------------------------------------------------------- runtime redesign

TEST_CASE ("runtime redesign converges to a fresh design of the new settings", "[dsp][eq][fir][swap]")
{
    FirEq live, fresh;
    prepare (live, EqSettings {}, kLinearPhasePercent, kFs, 256);
    prepare (fresh, mixedSettings(), 30.0f, kFs, 256);

    const int changeAt = 5000;
    Noise noise;
    float maxErr = 0.0f;
    bool sawSwap = false;
    int swapEnd = -1;
    for (int i = 0; i < 48000 * 2; ++i)
    {
        if (i == changeAt)
            REQUIRE (designStandby (live, mixedSettings(), 30.0f));
        beginBlock (live);
        beginBlock (fresh);
        const bool swapping = isSwapping (live);
        sawSwap |= swapping;
        if (sawSwap && ! swapping && swapEnd < 0)
            swapEnd = i;

        const float xl = noise.next(), xr = noise.next();
        float al = xl, ar = xr, bl = xl, br = xr;
        process (live, al, ar);
        process (fresh, bl, br);
        if (swapEnd >= 0)
            maxErr = std::max ({ maxErr, std::abs (al - bl), std::abs (ar - br) });
    }
    REQUIRE (sawSwap);
    REQUIRE (swapEnd > 0);
    CHECK (activeDesign (live).phasePercent == 30.0f);
    CHECK (activeDesign (live).settings == mixedSettings());
    CHECK (maxErr < 1e-5f);   // warm-up too short would leave the new slot's history incomplete
}

TEST_CASE ("swap between equivalent designs keeps the null through warm-up and fade", "[dsp][eq][fir][swap][null]")
{
    // Neutral is a pure delay at every phase blend, so neutral/linear -> neutral/minimum must
    // not change the output at any point. A warm-up shorter than the kernel + partition
    // would fade in a slot whose history is still incomplete.
    FirEq eq;
    prepare (eq, EqSettings {}, kLinearPhasePercent, kFs, 256);
    const int latency = latencySamples (eq);
    Noise noise;
    std::vector<float> x (48000);
    for (auto& v : x) v = noise.next();

    float maxErr = 0.0f;
    bool sawSwap = false;
    for (size_t i = 0; i < x.size(); ++i)
    {
        if (i == static_cast<size_t> (latency) + 1000)
            REQUIRE (designStandby (eq, EqSettings {}, kMinimumPhasePercent));
        beginBlock (eq);
        sawSwap |= isSwapping (eq);
        float l = x[i], r = -x[i];
        process (eq, l, r);
        if (i >= static_cast<size_t> (latency))
            maxErr = std::max (maxErr, std::abs (l - x[i - static_cast<size_t> (latency)]));
    }
    CHECK (sawSwap);
    CHECK_FALSE (isSwapping (eq));
    CHECK (maxErr < 1e-6f);
}

TEST_CASE ("runtime redesign crossfade is click-free", "[dsp][eq][fir][swap]")
{
    FirEq eq;
    prepare (eq, EqSettings {}, kLinearPhasePercent, kFs, 256);
    const int latency = latencySamples (eq);

    const double freq = 1000.0;
    const double w = 2.0 * std::numbers::pi * freq / kFs;
    float previous = 0.0f;
    double maxStep = 0.0;
    for (int i = 0; i < 48000 * 2; ++i)
    {
        if (i == latency * 2)
            REQUIRE (designStandby (eq, bellAt (1000.0f, 12.0f), 50.0f));   // 0.25 -> 1.0 amplitude
        beginBlock (eq);
        float l = static_cast<float> (0.25 * std::sin (w * i)), r = l;
        process (eq, l, r);
        if (i > latency * 2)
            maxStep = std::max (maxStep, static_cast<double> (std::abs (l - previous)));
        previous = l;
    }
    // Output never exceeds amplitude 1.0, so a sine moves at most w per sample.
    INFO ("max step = " << maxStep << " bound = " << w);
    CHECK (maxStep < w * 1.05);
}

TEST_CASE ("FIR EQ designStandby refuses while a swap is pending", "[dsp][eq][fir][swap]")
{
    FirEq eq;
    prepare (eq, EqSettings {}, kLinearPhasePercent, kFs, 256);
    REQUIRE (designStandby (eq, mixedSettings(), 0.0f));
    CHECK_FALSE (designStandby (eq, bellAt (500.0f, 3.0f), 0.0f));
    beginBlock (eq);
    CHECK (isSwapping (eq));
    CHECK_FALSE (designStandby (eq, bellAt (500.0f, 3.0f), 0.0f));
}

TEST_CASE ("FIR EQ: concurrent redesigns from a background thread stay finite", "[dsp][eq][fir][concurrency]")
{
    FirEq eq;
    prepare (eq, EqSettings {}, kLinearPhasePercent, kFs, 256);

    std::atomic<bool> stop { false };
    std::atomic<int> designs { 0 };
    std::thread designer ([&]
    {
        float phase = 0.0f;
        while (! stop.load (std::memory_order_relaxed))
        {
            if (designStandby (eq, bellAt (1000.0f, phase > 50.0f ? 6.0f : -6.0f), phase))
            {
                designs.fetch_add (1, std::memory_order_relaxed);
                phase = phase > 90.0f ? 0.0f : phase + 25.0f;
            }
            std::this_thread::yield();
        }
    });

    Noise noise;
    bool finite = true;
    for (int i = 0; i < 48000 * 4; ++i)
    {
        beginBlock (eq);
        float l = noise.next(), r = noise.next();
        process (eq, l, r);
        finite &= std::isfinite (l) && std::isfinite (r);
    }
    stop = true;
    designer.join();

    INFO ("completed designs = " << designs.load());
    CHECK (designs.load() >= 2);
    CHECK (finite);
}
