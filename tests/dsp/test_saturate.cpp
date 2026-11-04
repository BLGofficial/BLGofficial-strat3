// SATURATE layer (band domain): per-band waveshaper, residual form with first-order ADAA,
// inside the per-sample oversampler.
//   out = ref + mix * down(ADAA(r)(k * up(x)) / k)     ref = down(up(x))
//   k = 10^(drive/20) - 1, so drive 0 dB is exactly linear and every model has unity
//   small-signal gain. r(u) = f(u) - u is the model's distortion residual.
// Models: warm-even (biased tanh), odd (tanh), tape-style (tanh between a pre-emphasis and
// its exact inverse), triode-style (asymmetric cubic soft clip).
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>
#include <limits>
#include <memory>
#include <numbers>
#include <vector>

#include "dsp/layers/SaturateLayer.h"
#include "support/Analysis.h"

using namespace strat3::dsp;
using namespace strat3::test;

namespace {

constexpr double kFs = 48000.0;
constexpr double kTwoPi = 2.0 * std::numbers::pi;
constexpr std::array kModels { SatModel::WarmEven, SatModel::Odd, SatModel::Tape, SatModel::Triode };

std::unique_ptr<SaturateLayer> makeSat (const SatSettings& s, int os = 2, OsKind kind = OsKind::Iir, double fs = kFs)
{
    auto sat = std::make_unique<SaturateLayer>();
    prepare (*sat, fs, os, kind);
    setSettings (*sat, s);
    snapToTargets (*sat);
    return sat;
}

SatSettings oneBand (size_t band, SatModel model, float driveDb, float mix = 100.0f)
{
    SatSettings s;
    s.bands[band] = { model, driveDb, mix };
    return s;
}

struct Run
{
    std::vector<float> out, ref;
};

/** Feeds x into band `band` (both channels, other bands silent); returns that band's left channel. */
Run runBand (SaturateLayer& sat, size_t band, const std::vector<float>& x)
{
    Run r;
    r.out.reserve (x.size());
    r.ref.reserve (x.size());
    for (float v : x)
    {
        StereoBands io, ref;
        io.l[band] = v;
        io.r[band] = v;
        process (sat, io, ref);
        r.out.push_back (io.l[band]);
        r.ref.push_back (ref.l[band]);
    }
    return r;
}

std::vector<float> sine (double hz, double amp, size_t n, double fs = kFs)
{
    std::vector<float> x (n);
    for (size_t i = 0; i < n; ++i)
        x[i] = static_cast<float> (amp * std::sin (kTwoPi * hz * static_cast<double> (i) / fs));
    return x;
}

/** Amplitude at `hz` over a coherent window (integer cycles: no leakage). */
double toneAmp (const std::vector<float>& x, size_t from, size_t count, double hz, double fs = kFs)
{
    std::complex<double> sum { 0.0, 0.0 };
    const double w = kTwoPi * hz / fs;
    for (size_t n = 0; n < count; ++n)
        sum += static_cast<double> (x[from + n]) * std::polar (1.0, -w * static_cast<double> (n));
    return 2.0 * std::abs (sum) / static_cast<double> (count);
}

double db (double ratio) { return 20.0 * std::log10 (std::max (ratio, 1e-30)); }

// 1 s settle (DC blockers, ramps), then 9600 samples = 5 Hz bins at 48 kHz.
constexpr size_t kSettle = 48000, kCount = 9600;

double harmonicDb (const std::vector<float>& y, double f0, int h)
{
    return db (toneAmp (y, kSettle, kCount, f0 * h) / toneAmp (y, kSettle, kCount, f0));
}

double thd (const std::vector<float>& y, double f0)
{
    double p = 0.0;
    for (int h = 2; h * f0 < 20000.0; ++h)
    {
        const double a = toneAmp (y, kSettle, kCount, f0 * h);
        p += a * a;
    }
    return std::sqrt (p) / toneAmp (y, kSettle, kCount, f0);
}

/** Power of everything below 20 kHz that is neither DC nor a harmonic of f0, relative to f0. */
double aliasDb (const std::vector<float>& y, double f0, size_t from, size_t count, double fs = kFs)
{
    const double bin = fs / static_cast<double> (count);
    double p = 0.0;
    for (double f = bin; f < 20000.0; f += bin)
    {
        const double h = f / f0;
        if (std::abs (h - std::round (h)) < 1e-6)
            continue;
        const double a = toneAmp (y, from, count, f, fs);
        p += a * a;
    }
    return 10.0 * std::log10 (std::max (p, 1e-60)) - db (toneAmp (y, from, count, f0, fs)) ;
}

} // namespace

// ------------------------------------------------------------------- shaper functions

TEST_CASE ("drive maps to k = 10^(dB/20) - 1: 0 dB is exactly linear", "[dsp][sat][shaper]")
{
    CHECK (satDriveToK (0.0f) == 0.0f);
    CHECK (std::abs (satDriveToK (6.0206f) - 1.0f) < 1e-4f);
    CHECK (std::abs (satDriveToK (24.0f) - 14.8489f) < 1e-3f);
    float prev = -1.0f;
    for (float d = 0.0f; d <= 24.0f; d += 0.25f)
    {
        CHECK (satDriveToK (d) > prev);
        prev = satDriveToK (d);
    }
}

TEST_CASE ("residual integral differentiates to the residual for every model", "[dsp][sat][shaper]")
{
    for (auto m : kModels)
    {
        CAPTURE (static_cast<int> (m));
        for (double u = -20.0; u <= 20.0; u += 0.0137)
        {
            const double h = 1e-5;
            const double d = (satdetail::residualIntegral (m, u + h) - satdetail::residualIntegral (m, u - h)) / (2.0 * h);
            CAPTURE (u);
            REQUIRE (std::abs (d - satdetail::residual (m, u)) < 1e-6 * (1.0 + std::abs (u)));
        }
    }
}

TEST_CASE ("residual is zero with zero slope at the origin: unity small-signal gain", "[dsp][sat][shaper]")
{
    for (auto m : kModels)
    {
        CAPTURE (static_cast<int> (m));
        CHECK (satdetail::residual (m, 0.0) == 0.0);
        CHECK (satdetail::residualIntegral (m, 0.0) == 0.0);
        const double h = 1e-4;
        CHECK (std::abs ((satdetail::residual (m, h) - satdetail::residual (m, -h)) / (2.0 * h)) < 1e-3);
    }
}

TEST_CASE ("residual integral is continuous across the piecewise and large-argument switches", "[dsp][sat][shaper]")
{
    for (auto m : kModels)
        for (double u : { -1.0, 1.0, -1.5, 1.5, -0.9, 0.9, 0.6, -0.6 })   // logcosh switch, triode knees
        {
            CAPTURE (static_cast<int> (m), u);
            CHECK (std::abs (satdetail::residualIntegral (m, u + 1e-9) - satdetail::residualIntegral (m, u - 1e-9)) < 1e-8);
            CHECK (std::abs (satdetail::residual (m, u + 1e-9) - satdetail::residual (m, u - 1e-9)) < 1e-7);
        }
}

TEST_CASE ("odd and tape residuals are odd and compress: |r(u)| <= |u|, opposite sign", "[dsp][sat][shaper]")
{
    for (auto m : { SatModel::Odd, SatModel::Tape })
        for (double u = 0.001; u < 50.0; u *= 1.07)
        {
            const double r = satdetail::residual (m, u);
            CHECK (r < 0.0);
            CHECK (-r <= u);
            CHECK (satdetail::residual (m, -u) == -r);
        }
}

TEST_CASE ("warm-even and triode residuals are asymmetric", "[dsp][sat][shaper]")
{
    for (auto m : { SatModel::WarmEven, SatModel::Triode })
    {
        CAPTURE (static_cast<int> (m));
        CHECK (std::abs (satdetail::residual (m, 1.0) + satdetail::residual (m, -1.0)) > 0.05);
    }
}

TEST_CASE ("residual integral stays finite for huge arguments", "[dsp][sat][shaper][stability]")
{
    for (auto m : kModels)
        for (double u : { -1e4, -800.0, 800.0, 1e4 })
        {
            CHECK (std::isfinite (satdetail::residual (m, u)));
            CHECK (std::isfinite (satdetail::residualIntegral (m, u)));
        }
}

// ------------------------------------------------------------------- layer: neutral, null, latency

TEST_CASE ("SAT neutral (drive 0) output is bit-identical to the reference path", "[dsp][sat][null]")
{
    const int os = GENERATE (1, 2, 4);
    const auto kind = GENERATE (OsKind::Iir, OsKind::Fir);
    auto sat = makeSat ({}, os, kind);
    WhiteNoise n;
    for (int i = 0; i < 20000; ++i)
    {
        StereoBands io, ref;
        for (size_t b = 0; b < 4; ++b)
        {
            io.l[b] = 0.3f * n.next();
            io.r[b] = 0.3f * n.next();
        }
        process (*sat, io, ref);
        for (size_t b = 0; b < 4; ++b)
        {
            REQUIRE (io.l[b] == ref.l[b]);
            REQUIRE (io.r[b] == ref.r[b]);
        }
    }
}

TEST_CASE ("SAT at 1x: the reference is the input, bit-exact, latency 0", "[dsp][sat][null][latency]")
{
    auto sat = makeSat (oneBand (1, SatModel::Odd, 12.0f), 1);
    CHECK (latencySamples (*sat) == 0);
    WhiteNoise n;
    for (int i = 0; i < 5000; ++i)
    {
        StereoBands io, ref;
        for (size_t b = 0; b < 4; ++b)
            io.l[b] = io.r[b] = 0.3f * n.next();
        const StereoBands in = io;
        process (*sat, io, ref);
        for (size_t b = 0; b < 4; ++b)
            REQUIRE (ref.l[b] == in.l[b]);
    }
}

TEST_CASE ("SAT mix 0 at full drive is bit-identical to the reference", "[dsp][sat][null]")
{
    auto sat = makeSat (oneBand (0, SatModel::WarmEven, 24.0f, 0.0f), 4, OsKind::Fir);
    const auto r = runBand (*sat, 0, sine (100.0, 0.7, 20000));
    for (size_t i = 0; i < r.out.size(); ++i)
        REQUIRE (r.out[i] == r.ref[i]);
}

TEST_CASE ("SAT drive on one band leaves the other bands bit-exact", "[dsp][sat][null]")
{
    auto sat = makeSat (oneBand (2, SatModel::Triode, 18.0f), 2);
    WhiteNoise n;
    for (int i = 0; i < 20000; ++i)
    {
        StereoBands io, ref;
        for (size_t b = 0; b < 4; ++b)
            io.l[b] = io.r[b] = 0.5f * n.next();
        process (*sat, io, ref);
        for (size_t b : { 0u, 1u, 3u })
            REQUIRE (io.l[b] == ref.l[b]);
    }
}

TEST_CASE ("SAT reference chain is identical in every band (band merge stays flat)", "[dsp][sat][null]")
{
    auto sat = makeSat (oneBand (0, SatModel::Odd, 12.0f), 4, OsKind::Iir);
    for (int i = 0; i < 4096; ++i)
    {
        StereoBands io, ref;
        const float x = i == 0 ? 1.0f : 0.0f;
        io.l.fill (x);
        io.r.fill (x);
        process (*sat, io, ref);
        for (size_t b = 1; b < 4; ++b)
            REQUIRE (ref.l[b] == ref.l[0]);
    }
}

TEST_CASE ("SAT latency is the oversampler's, and the FIR reference peaks there", "[dsp][sat][latency]")
{
    const int os = GENERATE (1, 2, 4);
    const auto kind = GENERATE (OsKind::Iir, OsKind::Fir);
    auto sat = makeSat ({}, os, kind);
    CHECK (latencySamples (*sat) == latencySamples (makeOsConfig (os, kind, kFs)));
    if (kind == OsKind::Fir)
    {
        std::vector<float> imp (512, 0.0f);
        imp[0] = 1.0f;
        const auto r = runBand (*sat, 3, imp);
        size_t peak = 0;
        for (size_t i = 1; i < r.ref.size(); ++i)
            if (std::abs (r.ref[i]) > std::abs (r.ref[peak]))
                peak = i;
        CHECK (peak == static_cast<size_t> (latencySamples (*sat)));
    }
}

// ------------------------------------------------------------------- layer: tone

TEST_CASE ("SAT small signals pass at unity gain for every model at full drive", "[dsp][sat][tone]")
{
    for (auto m : kModels)
    {
        CAPTURE (static_cast<int> (m));
        auto sat = makeSat (oneBand (0, m, 24.0f), 2);
        const auto r = runBand (*sat, 0, sine (1000.0, 1e-4, kSettle + kCount));
        CHECK (std::abs (db (toneAmp (r.out, kSettle, kCount, 1000.0) / toneAmp (r.ref, kSettle, kCount, 1000.0))) < 0.01);
    }
}

TEST_CASE ("SAT odd model makes odd harmonics only", "[dsp][sat][tone]")
{
    auto sat = makeSat (oneBand (0, SatModel::Odd, 12.0f), 4);
    const auto r = runBand (*sat, 0, sine (1000.0, 0.5, kSettle + kCount));
    CHECK (harmonicDb (r.out, 1000.0, 3) > -40.0);
    CHECK (harmonicDb (r.out, 1000.0, 2) < -90.0);
    CHECK (harmonicDb (r.out, 1000.0, 4) < -90.0);
}

TEST_CASE ("SAT warm-even model is dominated by the second harmonic", "[dsp][sat][tone]")
{
    auto sat = makeSat (oneBand (0, SatModel::WarmEven, 12.0f), 4);
    const auto r = runBand (*sat, 0, sine (1000.0, 0.3, kSettle + kCount));
    CHECK (harmonicDb (r.out, 1000.0, 2) > -40.0);
    CHECK (harmonicDb (r.out, 1000.0, 2) > harmonicDb (r.out, 1000.0, 3));
}

TEST_CASE ("SAT triode model makes a strong second harmonic", "[dsp][sat][tone]")
{
    auto sat = makeSat (oneBand (0, SatModel::Triode, 18.0f), 4);
    const auto r = runBand (*sat, 0, sine (1000.0, 0.5, kSettle + kCount));
    CHECK (harmonicDb (r.out, 1000.0, 2) > -30.0);
}

TEST_CASE ("SAT tape model compresses highs more than lows", "[dsp][sat][tone]")
{
    auto gainDb = [] (double hz) {
        auto sat = makeSat (oneBand (0, SatModel::Tape, 12.0f), 4);
        const auto r = runBand (*sat, 0, sine (hz, 0.5, kSettle + kCount));
        return db (toneAmp (r.out, kSettle, kCount, hz) / toneAmp (r.ref, kSettle, kCount, hz));
    };
    const double low = gainDb (200.0), high = gainDb (8000.0);
    CAPTURE (low, high);
    CHECK (high < low - 1.0);
}

TEST_CASE ("SAT distortion rises strictly with drive for every model", "[dsp][sat][tone]")
{
    for (auto m : kModels)
    {
        CAPTURE (static_cast<int> (m));
        double prev = 0.0;
        for (float d : { 3.0f, 6.0f, 12.0f, 18.0f, 24.0f })
        {
            auto sat = makeSat (oneBand (0, m, d), 2);
            const auto r = runBand (*sat, 0, sine (500.0, 0.3, kSettle + kCount));
            const double t = thd (r.out, 500.0);
            CAPTURE (d, t);
            CHECK (t > prev);
            prev = t;
        }
    }
}

TEST_CASE ("SAT asymmetric models leave no DC", "[dsp][sat][tone]")
{
    for (auto m : { SatModel::WarmEven, SatModel::Triode })
    {
        auto sat = makeSat (oneBand (0, m, 18.0f), 2);
        const auto r = runBand (*sat, 0, sine (100.0, 0.7, kSettle + 24000));
        double mean = 0.0;
        for (size_t i = kSettle; i < r.out.size(); ++i)   // 50 whole cycles
            mean += static_cast<double> (r.out[i]);
        mean /= 24000.0;
        CAPTURE (static_cast<int> (m), mean);
        CHECK (std::abs (mean) < 1e-4);
    }
}

TEST_CASE ("SAT odd model at 1x never exceeds the input peak", "[dsp][sat][tone]")
{
    auto sat = makeSat (oneBand (0, SatModel::Odd, 24.0f), 1);
    WhiteNoise n;
    std::vector<float> x (48000);
    for (auto& v : x)
        v = 0.9f * n.next();
    const auto r = runBand (*sat, 0, x);
    for (size_t i = 0; i < x.size(); ++i)
        REQUIRE (std::abs (r.out[i]) <= 0.9f * (1.0f + 1e-6f));
}

// ------------------------------------------------------------------- layer: aliasing

TEST_CASE ("SAT aliasing falls with oversampling, and ADAA beats a naive shaper at 1x", "[dsp][sat][alias]")
{
    const double f0 = 5000.0;   // odd harmonics fold onto non-harmonic bins at 48 kHz
    const size_t settle = 4800, count = 4800;
    const auto x = sine (f0, 0.5, settle + count);

    double alias[3] {};
    const int factors[3] { 1, 2, 4 };
    for (int i = 0; i < 3; ++i)
    {
        auto sat = makeSat (oneBand (0, SatModel::Odd, 18.0f), factors[i], OsKind::Fir);
        alias[i] = aliasDb (runBand (*sat, 0, x).out, f0, settle, count);
    }

    // Naive (no ADAA) shaper at 1x, same function.
    const double k = satDriveToK (18.0f);
    std::vector<float> naive (x.size());
    for (size_t i = 0; i < x.size(); ++i)
        naive[i] = static_cast<float> (static_cast<double> (x[i]) + satdetail::residual (SatModel::Odd, k * static_cast<double> (x[i])) / k);
    const double naiveAlias = aliasDb (naive, f0, settle, count);

    CAPTURE (alias[0], alias[1], alias[2], naiveAlias);
    CHECK (alias[0] < naiveAlias - 6.0);
    CHECK (alias[1] < alias[0] - 10.0);
    CHECK (alias[2] < alias[1] - 10.0);
    CHECK (alias[2] < -70.0);
}

// ------------------------------------------------------------------- layer: controls

TEST_CASE ("SAT mix is linear in the residual", "[dsp][sat][mix]")
{
    auto full = makeSat (oneBand (0, SatModel::Triode, 12.0f, 100.0f), 2);
    auto half = makeSat (oneBand (0, SatModel::Triode, 12.0f, 50.0f), 2);
    const auto x = sine (300.0, 0.6, 20000);
    const auto a = runBand (*full, 0, x), b = runBand (*half, 0, x);
    for (size_t i = 0; i < x.size(); ++i)
        REQUIRE (std::abs ((b.out[i] - b.ref[i]) - 0.5f * (a.out[i] - a.ref[i])) < 1e-6f);
}

TEST_CASE ("SAT drive changes ramp in (no jump into distortion)", "[dsp][sat][smoothing]")
{
    auto sat = makeSat ({}, 2);
    const auto x = sine (1000.0, 0.5, 48000 + 4800);
    std::vector<float> res;
    for (size_t i = 0; i < x.size(); ++i)
    {
        if (i == 48000)
            setSettings (*sat, oneBand (0, SatModel::Odd, 24.0f));
        StereoBands io, ref;
        io.l[0] = io.r[0] = x[i];
        process (*sat, io, ref);
        res.push_back (io.l[0] - ref.l[0]);
    }
    auto peak = [&] (size_t from, size_t to) {
        float m = 0.0f;
        for (size_t i = from; i < to; ++i)
            m = std::max (m, std::abs (res[i]));
        return m;
    };
    const float firstMs = peak (48000, 48048), settled = peak (48000 + 1440, 48000 + 4800);   // 30..100 ms
    CAPTURE (firstMs, settled);
    CHECK (settled > 0.1f);
    CHECK (firstMs < 0.3f * settled);
}

TEST_CASE ("SAT model switch fades instead of clicking", "[dsp][sat][smoothing]")
{
    auto sat = makeSat (oneBand (0, SatModel::WarmEven, 12.0f), 2);
    const auto x = sine (1000.0, 0.5, 48000 + 9600);
    std::vector<float> y;
    for (size_t i = 0; i < x.size(); ++i)
    {
        if (i == 48000)
            setSettings (*sat, oneBand (0, SatModel::Odd, 12.0f));
        StereoBands io, ref;
        io.l[0] = io.r[0] = x[i];
        process (*sat, io, ref);
        y.push_back (io.l[0]);
    }
    auto maxD2 = [&] (size_t from, size_t to) {
        float m = 0.0f;
        for (size_t i = from; i < to; ++i)
            m = std::max (m, std::abs (y[i] - 2.0f * y[i - 1] + y[i - 2]));
        return m;
    };
    const float before = maxD2 (38400, 48000), after = maxD2 (52800, 57600), during = maxD2 (48000, 52800);
    CAPTURE (before, after, during);
    CHECK (during <= 1.5f * std::max (before, after));

    // And the switch really happened: odd model now, so no 2nd harmonic left.
    CHECK (db (toneAmp (y, 52800, 4800, 2000.0) / toneAmp (y, 52800, 4800, 1000.0)) < -80.0);
}

TEST_CASE ("SAT settings clamp, and non-finite values keep the previous value", "[dsp][sat][params]")
{
    auto sat = makeSat (oneBand (1, SatModel::Odd, 6.0f, 40.0f), 2);
    SatSettings bad = oneBand (1, SatModel::Odd, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity());
    setSettings (*sat, bad);
    CHECK (sat->settings.bands[1].driveDb == 6.0f);
    CHECK (sat->settings.bands[1].mixPercent == 40.0f);

    setSettings (*sat, oneBand (1, SatModel::Odd, 40.0f, 140.0f));
    CHECK (sat->settings.bands[1].driveDb == 24.0f);
    CHECK (sat->settings.bands[1].mixPercent == 100.0f);
    setSettings (*sat, oneBand (1, SatModel::Odd, -3.0f, -5.0f));
    CHECK (sat->settings.bands[1].driveDb == 0.0f);
    CHECK (sat->settings.bands[1].mixPercent == 0.0f);
}

// ------------------------------------------------------------------- layer: stability

TEST_CASE ("SAT stays finite and bounded at extreme settings", "[dsp][sat][stability]")
{
    const auto kind = GENERATE (OsKind::Iir, OsKind::Fir);
    for (auto m : kModels)
    {
        SatSettings s;
        for (auto& b : s.bands)
            b = { m, 24.0f, 100.0f };
        auto sat = makeSat (s, 4, kind);
        WhiteNoise n;
        for (int i = 0; i < 48000; ++i)
        {
            StereoBands io, ref;
            for (size_t b = 0; b < 4; ++b)
            {
                io.l[b] = 2.0f * n.next();
                io.r[b] = 2.0f * n.next();
            }
            process (*sat, io, ref);
            for (size_t b = 0; b < 4; ++b)
            {
                REQUIRE (std::isfinite (io.l[b]));
                REQUIRE (std::abs (io.l[b]) < 8.0f);
                REQUIRE (std::abs (io.r[b]) < 8.0f);
            }
        }
    }
}

TEST_CASE ("SAT recovers from a NaN input", "[dsp][sat][stability]")
{
    auto sat = makeSat (oneBand (0, SatModel::Tape, 12.0f), 4, OsKind::Fir);
    StereoBands io, ref;
    io.l[0] = std::numeric_limits<float>::quiet_NaN();
    process (*sat, io, ref);
    CHECK (resetIfNotFinite (*sat));
    CHECK_FALSE (resetIfNotFinite (*sat));

    const auto r = runBand (*sat, 0, sine (1000.0, 0.5, 4800));
    for (size_t i = 0; i < r.out.size(); ++i)
        REQUIRE (std::isfinite (r.out[i]));
}

TEST_CASE ("SAT state never goes subnormal in silence when flushed per block", "[dsp][sat][stability]")
{
    const auto kind = GENERATE (OsKind::Iir, OsKind::Fir);
    SatSettings s;
    for (size_t b = 0; b < 4; ++b)
        s.bands[b] = { kModels[b], 12.0f, 100.0f };
    auto sat = makeSat (s, 4, kind);
    WhiteNoise n;
    for (int i = 0; i < 480000; ++i)   // 0.5 s noise, then 9.5 s silence
    {
        StereoBands io, ref;
        const float v = i < 24000 ? 0.5f * n.next() : 0.0f;
        io.l.fill (v);
        io.r.fill (v);
        process (*sat, io, ref);
        if (i % 64 == 63)
            flushDenormals (*sat);
    }
    bool sub = false;
    forEachFloatState (*sat, [&] (float v) { sub |= isSubnormal (v); });
    CHECK_FALSE (sub);
}
