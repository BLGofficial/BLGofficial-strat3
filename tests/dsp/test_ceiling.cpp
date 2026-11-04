// CEILING layer (always last, depth-exempt).
//   True-peak detection: 4x polyphase interpolation (BS.1770-4 Annex 2 method, own windowed-
//   sinc filter: 16 taps per phase, 8 samples of look-ahead).
//   TP limiter: needed gain -> sliding minimum covering the look-ahead plus the interpolator
//   support -> instant attack / fixed release -> box over L+1 -> delayed audio.
//   TRACK (no look-ahead): sample-peak only, zero latency, true peak unguarded.
//   Dither (16 bit only): TPDF +-1 LSB, optionally through a 9-tap error-feedback noise shaper
//   designed from an inverted hearing-threshold curve (minimum phase via cepstrum).
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>
#include <limits>
#include <memory>
#include <numbers>
#include <vector>

#include "analysis/TruePeak.h"
#include "dsp/layers/CeilingLayer.h"
#include "support/Analysis.h"

using namespace strat3::dsp;
using namespace strat3::analysis;
using namespace strat3::test;

namespace {

constexpr double kFs = 48000.0;
constexpr double kTwoPi = 2.0 * std::numbers::pi;
constexpr double kQ16 = 1.0 / 32768.0;

std::unique_ptr<CeilingLayer> makeCeiling (const CeilingSettings& s, bool lookahead = true, double fs = kFs)
{
    auto c = std::make_unique<CeilingLayer>();
    prepare (*c, fs, lookahead);
    setSettings (*c, s);
    snapToTargets (*c);
    return c;
}

CeilingSettings settings (float ceilingDb, DitherMode d = DitherMode::Off, int bits = 24)
{
    CeilingSettings s;
    s.ceilingDb = ceilingDb;
    s.dither = d;
    s.bits = bits;
    return s;
}

struct Stereo
{
    std::vector<float> l, r;
};

Stereo run (CeilingLayer& c, const std::vector<float>& l, const std::vector<float>& r)
{
    Stereo out;
    for (size_t i = 0; i < l.size(); ++i)
    {
        float a = l[i], b = r[i];
        process (c, a, b);
        out.l.push_back (a);
        out.r.push_back (b);
    }
    return out;
}

std::vector<float> sine (double hz, double amp, size_t n, double phase = 0.0, double fs = kFs)
{
    std::vector<float> x (n);
    for (size_t i = 0; i < n; ++i)
        x[i] = static_cast<float> (amp * std::sin (kTwoPi * hz * static_cast<double> (i) / fs + phase));
    return x;
}

std::vector<float> lowpassedNoise (float amp, size_t n, double hz, std::uint64_t seed)
{
    WhiteNoise w;
    w.counter = seed;
    const float a = static_cast<float> (std::exp (-kTwoPi * hz / kFs));
    float s1 = 0.0f, s2 = 0.0f;
    std::vector<float> x (n);
    for (auto& v : x)
    {
        s1 = (1.0f - a) * w.next() + a * s1;
        s2 = (1.0f - a) * s1 + a * s2;
        v = amp * s2;
    }
    return x;
}

double db (double v) { return 20.0 * std::log10 (std::max (v, 1e-30)); }
double db (float v) { return db (static_cast<double> (v)); }

/** Reference "analog" peak: 32x windowed-sinc reconstruction, +-64 samples, double precision. */
double referencePeak (const std::vector<float>& x, size_t from, size_t to)
{
    constexpr int half = 64, over = 32;
    const double beta = 10.0;
    auto i0 = [] (double v) { double s = 1.0, t = 1.0; for (int k = 1; k < 60; ++k) { t *= (v * v / 4.0) / (k * k); s += t; } return s; };
    const double i0b = i0 (beta);
    double peak = 0.0;
    for (size_t n = std::max<size_t> (from, half); n + half < std::min (to, x.size()); ++n)
        for (int p = 0; p < over; ++p)
        {
            const double t = static_cast<double> (p) / over;
            double acc = 0.0;
            for (int k = -half + 1; k <= half; ++k)
            {
                const double tau = t - k;
                const double sinc = std::abs (tau) < 1e-12 ? 1.0 : std::sin (std::numbers::pi * tau) / (std::numbers::pi * tau);
                const double r = tau / half;
                const double w = std::abs (r) >= 1.0 ? 0.0 : i0 (beta * std::sqrt (1.0 - r * r)) / i0b;
                acc += static_cast<double> (x[n + static_cast<size_t> (k)]) * sinc * w;
            }
            peak = std::max (peak, std::abs (acc));
        }
    return peak;
}

/** What a BS.1770-4 meter reads: our own 4x detector, intervals [from, to) only. Nothing is
 *  padded after the end: a hard stop would ring above the signal and read as a fake over. */
float meterPeak (const std::vector<float>& x, size_t from, size_t to = 0)
{
    if (to == 0 || to > x.size())
        to = x.size();
    TruePeakDetector d;
    reset (d);
    float peak = 0.0f;
    for (size_t i = 0; i < to; ++i)
    {
        const float tp = push (d, x[i]);
        if (i >= from + static_cast<size_t> (kTruePeakLookahead))
            peak = std::max (peak, tp);
    }
    return peak;
}

/** Power spectral density estimate (averaged periodogram, Hann) of `x` at `hz`. */
double psd (const std::vector<double>& x, double hz, size_t seg = 4096)
{
    double sum = 0.0;
    int count = 0;
    for (size_t start = 0; start + seg <= x.size(); start += seg / 2)
    {
        std::complex<double> acc { 0.0, 0.0 };
        for (size_t n = 0; n < seg; ++n)
        {
            const double w = 0.5 - 0.5 * std::cos (kTwoPi * static_cast<double> (n) / static_cast<double> (seg));
            acc += x[start + n] * w * std::polar (1.0, -kTwoPi * hz * static_cast<double> (n) / kFs);
        }
        sum += std::norm (acc);
        ++count;
    }
    return sum / count;
}

} // namespace

// ------------------------------------------------------------------- true-peak detector

TEST_CASE ("true-peak detector never over-reads and under-reads by at most 0.6 dB up to 20 kHz", "[dsp][ceiling][tp]")
{
    for (double hz : { 997.0, 5000.0, 10000.0, 12000.0, 15000.0, 18000.0, 20000.0 })
        for (double ph : { 0.0, 0.3, 0.785398, 1.2 })
        {
            const auto x = sine (hz, 0.5, 4800, ph);
            const double meter = meterPeak (x, 512, 1100), ref = referencePeak (x, 512, 1100 + 64);
            CAPTURE (hz, ph, db (meter / ref));
            CHECK (db (meter / ref) <= 0.05);
            CHECK (db (meter / ref) >= (hz <= 10000.0 ? -0.15 : -0.6));
        }
}

TEST_CASE ("true-peak detector sees the classic inter-sample over: fs/4 at 45 degrees", "[dsp][ceiling][tp]")
{
    const auto x = sine (12000.0, 1.0, 4800, std::numbers::pi / 4.0);   // samples at +-0.707
    float sp = 0.0f;
    for (float v : x)
        sp = std::max (sp, std::abs (v));
    CHECK (std::abs (db (sp) + 3.01) < 0.01);
    CHECK (std::abs (db (meterPeak (x, 512))) < 0.15);
}

TEST_CASE ("true-peak detector: phase 0 is the input sample itself, delayed by the look-ahead", "[dsp][ceiling][tp]")
{
    TruePeakDetector d;
    reset (d);
    std::vector<float> tp;
    for (int i = 0; i < 64; ++i)
        tp.push_back (push (d, i == 10 ? 0.5f : 0.0f));
    // The interval starting at the impulse reports it at index 10 + look-ahead.
    CHECK (tp[static_cast<size_t> (10 + kTruePeakLookahead)] >= 0.5f);
    for (int i = 0; i < 10; ++i)   // intervals before the impulse's support
        CHECK (tp[static_cast<size_t> (i)] == 0.0f);
}

TEST_CASE ("true-peak detector: interpolated points sit at t0 + p/4 of the reported interval", "[dsp][ceiling][tp]")
{
    // A 1 kHz sine is well inside the passband: each reported peak must match the analytic
    // sine at the four points of [n - D, n - D + 1). One sample of misalignment is ~0.12 off.
    const double amp = 0.9, ph = 0.3, w = kTwoPi * 1000.0 / kFs;
    const auto x = sine (1000.0, amp, 400, ph);
    TruePeakDetector d;
    reset (d);
    double worst = 0.0;
    for (size_t n = 0; n < x.size(); ++n)
    {
        const float tp = push (d, x[n]);
        if (n < 64)
            continue;
        double expected = 0.0;
        for (int p = 0; p < kTruePeakOversample; ++p)
        {
            const double t = static_cast<double> (n) - kTruePeakLookahead + static_cast<double> (p) / kTruePeakOversample;
            expected = std::max (expected, std::abs (amp * std::sin (w * t + ph)));
        }
        worst = std::max (worst, std::abs (static_cast<double> (tp) - expected));
    }
    CAPTURE (worst);
    CHECK (worst < 1e-3);   // measured 4.1e-4: the 16-tap passband ripple at 1 kHz
}

TEST_CASE ("true-peak detector reads DC exactly: every phase has unity gain", "[dsp][ceiling][tp]")
{
    TruePeakDetector d;
    reset (d);
    float tp = 0.0f;
    for (int i = 0; i < 64; ++i)
        tp = push (d, 0.5f);
    CAPTURE (tp - 0.5f);
    CHECK (std::abs (tp - 0.5f) < 1e-6f);

    // Phase 0 dominates a DC reading, so check the interpolating phases directly: before
    // normalisation they sum to ~0.999 (a 0.01 dB under-read at low frequencies).
    for (const auto& ph : kTruePeakFilter.tap)
    {
        double sum = 0.0;
        for (float h : ph)
            sum += static_cast<double> (h);
        CHECK (std::abs (sum - 1.0) < 1e-6);
    }
}

// ------------------------------------------------------------------- limiter: neutral, latency

TEST_CASE ("CEILING below the ceiling is a bit-exact delay", "[dsp][ceiling][null]")
{
    const bool la = GENERATE (false, true);
    auto c = makeCeiling (settings (-1.0f), la);
    const int lat = latencySamples (*c);
    const auto l = lowpassedNoise (0.5f, 48000, 3000.0, 1), r = lowpassedNoise (0.5f, 48000, 3000.0, 2);
    const auto out = run (*c, l, r);
    for (size_t i = 0; i < out.l.size(); ++i)
    {
        const float el = i >= static_cast<size_t> (lat) ? l[i - static_cast<size_t> (lat)] : 0.0f;
        const float er = i >= static_cast<size_t> (lat) ? r[i - static_cast<size_t> (lat)] : 0.0f;
        REQUIRE (out.l[i] == el);
        REQUIRE (out.r[i] == er);
    }
}

TEST_CASE ("CEILING latency: 0 in TRACK; look-ahead + interpolator support + detector delay otherwise", "[dsp][ceiling][latency]")
{
    for (double fs : { 44100.0, 48000.0, 96000.0 })
    {
        CAPTURE (fs);
        CHECK (latencySamples (*makeCeiling ({}, false, fs)) == 0);
        const int la = static_cast<int> (std::lround (static_cast<double> (kCeilingLookaheadMs) * 0.001 * fs));
        CHECK (latencySamples (*makeCeiling ({}, true, fs)) == la + 2 * kTruePeakLookahead);
    }
}

// ------------------------------------------------------------------- limiter: guarantees

TEST_CASE ("CEILING catches the inter-sample over a sample-peak limiter misses", "[dsp][ceiling][limit]")
{
    auto c = makeCeiling (settings (-1.0f));
    const auto x = sine (12000.0, 1.0, 48000, std::numbers::pi / 4.0);
    const auto out = run (*c, x, x);
    const float meter = meterPeak (out.l, 24000);
    CAPTURE (db (meter));
    CHECK (db (meter) <= -1.0 + 0.01);
    CHECK (db (meter) > -1.3);   // and it does not over-limit
}

TEST_CASE ("CEILING output true peak stays at the ceiling for program and worst-case material", "[dsp][ceiling][limit]")
{
    const float ceil = GENERATE (-3.0f, -1.0f, 0.0f);
    CAPTURE (ceil);
    std::vector<std::vector<float>> signals;
    signals.push_back (lowpassedNoise (4.0f, 96000, 6000.0, 3));            // hot program
    {
        auto sq = sine (997.0, 1.0, 96000);                                    // hard-clipped sine:
        for (auto& v : sq)                                                     // big inter-sample overs
            v = std::clamp (3.0f * v, -1.0f, 1.0f);
        signals.push_back (sq);
    }
    signals.push_back (sine (11025.0, 1.2, 96000, 0.7));
    for (const auto& x : signals)
    {
        auto c = makeCeiling (settings (ceil));
        const auto out = run (*c, x, x);
        float sp = 0.0f;
        for (float v : out.l)
            sp = std::max (sp, std::abs (v));
        const float meter = meterPeak (out.l, 0);
        const double analog = referencePeak (out.l, 48000, 60000);
        CAPTURE (db (sp), db (meter), db (analog));
        CHECK (db (sp) <= static_cast<double> (ceil) + 0.01);
        CHECK (db (meter) <= static_cast<double> (ceil) + 0.01);
        CHECK (db (analog) <= static_cast<double> (ceil) + 0.6);   // 4x metering itself under-reads by up to ~0.5 dB
    }
}

TEST_CASE ("CEILING in TRACK mode guards the sample peak exactly", "[dsp][ceiling][limit]")
{
    auto c = makeCeiling (settings (-1.0f), false);
    const auto x = lowpassedNoise (4.0f, 48000, 6000.0, 9);
    const auto out = run (*c, x, x);
    const float ceil = std::pow (10.0f, -1.0f / 20.0f) * (1.0f + 1e-6f);
    for (float v : out.l)
        REQUIRE (std::abs (v) <= ceil);
}

TEST_CASE ("CEILING is always stereo-linked", "[dsp][ceiling][limit]")
{
    auto c = makeCeiling (settings (-1.0f));
    const auto l = sine (1000.0, 2.0, 24000), r = sine (1000.0, 0.1, 24000);
    run (*c, l, r);
    CHECK (ceilingGainDb (*c, 0) == ceilingGainDb (*c, 1));
    CHECK (ceilingGainDb (*c, 0) < -6.0f);

    // Either channel drives the shared gain.
    auto c2 = makeCeiling (settings (-1.0f));
    run (*c2, r, l);
    CHECK (ceilingGainDb (*c2, 0) < -6.0f);
}

TEST_CASE ("CEILING release follows the fixed release time", "[dsp][ceiling][limit]")
{
    auto c = makeCeiling (settings (-1.0f));
    run (*c, sine (1000.0, 2.0, 24000), sine (1000.0, 2.0, 24000));
    const float deepest = ceilingGainDb (*c, 0);
    REQUIRE (deepest < -6.0f);
    int half = 0;
    for (; half < 480000 && ceilingGainDb (*c, 0) < 0.5f * deepest; ++half)
    {
        float a = 0.0f, b = 0.0f;
        process (*c, a, b);
    }
    // Hold + box (~ L + 2S + L samples) then an exponential: half the dB at ln 2 time constants.
    const double expected = std::log (2.0) * static_cast<double> (kCeilingReleaseMs) * 0.001 * kFs;
    CAPTURE (half, expected);
    CHECK (half > 0.7 * expected);
    CHECK (half < 1.3 * expected + 200.0);
}

TEST_CASE ("CEILING releases back to exactly 0 dB", "[dsp][ceiling][limit]")
{
    auto c = makeCeiling (settings (-1.0f));
    run (*c, sine (1000.0, 2.0, 24000), sine (1000.0, 2.0, 24000));
    int i = 0;
    for (; i < 480000 && ceilingGainDb (*c, 0) < 0.0f; ++i)
    {
        float a = 0.0f, b = 0.0f;
        process (*c, a, b);
    }
    CAPTURE (i);
    CHECK (ceilingGainDb (*c, 0) == 0.0f);
    CHECK (i < 50 * static_cast<int> (static_cast<double> (kCeilingReleaseMs) * 0.001 * kFs));   // ln(6 dB / snap) ~ 36 time constants
}

TEST_CASE ("CEILING look-ahead attack is a ramp, not a step", "[dsp][ceiling][limit]")
{
    auto c = makeCeiling (settings (-3.0f));
    const int L = static_cast<int> (std::lround (static_cast<double> (kCeilingLookaheadMs) * 0.001 * kFs));
    auto x = sine (1000.0, 0.1, 24000);
    const auto loud = sine (1000.0, 2.0, 24000);   // ~9 dB over a -3 dBTP ceiling
    x.insert (x.end(), loud.begin(), loud.end());
    float prev = 0.0f, worst = 0.0f, deepest = 0.0f;
    for (float v : x)
    {
        float a = v, b = v;
        process (*c, a, b);
        const float g = ceilingGainDb (*c, 0);
        worst = std::max (worst, std::abs (g - prev));
        deepest = std::min (deepest, g);
        prev = g;
    }
    CAPTURE (worst, deepest);
    CHECK (deepest < -5.0f);
    CHECK (worst <= -deepest / static_cast<float> (L + 1) * 1.02f);
}

// ------------------------------------------------------------------- dither

TEST_CASE ("dither off or 24 bit: output is the limiter output, untouched", "[dsp][ceiling][dither]")
{
    for (auto [mode, bits] : { std::pair { DitherMode::Off, 16 }, std::pair { DitherMode::Tpdf, 24 }, std::pair { DitherMode::TpdfShaped, 24 } })
    {
        auto c = makeCeiling (settings (-1.0f, mode, bits), false);
        const auto x = lowpassedNoise (0.3f, 4800, 3000.0, 4);
        const auto out = run (*c, x, x);
        for (size_t i = 0; i < x.size(); ++i)
            REQUIRE (out.l[i] == x[i]);
    }
}

TEST_CASE ("16-bit dither lands every sample on the 16-bit grid", "[dsp][ceiling][dither]")
{
    const auto mode = GENERATE (DitherMode::Tpdf, DitherMode::TpdfShaped);
    auto c = makeCeiling (settings (-1.0f, mode, 16), false);
    const auto x = lowpassedNoise (0.3f, 48000, 3000.0, 5);
    const auto out = run (*c, x, x);
    for (float v : out.l)
    {
        const double k = static_cast<double> (v) / kQ16;
        REQUIRE (k == std::round (k));
    }
}

TEST_CASE ("TPDF: error mean and power do not depend on the signal (no noise modulation)", "[dsp][ceiling][dither]")
{
    // Without dither, or with RPDF, the error power swings with the input's position between
    // two LSBs. TPDF makes the first two moments of the total error constant: q^2/12 + q^2/6.
    for (double offset : { 0.0, 0.25, 0.5, 0.75 })
    {
        auto c = makeCeiling (settings (-1.0f, DitherMode::Tpdf, 16), false);
        const float dc = static_cast<float> ((1000.0 + offset) * kQ16);
        double mean = 0.0, power = 0.0;
        const int n = 200000;
        for (int i = 0; i < n; ++i)
        {
            float a = dc, b = dc;
            process (*c, a, b);
            const double e = static_cast<double> (a) - static_cast<double> (dc);
            mean += e;
            power += e * e;
        }
        mean /= n;
        power /= n;
        CAPTURE (offset, mean / kQ16, power / (kQ16 * kQ16));
        CHECK (std::abs (mean) < 0.01 * kQ16);
        CHECK (std::abs (power / (kQ16 * kQ16) - 0.25) < 0.01);
    }
}

TEST_CASE ("dither channels are independent", "[dsp][ceiling][dither]")
{
    auto c = makeCeiling (settings (-1.0f, DitherMode::Tpdf, 16), false);
    double lr = 0.0, ll = 0.0, rr = 0.0;
    for (int i = 0; i < 100000; ++i)
    {
        float a = 0.0f, b = 0.0f;
        process (*c, a, b);
        lr += static_cast<double> (a) * static_cast<double> (b);
        ll += static_cast<double> (a) * static_cast<double> (a);
        rr += static_cast<double> (b) * static_cast<double> (b);
    }
    CHECK (std::abs (lr / std::sqrt (ll * rr)) < 0.02);
}

TEST_CASE ("noise shaper: 9 taps, monic, minimum phase", "[dsp][ceiling][dither]")
{
    for (double fs : { 44100.0, 48000.0, 96000.0 })
    {
        const auto h = designNoiseShaper (fs);
        CAPTURE (fs);
        CHECK (h.size() == 9);
        // NTF(z) = 1 + sum h_k z^-k. Minimum phase <=> mean log|NTF| over the circle is 0.
        double meanLog = 0.0;
        const int n = 4096;
        for (int k = 0; k < n; ++k)
        {
            std::complex<double> ntf { 1.0, 0.0 };
            for (size_t i = 0; i < h.size(); ++i)
                ntf += static_cast<double> (h[i]) * std::polar (1.0, -kTwoPi * (k + 0.5) / n * static_cast<double> (i + 1));
            meanLog += std::log (std::abs (ntf));
        }
        // 0 for an exact minimum-phase NTF; truncating to 9 taps costs a little (0.035 Np = 0.3 dB
        // of extra noise power at 96 kHz, where the curve spans the most bins).
        CHECK (meanLog / n >= -1e-9);
        CHECK (meanLog / n < 0.05);
    }
}

TEST_CASE ("shaped dither moves noise out of the 2-5 kHz region and above 15 kHz", "[dsp][ceiling][dither]")
{
    auto measure = [] (DitherMode mode) {
        auto c = makeCeiling (settings (-1.0f, mode, 16), false);
        std::vector<double> e;
        const auto x = sine (440.0, 0.25, 4096 * 40);
        for (float v : x)
        {
            float a = v, b = v;
            process (*c, a, b);
            e.push_back (static_cast<double> (a) - static_cast<double> (v));
        }
        return e;
    };
    const auto flat = measure (DitherMode::Tpdf), shaped = measure (DitherMode::TpdfShaped);
    const double at4k = 10.0 * std::log10 (psd (shaped, 3500.0) / psd (flat, 3500.0));
    const double at18k = 10.0 * std::log10 (psd (shaped, 18000.0) / psd (flat, 18000.0));
    CAPTURE (at4k, at18k);
    CHECK (at4k < -6.0);
    CHECK (at18k > 6.0);
}

TEST_CASE ("shaped dither stays bounded at full scale", "[dsp][ceiling][dither][stability]")
{
    auto c = makeCeiling (settings (0.0f, DitherMode::TpdfShaped, 16), false);
    const auto x = lowpassedNoise (8.0f, 96000, 8000.0, 6);
    const auto out = run (*c, x, x);
    for (float v : out.l)
    {
        REQUIRE (std::isfinite (v));
        REQUIRE (std::abs (v) <= 1.0f);
    }
}

TEST_CASE ("shaped dither recovers at once after sustained full-scale clipping", "[dsp][ceiling][dither][stability]")
{
    // Pinned at the top of the grid, the quantiser error is no longer bounded by the
    // quantiser: without the error clamp the feedback state winds up and the quiet passage
    // that follows comes out as noise far above the dither.
    auto c = makeCeiling (settings (0.0f, DitherMode::TpdfShaped, 16), false);
    const std::vector<float> pinned (48000, 1.0f);
    run (*c, pinned, pinned);
    const auto quiet = sine (1000.0, 0.1, 4800);
    const auto out = run (*c, quiet, quiet);
    double worst = 0.0;
    for (size_t i = 480; i < quiet.size(); ++i)   // 10 ms to settle
        worst = std::max (worst, std::abs (static_cast<double> (out.l[i] - quiet[i])) / kQ16);
    CAPTURE (worst);
    CHECK (worst < 64.0);
}

// ------------------------------------------------------------------- settings, stability

TEST_CASE ("CEILING settings clamp; non-finite ceiling keeps the previous value; odd bit depths mean 24", "[dsp][ceiling][params]")
{
    auto c = makeCeiling (settings (-2.0f));
    setSettings (*c, settings (std::numeric_limits<float>::quiet_NaN()));
    CHECK (c->settings.ceilingDb == -2.0f);
    setSettings (*c, settings (-9.0f));
    CHECK (c->settings.ceilingDb == -3.0f);
    setSettings (*c, settings (2.0f, DitherMode::Tpdf, 20));
    CHECK (c->settings.ceilingDb == 0.0f);
    CHECK (c->settings.bits == 24);
}

TEST_CASE ("CEILING ramps a ceiling change instead of jumping", "[dsp][ceiling][smoothing]")
{
    auto c = makeCeiling (settings (0.0f));
    const auto x = sine (1000.0, 1.0, 48000);
    float prev = 0.0f, worst = 0.0f;
    for (size_t i = 0; i < x.size(); ++i)
    {
        if (i == 24000)
            setSettings (*c, settings (-3.0f));
        float a = x[i], b = x[i];
        process (*c, a, b);
        if (i > 24000)
            worst = std::max (worst, std::abs (ceilingGainDb (*c, 0) - prev));
        prev = ceilingGainDb (*c, 0);
    }
    CHECK (worst < 0.01f);   // a jump would still be spread by the box, but at 3 dB / 73 = 0.04 dB per sample
}

TEST_CASE ("CEILING recovers from a NaN input", "[dsp][ceiling][stability]")
{
    auto c = makeCeiling (settings (-1.0f, DitherMode::TpdfShaped, 16));
    float a = std::numeric_limits<float>::quiet_NaN(), b = 0.0f;
    process (*c, a, b);
    CHECK (resetIfNotFinite (*c));
    CHECK_FALSE (resetIfNotFinite (*c));
    const auto x = sine (1000.0, 2.0, 9600);
    for (float v : run (*c, x, x).l)
        REQUIRE (std::isfinite (v));
}

TEST_CASE ("CEILING finds a NaN still in the delay line after the detector has flushed it", "[dsp][ceiling][stability]")
{
    // The detector ring (16) forgets a NaN long before the audio delay (L + 2S) does.
    auto c = makeCeiling (settings (-1.0f));
    float a = std::numeric_limits<float>::quiet_NaN(), b = 0.0f;
    process (*c, a, b);
    for (int i = 0; i < 2 * kTruePeakTaps; ++i)
    {
        float x = 0.1f, y = 0.1f;
        process (*c, x, y);
    }
    REQUIRE (latencySamples (*c) > 2 * kTruePeakTaps);
    CHECK (resetIfNotFinite (*c));
    for (float v : run (*c, sine (1000.0, 0.5, 4800), sine (1000.0, 0.5, 4800)).l)
        REQUIRE (std::isfinite (v));
}

TEST_CASE ("CEILING state never goes subnormal in silence when flushed per block", "[dsp][ceiling][stability]")
{
    auto c = makeCeiling (settings (-1.0f));
    WhiteNoise n;
    for (int i = 0; i < 480000; ++i)
    {
        float a = i < 24000 ? 3.0f * n.next() : 0.0f, b = -a;
        process (*c, a, b);
        if (i % 64 == 63)
            flushDenormals (*c);
    }
    bool sub = false;
    forEachFloatState (*c, [&] (float v) { sub |= isSubnormal (v); });
    CHECK_FALSE (sub);
}
