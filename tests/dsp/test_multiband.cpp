// MULTIBAND layer: per-band feedforward compressor (down) / upward compressor (up) running on
// the shared band domain. Log-domain soft-knee gain computer, peak / RMS / envelope detectors,
// crest-dependent release, per-band mix, stereo-linked gain, internal or external sidechain.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <numbers>
#include <vector>

#include "dsp/core/DelayLine.h"
#include "dsp/crossover/BandDomain.h"
#include "dsp/layers/MultibandLayer.h"
#include "support/Analysis.h"

using namespace strat3::dsp;
using namespace strat3::test;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double kFs = 48000.0;

MbBand comp (float threshDb, float ratio, MbDetector det = MbDetector::Rms, MbDir dir = MbDir::Down)
{
    MbBand b;
    b.threshDb = threshDb;
    b.ratio = ratio;
    b.dir = dir;
    b.kneeDb = 0.0f;
    b.detector = det;
    return b;
}

MultibandLayer makeMb (const MbSettings& s, double fs = kFs)
{
    MultibandLayer mb;
    prepare (mb, fs);
    setSettings (mb, s);
    snapToTargets (mb);
    return mb;
}

MbSettings oneBand (size_t band, const MbBand& b)
{
    MbSettings s;
    s.bands[band] = b;
    return s;
}

/** Steady sine in one band (both channels), returns output/input peak ratio in dB over the
 *  last quarter of the run. */
double steadyGainDb (MultibandLayer& mb, size_t band, double amplitude, double hz = 1000.0,
                     double fs = kFs, const StereoBands* sidechainEach = nullptr)
{
    const int n = static_cast<int> (fs);
    const double w = 2.0 * std::numbers::pi * hz / fs;
    float inPeak = 0.0f, outPeak = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        StereoBands b;
        b.l[band] = b.r[band] = static_cast<float> (amplitude * std::sin (w * i));
        const float in = b.l[band];
        process (mb, b, sidechainEach);
        if (i >= n * 3 / 4)
        {
            inPeak = std::max (inPeak, std::abs (in));
            outPeak = std::max (outPeak, std::abs (b.l[band]));
        }
    }
    return 20.0 * std::log10 (static_cast<double> (outPeak) / static_cast<double> (inPeak));
}

int ms (double milliseconds, double fs = kFs) { return static_cast<int> (std::lround (milliseconds * 0.001 * fs)); }

} // namespace

// ------------------------------------------------------------------- gain computer

TEST_CASE ("gain computer: ratio 1 is exactly 0 dB for every level, knee and direction", "[dsp][mb][computer]")
{
    for (const auto dir : { MbDir::Down, MbDir::Up })
        for (const float knee : { 0.0f, 6.0f, 24.0f })
            for (float x = -140.0f; x <= 12.0f; x += 0.5f)
            {
                MbBand b = comp (-20.0f, 1.0f, MbDetector::Peak, dir);
                b.kneeDb = knee;
                REQUIRE (gainComputerDb (b, x) == 0.0f);
            }
}

TEST_CASE ("gain computer: hard-knee downward compression", "[dsp][mb][computer]")
{
    const auto b = comp (-20.0f, 4.0f, MbDetector::Peak);
    CHECK (gainComputerDb (b, -30.0f) == 0.0f);
    CHECK (gainComputerDb (b, -20.0f) == 0.0f);
    CHECK_THAT (gainComputerDb (b, -8.0f), WithinAbs (-9.0, 1e-5));   // y = -20 + 12/4 = -17
    CHECK_THAT (gainComputerDb (b, 0.0f), WithinAbs (-15.0, 1e-5));
}

TEST_CASE ("gain computer: soft knee is continuous and quadratic around the threshold", "[dsp][mb][computer]")
{
    auto b = comp (-20.0f, 4.0f, MbDetector::Peak);
    b.kneeDb = 12.0f;
    CHECK (gainComputerDb (b, -26.0f) == 0.0f);                                  // knee start
    CHECK_THAT (gainComputerDb (b, -14.0f), WithinAbs (-0.75 * 6.0, 1e-4));      // knee end = hard curve
    CHECK_THAT (gainComputerDb (b, -20.0f), WithinAbs (-0.75 * 12.0 / 8.0, 1e-4)); // (1/R - 1) W / 8
    // No jumps: adjacent levels differ by at most the slope bound.
    float prev = gainComputerDb (b, -40.0f);
    for (float x = -40.0f; x <= 0.0f; x += 0.01f)
    {
        const float g = gainComputerDb (b, x);
        REQUIRE (std::abs (g - prev) <= 0.0076f);
        prev = g;
    }
}

TEST_CASE ("gain computer: upward compression boosts below threshold, capped at +12 dB", "[dsp][mb][computer]")
{
    const auto b = comp (-20.0f, 2.0f, MbDetector::Peak, MbDir::Up);
    CHECK (gainComputerDb (b, -10.0f) == 0.0f);
    CHECK_THAT (gainComputerDb (b, -30.0f), WithinAbs (5.0, 1e-5));   // y = -20 + (-10)/2 = -25
    CHECK (gainComputerDb (b, -80.0f) == kMbUpwardMaxDb);
    CHECK (gainComputerDb (b, -INFINITY) == kMbUpwardMaxDb);        // silence: finite, capped

    auto knee = b;
    knee.kneeDb = 10.0f;
    CHECK (gainComputerDb (knee, -15.0f) == 0.0f);                    // knee top
    CHECK_THAT (gainComputerDb (knee, -25.0f), WithinAbs (2.5, 1e-4)); // knee bottom = hard curve
}

TEST_CASE ("gain computer never inverts level order (output level is monotonic)", "[dsp][mb][computer]")
{
    for (const auto dir : { MbDir::Down, MbDir::Up })
        for (const float ratio : { 1.5f, 4.0f, 20.0f })
            for (const float knee : { 0.0f, 6.0f, 24.0f })
            {
                MbBand b = comp (-30.0f, ratio, MbDetector::Peak, dir);
                b.kneeDb = knee;
                float prevY = -1e9f;
                for (float x = -100.0f; x <= 12.0f; x += 0.05f)
                {
                    const float y = x + gainComputerDb (b, x);
                    INFO ("dir " << static_cast<int> (dir) << " ratio " << ratio << " knee " << knee << " x " << x);
                    REQUIRE (y >= prevY - 1e-4f);
                    prevY = y;
                }
            }
}

TEST_CASE ("gain computer handles non-finite levels", "[dsp][mb][computer]")
{
    const auto b = comp (-20.0f, 4.0f, MbDetector::Peak);
    CHECK (gainComputerDb (b, -INFINITY) == 0.0f);
    CHECK (std::isfinite (gainComputerDb (b, INFINITY)));
    CHECK (gainComputerDb (b, std::nanf ("")) == 0.0f);
}

TEST_CASE ("program release: nominal for low crest, down to a quarter for high crest", "[dsp][mb][release]")
{
    CHECK (programReleaseMs (100.0f, 0.0f) == 100.0f);
    CHECK (programReleaseMs (100.0f, 3.0f) == 100.0f);    // sine
    CHECK_THAT (programReleaseMs (100.0f, 7.5f), WithinAbs (50.0, 1e-3));
    CHECK_THAT (programReleaseMs (100.0f, 12.0f), WithinAbs (25.0, 1e-4));
    CHECK_THAT (programReleaseMs (100.0f, 40.0f), WithinAbs (25.0, 1e-4));
    float prev = 1e9f;
    for (float c = 0.0f; c < 30.0f; c += 0.1f)
    {
        REQUIRE (programReleaseMs (200.0f, c) <= prev);
        prev = programReleaseMs (200.0f, c);
    }
}

// ------------------------------------------------------------------- neutral

TEST_CASE ("MB neutral defaults are bit-exact passthrough", "[dsp][mb][null]")
{
    auto mb = makeMb (MbSettings {});
    Noise noise;
    for (int i = 0; i < 48000; ++i)
    {
        StereoBands in;
        for (size_t b = 0; b < 4; ++b) { in.l[b] = noise.next(); in.r[b] = noise.next(); }
        auto out = in;
        process (mb, out, nullptr);
        REQUIRE (out.l == in.l);
        REQUIRE (out.r == in.r);
    }
}

TEST_CASE ("MB with mix 0 is bit-exact passthrough even while compressing hard", "[dsp][mb][null]")
{
    MbSettings s;
    for (auto& b : s.bands) { b = comp (-60.0f, 20.0f); b.mixPercent = 0.0f; }
    auto mb = makeMb (s);
    Noise noise;
    for (int i = 0; i < 48000; ++i)
    {
        StereoBands in;
        for (size_t b = 0; b < 4; ++b) { in.l[b] = noise.next(); in.r[b] = noise.next(); }
        auto out = in;
        process (mb, out, nullptr);
        REQUIRE (out.l == in.l);
        REQUIRE (out.r == in.r);
    }
    CHECK (bandGainDb (mb, 0) < -20.0f);   // the detector still runs (metering, instant mix-in)
}

// ------------------------------------------------------------------- static curves

TEST_CASE ("RMS detector: steady sine settles on the computed gain", "[dsp][mb][static]")
{
    auto mb = makeMb (oneBand (1, comp (-20.0f, 4.0f, MbDetector::Rms)));
    // 0.5 peak sine: RMS -9.03 dB -> gain = -0.75 * 10.97 = -8.23 dB
    CHECK_THAT (steadyGainDb (mb, 1, 0.5), WithinAbs (-0.75 * (20.0 * std::log10 (0.5 / std::sqrt (2.0)) + 20.0), 0.05));
}

TEST_CASE ("peak detector: fast attack / slow release follows the crest", "[dsp][mb][static]")
{
    auto b = comp (-20.0f, 4.0f, MbDetector::Peak);
    b.attackMs = 0.1f;
    b.releaseMs = 1000.0f;
    auto mb = makeMb (oneBand (2, b));
    CHECK_THAT (steadyGainDb (mb, 2, 0.5), WithinAbs (-0.75 * (20.0 * std::log10 (0.5) + 20.0), 0.1));
}

TEST_CASE ("envelope detector: louder input always gets more reduction", "[dsp][mb][static]")
{
    double prev = 1.0;
    for (const double a : { 0.05, 0.1, 0.2, 0.4, 0.8 })
    {
        auto mb = makeMb (oneBand (0, comp (-30.0f, 4.0f, MbDetector::Envelope)));
        const double g = steadyGainDb (mb, 0, a, 200.0);
        INFO ("amplitude " << a << " gain " << g);
        CHECK (g < prev);
        prev = g;
    }
    CHECK (prev < -10.0);
}

TEST_CASE ("upward mode raises a quiet band, capped at +12 dB", "[dsp][mb][static]")
{
    {
        auto mb = makeMb (oneBand (1, comp (-20.0f, 2.0f, MbDetector::Rms, MbDir::Up)));
        // 0.01 peak: RMS -43.0 dB -> +11.5 dB
        CHECK_THAT (steadyGainDb (mb, 1, 0.01), WithinAbs (0.5 * (-20.0 - 20.0 * std::log10 (0.01 / std::sqrt (2.0))), 0.05));
    }
    {
        auto mb = makeMb (oneBand (1, comp (-20.0f, 2.0f, MbDetector::Rms, MbDir::Up)));
        CHECK_THAT (steadyGainDb (mb, 1, 0.001), WithinAbs (static_cast<double> (kMbUpwardMaxDb), 0.05));
    }
}

TEST_CASE ("mix blends the band gain linearly (parallel compression)", "[dsp][mb][static]")
{
    auto b = comp (-20.0f, 4.0f, MbDetector::Rms);
    b.mixPercent = 50.0f;
    auto mb = makeMb (oneBand (1, b));
    const double g = std::pow (10.0, -0.75 * (20.0 * std::log10 (0.5 / std::sqrt (2.0)) + 20.0) / 20.0);
    CHECK_THAT (steadyGainDb (mb, 1, 0.5), WithinAbs (20.0 * std::log10 (0.5 + 0.5 * g), 0.05));
}

// ------------------------------------------------------------------- ballistics

TEST_CASE ("attack and release time constants match the settings", "[dsp][mb][ballistics]")
{
    for (const double fs : { 44100.0, 48000.0, 192000.0 })
    {
        auto b = comp (-20.0f, 4.0f, MbDetector::Peak);
        b.attackMs = 10.0f;
        b.releaseMs = 100.0f;
        auto mb = makeMb (oneBand (1, b), fs);

        // DC steps keep the detector level exact: -40 dB (no reduction) -> -6 dB -> -40 dB.
        const int up = ms (200, fs), down = up + ms (500, fs);
        const float target = gainComputerDb (b, 20.0f * std::log10 (0.5f));
        int attackHit = -1, releaseHit = -1;
        for (int i = 0; i < down + ms (1000, fs); ++i)
        {
            StereoBands x;
            x.l[1] = x.r[1] = (i >= up && i < down) ? 0.5f : 0.01f;
            process (mb, x, nullptr);
            const float g = bandGainDb (mb, 1);
            if (attackHit < 0 && i >= up && g <= target * (1.0f - std::exp (-1.0f)))
                attackHit = i - up;
            if (releaseHit < 0 && i >= down && g >= target * std::exp (-1.0f))
                releaseHit = i - down;
        }
        INFO ("fs " << fs << " attack hit " << attackHit << " release hit " << releaseHit);
        CHECK (std::abs (attackHit - ms (10, fs)) <= ms (10, fs) / 20 + 1);    // 5 %
        CHECK (std::abs (releaseHit - ms (100, fs)) <= ms (100, fs) / 20 + 1); // DC: crest 0 dB -> nominal
    }
}

TEST_CASE ("RMS detector window is 10 ms: half power after half a window, full after one", "[dsp][mb][ballistics]")
{
    for (const double fs : { 44100.0, 48000.0, 192000.0 })
    {
        auto b = comp (-40.0f, 4.0f, MbDetector::Rms);
        b.attackMs = 0.1f;
        auto mb = makeMb (oneBand (1, b), fs);
        const int window = ms (10, fs), start = ms (100, fs);
        float atHalf = 0.0f, atFull = 0.0f;
        for (int i = 0; i < start + 2 * window; ++i)
        {
            StereoBands x;
            x.l[1] = x.r[1] = i >= start ? 0.5f : 0.0f;   // DC step: mean square ramps linearly
            process (mb, x, nullptr);
            if (i == start + window / 2 + ms (0.5, fs))   // + 5 attack time constants
                atHalf = bandGainDb (mb, 1);
            if (i == start + window + ms (0.5, fs))
                atFull = bandGainDb (mb, 1);
        }
        const float full = gainComputerDb (b, 20.0f * std::log10 (0.5f));
        const float half = gainComputerDb (b, 20.0f * std::log10 (0.5f) + 10.0f * std::log10 (0.5f + 0.05f));
        INFO ("fs " << fs << " half " << atHalf << " expect ~" << half << " full " << atFull << " expect " << full);
        CHECK (std::abs (atFull - full) < 0.01f);
        CHECK (std::abs (atHalf - half) < 0.3f);
    }
}

TEST_CASE ("crest meter reads ~3 dB for a sine and > 12 dB for sparse pulses", "[dsp][mb][release]")
{
    auto mb = makeMb (oneBand (1, comp (-20.0f, 4.0f)));
    const double w = 2.0 * std::numbers::pi * 1000.0 / kFs;
    for (int i = 0; i < 48000; ++i)
    {
        StereoBands x;
        x.l[1] = x.r[1] = static_cast<float> (0.3 * std::sin (w * i));
        x.l[2] = x.r[2] = (i % 480 == 0) ? 0.5f : 0.0f;
        process (mb, x, nullptr);
    }
    CHECK_THAT (bandCrestDb (mb, 1), WithinAbs (3.01, 0.3));
    CHECK (bandCrestDb (mb, 2) > 20.0f);
}

TEST_CASE ("high-crest material releases faster than low-crest material", "[dsp][mb][release]")
{
    // Same loud/quiet envelope, two textures: dense sine vs sparse pulses at the same peak.
    auto releaseTime = [] (bool pulses)
    {
        auto b = comp (-30.0f, 8.0f, MbDetector::Peak);
        b.attackMs = 0.1f;
        b.releaseMs = 400.0f;
        auto mb = makeMb (oneBand (1, b));
        const double w = 2.0 * std::numbers::pi * 1000.0 / kFs;
        const int drop = 48000;
        float atDrop = 0.0f;
        for (int i = 0; i < drop + 48000; ++i)
        {
            const double amp = i < drop ? 0.5 : 0.0;
            StereoBands x;
            x.l[1] = x.r[1] = pulses ? ((i % 480 == 0) ? static_cast<float> (amp) : 0.0f)
                                     : static_cast<float> (amp * std::sin (w * i));
            process (mb, x, nullptr);
            if (i == drop)
                atDrop = bandGainDb (mb, 1);
            if (i > drop && bandGainDb (mb, 1) >= atDrop * std::exp (-1.0f))
                return i - drop;
        }
        return 1 << 30;
    };
    const int dense = releaseTime (false), sparse = releaseTime (true);
    INFO ("63% recovery: dense " << dense << " samples, sparse " << sparse);
    CHECK (sparse * 2 < dense);
}

// ------------------------------------------------------------------- bands, link, sidechain

TEST_CASE ("bands are independent: untouched bands stay bit-exact", "[dsp][mb][bands]")
{
    auto mb = makeMb (oneBand (1, comp (-40.0f, 10.0f)));
    Noise noise;
    for (int i = 0; i < 24000; ++i)
    {
        StereoBands in;
        for (size_t b = 0; b < 4; ++b) { in.l[b] = noise.next(); in.r[b] = noise.next(); }
        auto out = in;
        process (mb, out, nullptr);
        for (const size_t b : { 0u, 2u, 3u })
        {
            REQUIRE (out.l[b] == in.l[b]);
            REQUIRE (out.r[b] == in.r[b]);
        }
    }
    CHECK (bandGainDb (mb, 1) < -10.0f);
}

TEST_CASE ("stereo-linked: both channels of a band get the same gain", "[dsp][mb][link]")
{
    auto mb = makeMb (oneBand (1, comp (-20.0f, 4.0f)));
    const double w = 2.0 * std::numbers::pi * 1000.0 / kFs;
    double worst = 0.0;
    for (int i = 0; i < 48000; ++i)
    {
        StereoBands x;
        x.l[1] = static_cast<float> (0.5 * std::sin (w * i));
        x.r[1] = static_cast<float> (0.05 * std::cos (w * i));   // quiet, different phase
        const auto in = x;
        process (mb, x, nullptr);
        if (std::abs (in.l[1]) > 0.1f && std::abs (in.r[1]) > 0.01f)
            worst = std::max (worst, std::abs (static_cast<double> (x.l[1] / in.l[1]) - static_cast<double> (x.r[1] / in.r[1])));
    }
    CHECK (worst < 1e-6);
    CHECK (bandGainDb (mb, 1) < -6.0f);   // driven by the loud channel
}

TEST_CASE ("external sidechain drives the gain; internal mode ignores it", "[dsp][mb][sidechain]")
{
    const double w = 2.0 * std::numbers::pi * 1000.0 / kFs;
    auto runWith = [&] (MbSidechain source, bool passSidechain)
    {
        auto s = oneBand (1, comp (-20.0f, 4.0f));
        s.sidechain = source;
        auto mb = makeMb (s);
        for (int i = 0; i < 48000; ++i)
        {
            StereoBands x, sc;
            x.l[1] = x.r[1] = static_cast<float> (0.01 * std::sin (w * i));   // -40 dB: below threshold
            sc.l[1] = sc.r[1] = static_cast<float> (0.5 * std::sin (w * i));
            process (mb, x, passSidechain ? &sc : nullptr);
        }
        return bandGainDb (mb, 1);
    };
    CHECK_THAT (runWith (MbSidechain::External, true), WithinAbs (-0.75 * (20.0 * std::log10 (0.5 / std::sqrt (2.0)) + 20.0), 0.05));
    CHECK (runWith (MbSidechain::Internal, true) == 0.0f);
    CHECK (runWith (MbSidechain::External, false) == 0.0f);   // bus disabled: falls back to internal
}

TEST_CASE ("sidechain high-pass removes lows from the detector only", "[dsp][mb][sidechain]")
{
    const double w = 2.0 * std::numbers::pi * 50.0 / kFs;
    auto reduction = [&] (bool hpOn)
    {
        auto s = oneBand (0, comp (-20.0f, 4.0f));
        s.sidechainHpOn = hpOn;
        s.sidechainHpHz = 200.0f;
        auto mb = makeMb (s);
        double worstRatio = 0.0;
        for (int i = 0; i < 48000; ++i)
        {
            StereoBands x;
            x.l[0] = x.r[0] = static_cast<float> (0.5 * std::sin (w * i));
            const float in = x.l[0];
            process (mb, x, nullptr);
            if (std::abs (in) > 0.1f)
                worstRatio = std::max (worstRatio, std::abs (static_cast<double> (x.l[0] / in) - std::pow (10.0, static_cast<double> (bandGainDb (mb, 0)) / 20.0)));
        }
        CHECK (worstRatio < 1e-5);   // the audio path never sees the filter
        return bandGainDb (mb, 0);
    };
    CHECK (reduction (false) < -8.0f);
    CHECK (reduction (true) > -0.5f);   // 50 Hz is 24 dB down at the detector: below threshold
}

TEST_CASE ("sidechain split: same crossover, delayed to the band-domain latency", "[dsp][mb][sidechain]")
{
    SidechainSplit sc;
    prepare (sc, kDefaultCrossovers, kFs, 4096);
    setDelay (sc, 300);
    std::vector<float> ir (65536);
    for (size_t n = 0; n < ir.size(); ++n)
    {
        const auto b = process (sc, n == 0 ? 1.0f : 0.0f, 0.0f);
        float l, r;
        merge (b, l, r);
        ir[n] = l;
        REQUIRE (r == 0.0f);
        if (n < 300)
            REQUIRE (l == 0.0f);
    }
    for (const double f : { 50.0, 500.0, 3000.0, 12000.0 })
    {
        INFO ("f = " << f);
        CHECK (std::abs (magnitudeDb (ir, f, kFs)) < 0.01);
    }
}

// ------------------------------------------------------------------- integration

TEST_CASE ("MB in the FIR band domain: neutral layer keeps the band-domain null", "[dsp][mb][band]")
{
    BandDomain domain;
    prepare (domain, SplitMode::Fir, kDefaultCrossovers, kFs, 256);
    const int latency = latencySamples (domain);
    auto mb = makeMb (MbSettings {});
    DelayLine refL;
    prepare (refL, static_cast<size_t> (latency));
    setDelay (refL, static_cast<size_t> (latency));
    Noise noise;
    double maxErr = 0.0;
    for (int i = 0; i < latency + 20000; ++i)
    {
        const float l = noise.next(), r = noise.next();
        auto bands = split (domain, l, r);
        process (mb, bands, nullptr);
        float outL, outR;
        merge (bands, outL, outR);
        const float ref = process (refL, l);
        if (i >= latency)
            maxErr = std::max (maxErr, static_cast<double> (std::abs (outL - ref)));
    }
    CHECK (maxErr < 2e-5);
}

TEST_CASE ("MB in the IIR band domain compresses loud noise and stays finite", "[dsp][mb][band]")
{
    BandDomain domain;
    prepare (domain, SplitMode::Iir, kDefaultCrossovers, kFs, 256);
    MbSettings s;
    for (auto& b : s.bands)
        b = comp (-30.0f, 4.0f);
    auto mb = makeMb (s);
    Noise noise;
    double inPow = 0.0, outPow = 0.0;
    for (int i = 0; i < 96000; ++i)
    {
        const float l = noise.next(), r = noise.next();
        auto bands = split (domain, l, r);
        process (mb, bands, nullptr);
        float outL, outR;
        merge (bands, outL, outR);
        REQUIRE (std::isfinite (outL));
        if (i >= 48000)
        {
            inPow += static_cast<double> (l) * static_cast<double> (l);
            outPow += static_cast<double> (outL) * static_cast<double> (outL);
        }
    }
    CHECK (10.0 * std::log10 (outPow / inPow) < -6.0);
}

// ------------------------------------------------------------------- robustness

TEST_CASE ("MB is stable under random automation at the extremes", "[dsp][mb][stability]")
{
    auto mb = makeMb (MbSettings {});
    Noise noise, automation;
    bool finite = true, gainsInRange = true;
    float inPeak = 0.0f, outPeak = 0.0f;
    for (int i = 0; i < 48000 * 10; ++i)
    {
        auto u = [&] { return 0.5f * (automation.next() + 1.0f); };
        if (i % 64 == 0)
        {
            MbSettings s;
            for (auto& b : s.bands)
            {
                b.threshDb = -60.0f * u();
                b.ratio = 1.0f + 19.0f * u();
                b.dir = u() > 0.5f ? MbDir::Up : MbDir::Down;
                b.kneeDb = 24.0f * u();
                b.attackMs = 0.1f + 99.9f * u();
                b.releaseMs = 5.0f + 995.0f * u();
                b.detector = static_cast<MbDetector> (static_cast<int> (u() * 2.999f));
                b.mixPercent = 100.0f * u();
            }
            s.sidechain = u() > 0.5f ? MbSidechain::External : MbSidechain::Internal;
            s.sidechainHpOn = u() > 0.5f;
            s.sidechainHpHz = 20.0f + 480.0f * u();
            setSettings (mb, s);
        }
        StereoBands x, sc;
        for (size_t b = 0; b < 4; ++b)
        {
            x.l[b] = noise.next(); x.r[b] = noise.next();
            sc.l[b] = noise.next(); sc.r[b] = noise.next();
            inPeak = std::max ({ inPeak, std::abs (x.l[b]), std::abs (x.r[b]) });
        }
        process (mb, x, &sc);
        for (size_t b = 0; b < 4; ++b)
        {
            finite &= std::isfinite (x.l[b]) && std::isfinite (x.r[b]);
            outPeak = std::max ({ outPeak, std::abs (x.l[b]), std::abs (x.r[b]) });
            const float g = bandGainDb (mb, static_cast<int> (b));
            gainsInRange &= g <= kMbUpwardMaxDb + 1e-4f && g > -200.0f;
        }
    }
    INFO ("in peak " << inPeak << " out peak " << outPeak);
    CHECK (finite);
    CHECK (gainsInRange);
    CHECK (outPeak <= inPeak * std::pow (10.0f, kMbUpwardMaxDb / 20.0f) * 1.001f);
}

TEST_CASE ("MB ignores non-finite settings and recovers from non-finite input", "[dsp][mb][nan]")
{
    auto mb = makeMb (oneBand (1, comp (-20.0f, 4.0f)));
    MbSettings bad = mb.settings;
    bad.bands[1].threshDb = std::nanf ("");
    bad.bands[1].ratio = INFINITY;
    bad.bands[1].attackMs = std::nanf ("");
    bad.sidechainHpHz = -INFINITY;
    setSettings (mb, bad);
    CHECK (mb.settings.bands[1].threshDb == -20.0f);
    CHECK (mb.settings.bands[1].ratio == 4.0f);
    CHECK (mb.settings.bands[1].attackMs == 10.0f);
    CHECK (mb.settings.sidechainHpHz == 80.0f);

    StereoBands x;
    x.l[1] = std::nanf ("");
    process (mb, x, nullptr);
    CHECK (resetIfNotFinite (mb));
    CHECK_FALSE (resetIfNotFinite (mb));
    for (int i = 0; i < 2000; ++i)
    {
        StereoBands z;
        process (mb, z, nullptr);
        for (size_t b = 0; b < 4; ++b)
            REQUIRE ((z.l[b] == 0.0f && z.r[b] == 0.0f));
    }
    CHECK (bandGainDb (mb, 1) == 0.0f);
}

TEST_CASE ("MB settings are clamped to the parameter ranges", "[dsp][mb]")
{
    MultibandLayer mb;
    prepare (mb, kFs);
    MbSettings s;
    s.bands[0].threshDb = -100.0f;
    s.bands[0].ratio = 0.5f;
    s.bands[0].kneeDb = -1.0f;
    s.bands[0].attackMs = 0.0f;
    s.bands[0].releaseMs = 2000.0f;
    s.bands[0].mixPercent = 150.0f;
    s.bands[1].threshDb = 10.0f;
    s.bands[1].ratio = 50.0f;
    s.bands[1].kneeDb = 30.0f;
    s.sidechainHpHz = 5.0f;
    setSettings (mb, s);
    const auto& b0 = mb.settings.bands[0];
    const auto& b1 = mb.settings.bands[1];
    CHECK (b0.threshDb == -60.0f);
    CHECK (b0.ratio == 1.0f);
    CHECK (b0.kneeDb == 0.0f);
    CHECK (b0.attackMs == 0.1f);
    CHECK (b0.releaseMs == 1000.0f);
    CHECK (b0.mixPercent == 100.0f);
    CHECK (b1.threshDb == 0.0f);
    CHECK (b1.ratio == 20.0f);
    CHECK (b1.kneeDb == 24.0f);
    CHECK (mb.settings.sidechainHpHz == 20.0f);
}

TEST_CASE ("MB leaves no subnormal state after loud input then long silence", "[dsp][mb][denormal]")
{
    MbSettings s;
    for (auto& b : s.bands)
        b = comp (-40.0f, 4.0f, MbDetector::Envelope);
    s.bands[3].dir = MbDir::Up;
    s.sidechainHpOn = true;
    auto mb = makeMb (s);
    Noise noise;
    for (int i = 0; i < 48000; ++i)
    {
        StereoBands x;
        for (size_t b = 0; b < 4; ++b) { x.l[b] = noise.next(); x.r[b] = noise.next(); }
        process (mb, x, nullptr);
    }
    for (int block = 0; block < 48000 * 30 / 256; ++block)   // 30 s: past where a bare release multiply goes subnormal
    {
        for (int i = 0; i < 256; ++i)
        {
            StereoBands z;
            process (mb, z, nullptr);
        }
        flushDenormals (mb);
    }
    bool subnormal = false;
    forEachFloatState (mb, [&] (float v) { subnormal |= isSubnormal (v); });
    CHECK_FALSE (subnormal);
}
