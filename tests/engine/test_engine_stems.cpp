// STEMS in the chain (module 22b): the layer at the head of the chain, after IN TRIM.
//   IN TRIM -> STEMS -> the six depth layers -> OUT TRIM -> monitor -> CEILING
// Until the STEMS row exists in the impact map (22c), STEMS shares the IN TRIM row: its
// latency and its effect are counted there. Contracts tested here:
//   - MIX and MASTER: STEMS adds its 1024 samples to the IN TRIM row whether it is on or off,
//     so the host's delay compensation never moves; TRACK has no STEMS and no latency for it
//   - off, or without a cache, STEMS only delays
//   - with a cache and the host playing, the stem gains apply; depth blends against the input
//     delayed by the layer (no comb); TRACK leaves the audio alone
//   - delta: STEMS is the topmost layer, so its delta wins; global delta includes it; both
//     are time-aligned with the normal output. TRACK ignores the STEMS delta
//   - the capture takes the STEMS input (after IN TRIM) with the host's timeline position
//   - the sidechain still meets MB on time with STEMS ahead of it (tested in test_engine.cpp)
//   - a block longer than the prepared size is processed in pieces with the right positions
//   - nothing allocates
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <numbers>
#include <vector>

#include "dsp/stems/Fingerprint.h"
#include "dsp/stems/MaskCache.h"
#include "engine/Engine.h"
#include "support/AllocCounter.h"
#include "support/Analysis.h"

using namespace strat3::engine;
namespace dsp = strat3::dsp;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double kFs = 48000.0;
constexpr int kBlock = 512;
constexpr LatencyMode kModes[] { LatencyMode::Track, LatencyMode::Mix, LatencyMode::Master };

EngineParams neutral()
{
    EngineParams p;
    for (auto& s : p.layer)
        s = { true, 100.0f, false };
    p.ceilingOn = true;
    p.ceiling.ceilingDb = -1.0f;
    p.stems = { true, 100.0f, false };
    return p;
}

std::unique_ptr<Engine> make (LatencyMode mode, const EngineParams& p, int maxBlock = kBlock)
{
    auto e = std::make_unique<Engine>();
    EngineConfig c;
    c.sampleRate = kFs;
    c.maxBlock = maxBlock;
    c.mode = mode;
    prepare (*e, c, p);
    return e;
}

int samples (double seconds) { return static_cast<int> (seconds * kFs); }

/** Independent white noise per channel, quiet enough that LIMIT and CEILING never act. */
void noise (std::vector<float>& l, std::vector<float>& r, int n, float amp = 0.05f)
{
    strat3::test::WhiteNoise a { 11 }, b { 977 };
    l.resize (static_cast<size_t> (n));
    r.resize (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
    {
        l[static_cast<size_t> (i)] = amp * a.next();
        r[static_cast<size_t> (i)] = amp * b.next();
    }
}

/** A cache that calls every bin of every frame `stem`, with the prints of what is played. */
dsp::MaskCache wholeMixIs (int stem, const std::vector<float>& l, const std::vector<float>& r, std::int64_t start)
{
    dsp::MaskCache c;
    const int n = static_cast<int> (l.size());
    c.sampleRate = kFs;
    c.startSample = start;
    c.frames = dsp::maskFramesFor (n);
    c.masks.assign (static_cast<size_t> (c.frames) * dsp::kStems * dsp::kMaskBins, 0);
    for (int f = 0; f < c.frames; ++f)
        for (int k = 0; k < dsp::kMaskBins; ++k)
            c.masks[dsp::maskIndex (f, stem, k)] = dsp::kMaskFull;
    c.prints = dsp::fingerprints (l.data(), r.data(), n, kFs);
    return c;
}

/** Runs l/r through the engine in blocks while the host plays from `start`. */
void play (Engine& e, std::vector<float>& l, std::vector<float>& r, std::int64_t start, int block = kBlock)
{
    const int n = static_cast<int> (l.size());
    for (int i = 0; i < n; i += block)
    {
        const int len = std::min (block, n - i);
        process (e, l.data() + i, r.data() + i, len, nullptr, nullptr, dsp::Transport { true, true, start + i });
    }
}

/** Output against input in dB over input samples [from, to), the output read `lat` later. */
double gainDb (const std::vector<float>& inL, const std::vector<float>& inR,
               const std::vector<float>& outL, const std::vector<float>& outR, int from, int to, int lat)
{
    double ei = 0.0, eo = 0.0;
    for (int n = from; n < to; ++n)
    {
        const auto i = static_cast<size_t> (n), o = static_cast<size_t> (n + lat);
        const double il = inL[i], ir = inR[i], ol = outL[o], orr = outR[o];
        ei += il * il + ir * ir;
        eo += ol * ol + orr * orr;
    }
    return 10.0 * std::log10 (eo / ei);
}

/** The gain of the chain with the bass stem at -12 dB on a mix the cache calls all bass. */
struct Measured
{
    double db = 0.0;
    std::vector<float> inL, inR, outL, outR;
    int lat = 0;
};

Measured bassDown (LatencyMode mode, EngineParams p, double seconds = 2.0)
{
    Measured m;
    noise (m.inL, m.inR, samples (seconds));
    const auto cache = wholeMixIs (dsp::kStemBass, m.inL, m.inR, 0);
    p.stemsSettings.gainDb[dsp::kStemBass] = -12.0f;
    auto e = make (mode, p);
    setStemsCache (*e, &cache);
    m.outL = m.inL;
    m.outR = m.inR;
    play (*e, m.outL, m.outR, 0);
    m.lat = latencySamples (*e);
    m.db = gainDb (m.inL, m.inR, m.outL, m.outR, samples (0.5), samples (seconds) - m.lat, m.lat);
    return m;
}

} // namespace

// ------------------------------------------------------------------- latency

TEST_CASE ("engine stems: MIX and MASTER add 1024 samples to the IN TRIM row, on or off; TRACK adds none", "[engine][stems][latency]")
{
    for (auto mode : kModes)
    {
        INFO ("mode " << static_cast<int> (mode));
        const int expected = mode == LatencyMode::Track ? 0 : dsp::kStemsFft;
        CHECK (stemsInChain (mode) == (mode != LatencyMode::Track));
        auto on = neutral(), off = neutral();
        off.stems.on = false;
        auto eOn = make (mode, on), eOff = make (mode, off);
        CHECK (rowLatencies (*eOn)[kRowInTrim] == expected);
        CHECK (rowLatencies (*eOff)[kRowInTrim] == expected);
        CHECK (latencySamples (*eOn) == latencySamples (*eOff));
        on.stems = { false, 0.0f, true };
        setParams (*eOn, on);
        CHECK (latencySamples (*eOn) == latencySamples (*eOff));
    }
}

TEST_CASE ("engine stems: off, or on without a cache, STEMS only delays", "[engine][stems]")
{
    for (auto mode : { LatencyMode::Mix, LatencyMode::Master })
    {
        INFO ("mode " << static_cast<int> (mode));
        auto off = neutral();
        off.stems.on = false;
        auto a = make (mode, off), b = make (mode, neutral());
        std::vector<float> la, ra;
        noise (la, ra, samples (1.0));
        auto lb = la, rb = ra;
        play (*a, la, ra, 0);
        play (*b, lb, rb, 0);
        float d = 0.0f;
        for (size_t i = 0; i < la.size(); ++i)
            d = std::max ({ d, std::abs (la[i] - lb[i]), std::abs (ra[i] - rb[i]) });
        CHECK (d < 1e-5f);
        CHECK (stemsStatus (*b) == dsp::StemsStatus::NoCache);

        // Off with a cache: still only the delay (the cache is not applied).
        std::vector<float> lc, rc;
        noise (lc, rc, samples (1.0));
        const auto cache = wholeMixIs (dsp::kStemBass, lc, rc, 0);
        off.stemsSettings.gainDb[dsp::kStemBass] = -12.0f;
        auto c = make (mode, off), ref = make (mode, off);
        setStemsCache (*c, &cache);
        auto lr = lc, rr = rc;
        play (*c, lc, rc, 0);
        play (*ref, lr, rr, 0);
        CHECK (lc == lr);
        CHECK (rc == rr);
    }
}

// ------------------------------------------------------------------- stems, depth, TRACK

TEST_CASE ("engine stems: with a cache and the host playing, a stem's gain applies (MIX and MASTER)", "[engine][stems]")
{
    for (auto mode : { LatencyMode::Mix, LatencyMode::Master })
    {
        INFO ("mode " << static_cast<int> (mode));
        CHECK_THAT (bassDown (mode, neutral()).db, WithinAbs (-12.0, 0.3));
        // The prints ignore level: a trim ahead of STEMS is still the analysed mix.
        auto p = neutral();
        p.inTrimDb = -6.0f;
        CHECK_THAT (bassDown (mode, p).db, WithinAbs (-18.0, 0.3));
    }
}

TEST_CASE ("engine stems: TRACK leaves the audio alone", "[engine][stems]")
{
    CHECK_THAT (bassDown (LatencyMode::Track, neutral()).db, WithinAbs (0.0, 0.1));
}

TEST_CASE ("engine stems: depth blends against the input delayed by the layer", "[engine][stems][depth]")
{
    auto p = neutral();
    p.stems.depth = 50.0f;
    // 0.5 + 0.5 * -12 dB: a misaligned dry path would comb instead of landing here.
    const double expected = 20.0 * std::log10 (0.5 + 0.5 * std::pow (10.0, -12.0 / 20.0));
    CHECK_THAT (bassDown (LatencyMode::Mix, p).db, WithinAbs (expected, 0.3));
    p.stems.depth = 0.0f;
    CHECK_THAT (bassDown (LatencyMode::Mix, p).db, WithinAbs (0.0, 0.1));
}

// ------------------------------------------------------------------- delta

TEST_CASE ("engine stems: the STEMS delta is what STEMS adds, time-aligned, and wins over the layers below", "[engine][stems][delta]")
{
    auto p = neutral();
    p.stems.delta = true;
    p.layer[kEq].delta = true;   // below STEMS: not heard
    p.eq[0].on = true;
    p.eq[0].type = dsp::EqType::Bell;
    p.eq[0].freq = 1000.0f;
    p.eq[0].gainDb = 6.0f;
    p.eq[0].q = 0.7f;
    for (auto mode : { LatencyMode::Mix, LatencyMode::Master })
    {
        INFO ("mode " << static_cast<int> (mode));
        const auto m = bassDown (mode, p);
        // The delta passes no other layer: it is the input times (-12 dB - 1), at the latency.
        const float k = std::pow (10.0f, -12.0f / 20.0f) - 1.0f;
        float worst = 0.0f;
        for (int n = samples (0.5); n < samples (2.0) - m.lat; ++n)
        {
            const auto i = static_cast<size_t> (n), o = static_cast<size_t> (n + m.lat);
            worst = std::max ({ worst, std::abs (m.outL[o] - k * m.inL[i]), std::abs (m.outR[o] - k * m.inR[i]) });
        }
        CHECK (worst < 1e-4f);
    }
}

TEST_CASE ("engine stems: global delta includes what STEMS adds", "[engine][stems][delta]")
{
    auto p = neutral();
    p.globalDelta = true;
    // The six neutral layers add (almost) nothing, so the sum is STEMS's addition.
    const double expected = 20.0 * std::log10 (1.0 - std::pow (10.0, -12.0 / 20.0));
    CHECK_THAT (bassDown (LatencyMode::Mix, p).db, WithinAbs (expected, 0.3));
}

TEST_CASE ("engine stems: TRACK ignores the STEMS delta", "[engine][stems][delta]")
{
    auto p = neutral();
    p.stems.delta = true;
    CHECK_THAT (bassDown (LatencyMode::Track, p).db, WithinAbs (0.0, 0.1));
}

// ------------------------------------------------------------------- cache and capture

TEST_CASE ("engine stems: a cache handed over is used from the next block, and can be taken away", "[engine][stems]")
{
    std::vector<float> l, r;
    noise (l, r, samples (1.0));
    const auto cache = wholeMixIs (dsp::kStemBass, l, r, 0);
    auto e = make (LatencyMode::Mix, neutral());
    CHECK (stemsStatus (*e) == dsp::StemsStatus::NoCache);
    setStemsCache (*e, &cache);
    // Half the stretch: past its last whole frame the layer waits (bypassed) by design.
    std::vector<float> pl (l.begin(), l.begin() + samples (0.5)), pr (r.begin(), r.begin() + samples (0.5));
    play (*e, pl, pr, 0);
    CHECK (stemsStatus (*e) == dsp::StemsStatus::Active);
    setStemsCache (*e, nullptr);
    CHECK (stemsStatus (*e) == dsp::StemsStatus::NoCache);
}

TEST_CASE ("engine stems: the capture takes the STEMS input, after IN TRIM, with the host's position", "[engine][stems][capture]")
{
    auto p = neutral();
    p.inTrimDb = -6.0f;
    std::vector<float> l, r;
    noise (l, r, samples (0.5));

    dsp::StemsCapture capture;
    dsp::prepare (capture, samples (1.0));
    REQUIRE (dsp::arm (capture));
    auto e = make (LatencyMode::Mix, p);
    setStemsCapture (*e, &capture);
    auto pl = l, pr = r;
    play (*e, pl, pr, 48000, 300);

    CHECK (dsp::state (capture) == dsp::CaptureState::Running);
    CHECK (dsp::startSample (capture) == 48000);
    REQUIRE (dsp::available (capture) == static_cast<std::int64_t> (l.size()));
    std::vector<float> cl (l.size()), cr (r.size());
    dsp::read (capture, cl.data(), cr.data(), static_cast<int> (l.size()));
    const float g = std::pow (10.0f, -6.0f / 20.0f);
    float worst = 0.0f;
    for (size_t i = 0; i < l.size(); ++i)
        worst = std::max ({ worst, std::abs (cl[i] - g * l[i]), std::abs (cr[i] - g * r[i]) });
    CHECK (worst < 1e-6f);

    // TRACK has no STEMS: nothing to analyse.
    dsp::StemsCapture none;
    dsp::prepare (none, samples (1.0));
    REQUIRE (dsp::arm (none));
    auto t = make (LatencyMode::Track, p);
    setStemsCapture (*t, &none);
    pl = l;
    pr = r;
    play (*t, pl, pr, 0);
    CHECK (dsp::state (none) == dsp::CaptureState::Armed);
    CHECK (dsp::available (none) == 0);
}

// ------------------------------------------------------------------- blocks and allocation

TEST_CASE ("engine stems: a block longer than the prepared size is processed in pieces, positions intact", "[engine][stems][blocks]")
{
    std::vector<float> l, r;
    noise (l, r, samples (1.5));
    const auto cache = wholeMixIs (dsp::kStemBass, l, r, 0);
    auto p = neutral();
    p.stemsSettings.gainDb[dsp::kStemBass] = -12.0f;
    auto a = make (LatencyMode::Mix, p), b = make (LatencyMode::Mix, p);
    setStemsCache (*a, &cache);
    setStemsCache (*b, &cache);
    // The cache is bass everywhere, so a wrong position would still sound right: the capture
    // is what checks each piece's position (a piece at the wrong one ends it as a jump).
    dsp::StemsCapture capture;
    dsp::prepare (capture, samples (2.0));
    REQUIRE (dsp::arm (capture));
    setStemsCapture (*b, &capture);
    auto la = l, ra = r, lb = l, rb = r;
    play (*a, la, ra, 0, kBlock);
    play (*b, lb, rb, 0, 2000);   // 2000 > maxBlock 512
    CHECK (dsp::state (capture) == dsp::CaptureState::Running);
    CHECK (dsp::available (capture) == static_cast<std::int64_t> (l.size()));
    float d = 0.0f;
    for (size_t i = 0; i < la.size(); ++i)
        d = std::max ({ d, std::abs (la[i] - lb[i]), std::abs (ra[i] - rb[i]) });
    CHECK (d < 1e-6f);
    // ... and STEMS did apply (two bypassed chains would match too).
    CHECK_THAT (gainDb (l, r, la, ra, samples (0.5), samples (1.0), latencySamples (*a)), WithinAbs (-12.0, 0.3));
}

TEST_CASE ("engine stems: processing with STEMS, a cache and a capture never allocates", "[engine][stems][alloc]")
{
    std::vector<float> l, r;
    noise (l, r, samples (1.0));
    const auto cache = wholeMixIs (dsp::kStemBass, l, r, 0);
    dsp::StemsCapture capture;
    dsp::prepare (capture, samples (2.0));
    REQUIRE (dsp::arm (capture));
    auto p = neutral();
    p.stemsSettings.gainDb[dsp::kStemBass] = -12.0f;
    for (auto mode : kModes)
    {
        auto e = make (mode, p);
        setStemsCapture (*e, &capture);
        auto pl = l, pr = r;
        std::size_t allocs = 0;
        {
            const strat3::test::ScopedAllocCounter count;
            setStemsCache (*e, &cache);
            for (int i = 0; i + kBlock <= static_cast<int> (pl.size()); i += kBlock)
            {
                p.stems.delta = (i / kBlock) % 2 == 1;
                setParams (*e, p);
                process (*e, pl.data() + i, pr.data() + i, kBlock, nullptr, nullptr, dsp::Transport { true, true, i });
            }
            allocs = count.count();
        }
        INFO ("mode " << static_cast<int> (mode));
        CHECK (allocs == 0);
        dsp::end (capture, dsp::CaptureEnd::Cancelled);
        std::vector<float> sink (static_cast<size_t> (samples (2.0)));
        dsp::read (capture, sink.data(), sink.data(), samples (2.0));
        REQUIRE (dsp::reset (capture));
        REQUIRE (dsp::arm (capture));
    }
}
