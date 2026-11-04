// Per-sample polyphase oversampler (2x / 4x, cascaded halfband stages).
//   IIR (TRACK/MIX): halfband built from two all-pass branches (elliptic design), near-zero latency.
//   FIR (MASTER):    linear-phase Kaiser halfband, integer latency.
// Requirements, at the worst-case base rate of 44.1 kHz:
//   images of a 20 kHz tone and aliases folding onto 20 kHz are >= 100 dB down,
//   the up -> down composite is flat to 0.01 dB from 20 Hz to 20 kHz.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>
#include <numbers>
#include <vector>

#include "dsp/core/Oversampler.h"
#include "support/Analysis.h"

using namespace strat3::dsp;
using namespace strat3::test;

namespace {

constexpr double kFs = 44100.0;
constexpr double kTwoPi = 2.0 * std::numbers::pi;

/** Amplitude of the component at `hz` over a coherent window (integer cycles: no leakage). */
double toneAmp (const std::vector<float>& x, size_t from, size_t count, double hz, double fs)
{
    std::complex<double> sum { 0.0, 0.0 };
    const double w = kTwoPi * hz / fs;
    for (size_t n = 0; n < count; ++n)
        sum += static_cast<double> (x[from + n]) * std::polar (1.0, -w * static_cast<double> (n));
    return 2.0 * std::abs (sum) / static_cast<double> (count);
}

double db (double ratio) { return 20.0 * std::log10 (std::max (ratio, 1e-30)); }

/** Composite (up then down) impulse response at the base rate. */
std::vector<float> compositeIr (const OsConfig& cfg, size_t length)
{
    UpChain up;
    DownChain down;
    reset (up);
    reset (down);
    float os[4] {};
    return impulseResponse ([&] (float x) {
        upsample (cfg, up, x, os);
        return downsample (cfg, down, os);
    }, length);
}

} // namespace

TEST_CASE ("oversampler 1x is a bit-exact passthrough with zero latency", "[dsp][os]")
{
    const auto kind = GENERATE (OsKind::Iir, OsKind::Fir);
    const OsConfig cfg = makeOsConfig (1, kind, kFs);
    CHECK (cfg.factor == 1);
    CHECK (latencySamples (cfg) == 0);

    UpChain up;
    DownChain down;
    reset (up);
    reset (down);
    WhiteNoise n;
    float os[4] {};
    for (int i = 0; i < 1000; ++i)
    {
        const float x = n.next();
        CHECK (upsample (cfg, up, x, os) == 1);
        CHECK (os[0] == x);
        CHECK (downsample (cfg, down, os) == x);
    }
}

TEST_CASE ("oversampler: unsupported factors fall back to 1x", "[dsp][os]")
{
    for (int f : { 0, -2, 3, 8 })
        CHECK (makeOsConfig (f, OsKind::Fir, kFs).factor == 1);
    CHECK (makeOsConfig (2, OsKind::Fir, kFs).factor == 2);
    CHECK (makeOsConfig (4, OsKind::Iir, kFs).factor == 4);
}

TEST_CASE ("upsampler: images of a 20 kHz tone are >= 100 dB down, fundamental at unity", "[dsp][os][response]")
{
    const auto kind = GENERATE (OsKind::Iir, OsKind::Fir);
    const int factor = GENERATE (2, 4);
    CAPTURE (kind == OsKind::Iir, factor);
    const OsConfig cfg = makeOsConfig (factor, kind, kFs);

    const double f0 = 20000.0, amp = 0.5;
    const size_t settle = 8820, count = 4410;   // count: 10 Hz bins at the base rate
    UpChain up;
    reset (up);
    std::vector<float> y;
    float os[4] {};
    for (size_t n = 0; n < settle + count; ++n)
    {
        const float x = static_cast<float> (amp * std::sin (kTwoPi * f0 * static_cast<double> (n) / kFs));
        upsample (cfg, up, x, os);
        y.insert (y.end(), os, os + factor);
    }
    const double fsOs = kFs * factor;
    const size_t from = settle * static_cast<size_t> (factor), len = count * static_cast<size_t> (factor);
    const double fund = toneAmp (y, from, len, f0, fsOs);
    CHECK (std::abs (db (fund / amp)) < 0.01);

    for (int m = 1; m < factor; ++m)
        for (double img : { m * kFs - f0, m * kFs + f0 })
            if (img < fsOs / 2.0)
            {
                CAPTURE (img);
                CHECK (db (toneAmp (y, from, len, img, fsOs) / fund) <= -100.0);
            }
}

TEST_CASE ("downsampler: tones that would fold onto 20 kHz are >= 100 dB down", "[dsp][os][response]")
{
    const auto kind = GENERATE (OsKind::Iir, OsKind::Fir);
    const int factor = GENERATE (2, 4);
    const OsConfig cfg = makeOsConfig (factor, kind, kFs);
    const double fsOs = kFs * factor;

    std::vector<double> folders { kFs - 20000.0 };              // stage 1
    if (factor == 4)
        folders.push_back (2.0 * kFs - 20000.0);                 // stage 2 (folds at 2 fs)

    for (double f : folders)
    {
        CAPTURE (kind == OsKind::Iir, factor, f);
        DownChain down;
        reset (down);
        const size_t settle = 8820, count = 4410;
        std::vector<float> y;
        float os[4] {};
        for (size_t n = 0; n < settle + count; ++n)
        {
            for (int p = 0; p < factor; ++p)
            {
                const double t = static_cast<double> (n * static_cast<size_t> (factor) + static_cast<size_t> (p)) / fsOs;
                os[p] = static_cast<float> (0.5 * std::sin (kTwoPi * f * t));
            }
            y.push_back (downsample (cfg, down, os));
        }
        CHECK (db (toneAmp (y, settle, count, 20000.0, kFs) / 0.5) <= -100.0);
    }
}

TEST_CASE ("oversampler composite is flat to 0.01 dB from 20 Hz to 20 kHz", "[dsp][os][response]")
{
    const auto kind = GENERATE (OsKind::Iir, OsKind::Fir);
    const int factor = GENERATE (2, 4);
    CAPTURE (kind == OsKind::Iir, factor);
    const auto ir = compositeIr (makeOsConfig (factor, kind, kFs), 16384);
    for (double hz = 20.0; hz <= 20000.0; hz *= 1.1)
    {
        CAPTURE (hz);
        CHECK (std::abs (magnitudeDb (ir, hz, kFs)) < 0.01);
    }
}

TEST_CASE ("FIR oversampler: composite is linear phase, peaking exactly at the reported latency", "[dsp][os][latency]")
{
    const int factor = GENERATE (2, 4);
    const OsConfig cfg = makeOsConfig (factor, OsKind::Fir, kFs);
    const int lat = latencySamples (cfg);
    REQUIRE (lat > 0);
    const auto ir = compositeIr (cfg, static_cast<size_t> (4 * lat + 64));

    size_t peak = 0;
    for (size_t i = 1; i < ir.size(); ++i)
        if (std::abs (ir[i]) > std::abs (ir[peak]))
            peak = i;
    CHECK (peak == static_cast<size_t> (lat));
    for (int i = 1; i <= lat; ++i)
        CHECK (std::abs (ir[static_cast<size_t> (lat - i)] - ir[static_cast<size_t> (lat + i)]) < 1e-6f);
    for (size_t i = static_cast<size_t> (2 * lat + 1); i < ir.size(); ++i)
        CHECK (ir[i] == 0.0f);   // finite: nothing after the last tap
}

TEST_CASE ("FIR oversampler: 4x costs more latency than 2x, both under 2 ms at 44.1 kHz", "[dsp][os][latency]")
{
    const int l2 = latencySamples (makeOsConfig (2, OsKind::Fir, kFs));
    const int l4 = latencySamples (makeOsConfig (4, OsKind::Fir, kFs));
    CHECK (l4 > l2);
    CHECK (l4 < static_cast<int> (0.002 * kFs));
}

TEST_CASE ("IIR oversampler: reported latency matches the 1 kHz delay within half a sample", "[dsp][os][latency]")
{
    const int factor = GENERATE (2, 4);
    const OsConfig cfg = makeOsConfig (factor, OsKind::Iir, kFs);
    const int lat = latencySamples (cfg);
    CHECK (lat >= 1);
    CHECK (lat <= 8);   // TRACK mode budget

    const auto ir = compositeIr (cfg, 16384);
    const double f = 1000.0, w = kTwoPi * f / kFs;
    const double phase = std::arg (dtft (ir, f, kFs));      // = -w * delay (delay < half a period)
    const double delay = -phase / w;
    CAPTURE (delay, lat);
    CHECK (std::abs (delay - lat) <= 0.5);
}

TEST_CASE ("oversampler chains recover from non-finite input", "[dsp][os][stability]")
{
    const auto kind = GENERATE (OsKind::Iir, OsKind::Fir);
    const OsConfig cfg = makeOsConfig (4, kind, kFs);
    UpChain up;
    DownChain down;
    reset (up);
    reset (down);
    float os[4] {};
    upsample (cfg, up, std::numeric_limits<float>::quiet_NaN(), os);
    downsample (cfg, down, os);
    CHECK (resetIfNotFinite (up));
    CHECK (resetIfNotFinite (down));
    CHECK_FALSE (resetIfNotFinite (up));

    float y = 0.0f;
    for (int i = 0; i < 4096; ++i)
    {
        upsample (cfg, up, 0.25f, os);
        y = downsample (cfg, down, os);
        REQUIRE (std::isfinite (y));
    }
    CHECK (std::abs (y - 0.25f) < 1e-5f);   // DC passes at unity
}

TEST_CASE ("upsampler: DC comes out flat on every polyphase branch (no image at the base rate)", "[dsp][os][response]")
{
    const auto kind = GENERATE (OsKind::Iir, OsKind::Fir);
    const int factor = GENERATE (2, 4);
    CAPTURE (kind == OsKind::Iir, factor);
    const OsConfig cfg = makeOsConfig (factor, kind, kFs);
    UpChain up;
    reset (up);
    float os[4] {};
    float worst = 0.0f;
    for (int n = 0; n < 20000; ++n)
    {
        upsample (cfg, up, 0.5f, os);
        if (n >= 10000)
            for (int p = 0; p < factor; ++p)
                worst = std::max (worst, std::abs (os[p] - 0.5f));
    }
    CAPTURE (worst);
    // FIR: float tap rounding only (an unbalanced branch pair measured above 2e-7).
    // IIR: all-pass DC gain is exactly 1 in theory; float recursion near a = 1 leaves 6e-7.
    CHECK (worst < (kind == OsKind::Fir ? 2e-7f : 2e-6f));
}
