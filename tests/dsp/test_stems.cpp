// STEMS, the DSP (module 22a): a model-free separator that writes 4 masks per bin and frame
// (vocals, bass, drums, other), the mask cache, the block fingerprint, and the layer that
// applies the masks at playback. Contracts tested here:
//   - the 4 masks of every bin and frame sum to exactly one (255 of 255): with every stem at
//     0 dB the layer passes the mix through unchanged, only delayed
//   - the separator: a sustained centred tone is vocals, the same tone panned hard is other,
//     a low centred sine is bass, clicks are drums, silence is other
//   - frames are found by timeline position, rounded to the nearest frame
//   - the fingerprint ignores level (a trim change is not a new mix) but not the spectrum
//   - the layer: latency 1024 samples; stem gains and tilts per region of the cache; a seek
//     looks up the new position; it fades in and out over 50 ms; it bypasses while the host
//     is stopped, has no position, plays outside the cache, or runs at another sample rate;
//     audio that no longer matches the fingerprints is stale (bypassed) until it matches again
//   - no allocation while processing; a non-finite input is recovered
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <vector>

#include "dsp/layers/StemsLayer.h"
#include "dsp/stems/Fingerprint.h"
#include "dsp/stems/MaskCache.h"
#include "dsp/stems/Separator.h"
#include "support/AllocCounter.h"
#include "support/Analysis.h"

using namespace strat3::dsp;
using namespace strat3::test;

namespace {

constexpr double kFs = 48000.0;

struct Stereo
{
    std::vector<float> l, r;
    int size() const { return static_cast<int> (l.size()); }
};

int samples (double seconds) { return static_cast<int> (seconds * kFs); }

Stereo silence (double seconds) { return { std::vector<float> (static_cast<size_t> (samples (seconds))), std::vector<float> (static_cast<size_t> (samples (seconds))) }; }

/** A sustained tone of `harmonics` harmonics (amplitude 1/k), with a gain per channel. */
Stereo tone (double seconds, double f0, int harmonics, float gainL, float gainR)
{
    auto s = silence (seconds);
    for (int n = 0; n < s.size(); ++n)
    {
        double v = 0.0;
        for (int k = 1; k <= harmonics; ++k)
            v += std::sin (2.0 * std::numbers::pi * f0 * k * n / kFs) / k;
        s.l[static_cast<size_t> (n)] = gainL * 0.2f * static_cast<float> (v);
        s.r[static_cast<size_t> (n)] = gainR * 0.2f * static_cast<float> (v);
    }
    return s;
}

Stereo sine (double seconds, double hz, float amp)
{
    return tone (seconds, hz, 1, amp / 0.2f, amp / 0.2f);
}

/** One-sample clicks, the same in both channels, every `period` seconds from 0.1 s. */
Stereo clicks (double seconds, double period)
{
    auto s = silence (seconds);
    for (double t = 0.1; t < seconds; t += period)
    {
        const auto n = static_cast<size_t> (samples (t));
        s.l[n] = s.r[n] = 0.9f;
    }
    return s;
}

/** Independent white noise per channel. */
Stereo noise (double seconds, std::uint64_t seed = 1, float amp = 0.3f)
{
    auto s = silence (seconds);
    WhiteNoise a { seed * 1000003u }, b { seed * 1000003u + 77u };
    for (int n = 0; n < s.size(); ++n)
    {
        s.l[static_cast<size_t> (n)] = amp * a.next();
        s.r[static_cast<size_t> (n)] = amp * b.next();
    }
    return s;
}

/** White noise through a one-pole low-pass at about 300 Hz: a clearly different spectrum. */
Stereo darkNoise (double seconds, std::uint64_t seed = 9)
{
    auto s = noise (seconds, seed, 1.0f);
    const float a = std::exp (static_cast<float> (-2.0 * std::numbers::pi * 300.0 / kFs));
    float yl = 0.0f, yr = 0.0f;
    for (int n = 0; n < s.size(); ++n)
    {
        yl = (1.0f - a) * s.l[static_cast<size_t> (n)] + a * yl;
        yr = (1.0f - a) * s.r[static_cast<size_t> (n)] + a * yr;
        s.l[static_cast<size_t> (n)] = yl;
        s.r[static_cast<size_t> (n)] = yr;
    }
    return s;
}

Stereo mix (const Stereo& a, const Stereo& b)
{
    auto s = a;
    for (size_t n = 0; n < s.l.size(); ++n)
    {
        s.l[n] += b.l[n];
        s.r[n] += b.r[n];
    }
    return s;
}

Stereo slice (const Stereo& s, int from, int count)
{
    return { std::vector<float> (s.l.begin() + from, s.l.begin() + from + count), std::vector<float> (s.r.begin() + from, s.r.begin() + from + count) };
}

Stereo scaled (Stereo s, float g)
{
    for (auto& v : s.l) v *= g;
    for (auto& v : s.r) v *= g;
    return s;
}

/** `a` where (n / 512) is even, `b` where it is odd: a mix that changes on every print block. */
Stereo alternating (const Stereo& a, const Stereo& b)
{
    auto s = a;
    for (size_t n = 0; n < s.l.size(); ++n)
        if ((n / kPrintBlock) % 2 == 1)
        {
            s.l[n] = b.l[n];
            s.r[n] = b.r[n];
        }
    return s;
}

int binOf (double hz) { return static_cast<int> (std::lround (hz * kMaskFft / kFs)); }

/** Mean of one stem's mask over [binLo, binHi] x [frameLo, frameHi]. */
double meanMask (const MaskCache& c, int stem, int binLo, int binHi, int frameLo, int frameHi)
{
    double sum = 0.0;
    int count = 0;
    for (int f = frameLo; f <= frameHi; ++f)
        for (int k = binLo; k <= binHi; ++k)
        {
            sum += static_cast<double> (maskAt (c, f, stem, k));
            ++count;
        }
    return sum / count;
}

/** A cache whose masks are given by `stemOfFrame`, with the fingerprints of `played`. */
template <typename Fn>
MaskCache handMadeCache (const Stereo& played, std::int64_t start, Fn&& stemOfFrame)
{
    MaskCache c;
    c.sampleRate = kFs;
    c.startSample = start;
    c.frames = maskFramesFor (played.size());
    c.masks.assign (static_cast<size_t> (c.frames) * kStems * kMaskBins, 0);
    for (int f = 0; f < c.frames; ++f)
        for (int k = 0; k < kMaskBins; ++k)
            c.masks[maskIndex (f, stemOfFrame (f), k)] = kMaskFull;
    c.prints = fingerprints (played.l.data(), played.r.data(), played.size(), kFs);
    return c;
}

StemsLayer makeLayer (const MaskCache* cache, const StemsSettings& s = {})
{
    StemsLayer layer;
    prepare (layer, kFs);
    setCache (layer, cache);
    setSettings (layer, s);
    return layer;
}

/** Runs `in` through the layer in blocks of varying size; the timeline starts at `start`. */
Stereo run (StemsLayer& layer, const Stereo& in, std::int64_t start, bool playing = true, bool hasPosition = true)
{
    auto out = in;
    static constexpr int kBlocks[] { 64, 333, 512, 1000, 17, 256, 480 };
    int n = 0, i = 0;
    while (n < out.size())
    {
        const int len = std::min (kBlocks[i++ % 7], out.size() - n);
        process (layer, out.l.data() + n, out.r.data() + n, len, Transport { playing, hasPosition, start + n });
        n += len;
    }
    return out;
}

/** Output level against the input in dB, over input samples [from, to) (output delayed). */
double gainDb (const Stereo& in, const Stereo& out, int from, int to, int latency = kStemsFft)
{
    double ei = 0.0, eo = 0.0;
    for (int n = from; n < to; ++n)
    {
        const auto i = static_cast<size_t> (n), o = static_cast<size_t> (n + latency);
        ei += static_cast<double> (in.l[i] * in.l[i] + in.r[i] * in.r[i]);
        eo += static_cast<double> (out.l[o] * out.l[o] + out.r[o] * out.r[o]);
    }
    return 10.0 * std::log10 (eo / ei);
}

/** Largest |out[n + latency] - in[n]| over [from, to). */
float maxError (const Stereo& in, const Stereo& out, int from, int to, int latency = kStemsFft)
{
    float worst = 0.0f;
    for (int n = from; n < to; ++n)
    {
        const auto i = static_cast<size_t> (n), o = static_cast<size_t> (n + latency);
        worst = std::max ({ worst, std::abs (out.l[o] - in.l[i]), std::abs (out.r[o] - in.r[i]) });
    }
    return worst;
}

} // namespace

// ------------------------------------------------------------------- mask cache

TEST_CASE ("stems: a cache has one mask per stem, bin and frame, and one print per block", "[dsp][stems]")
{
    const auto in = noise (2.0);
    const auto c = separate (in.l.data(), in.r.data(), in.size(), kFs, 0);
    CHECK (maskFramesFor (in.size()) == (in.size() - kMaskFft) / kMaskHop + 1);
    CHECK (maskFramesFor (kMaskFft - 1) == 0);
    CHECK (c.frames == maskFramesFor (in.size()));
    CHECK (c.masks.size() == static_cast<size_t> (c.frames) * kStems * kMaskBins);
    CHECK (c.prints.size() == static_cast<size_t> (in.size() / kMaskHop));
    CHECK (c.sampleRate == kFs);
}

TEST_CASE ("stems: the four masks of every bin and frame sum to one", "[dsp][stems]")
{
    const auto in = mix (mix (tone (3.0, 220.0, 12, 1.0f, 0.6f), clicks (3.0, 0.25)), mix (sine (3.0, 55.0, 0.4f), noise (3.0, 3, 0.05f)));
    const auto c = separate (in.l.data(), in.r.data(), in.size(), kFs, 0);
    REQUIRE (c.frames > 0);
    int wrong = 0;
    for (int f = 0; f < c.frames; ++f)
        for (int k = 0; k < kMaskBins; ++k)
        {
            int sum = 0;
            for (int s = 0; s < kStems; ++s)
                sum += c.masks[maskIndex (f, s, k)];
            wrong += sum != kMaskFull;
        }
    CHECK (wrong == 0);
    // In floats the four quotients of 255 can round off the last bit; the bytes above are exact.
    const float sum = maskAt (c, 10, kStemVoc, 20) + maskAt (c, 10, kStemBass, 20) + maskAt (c, 10, kStemDrm, 20) + maskAt (c, 10, kStemOth, 20);
    CHECK_THAT (sum, Catch::Matchers::WithinAbs (1.0, 1e-6));
}

TEST_CASE ("stems: a frame is found by timeline position, rounded to the nearest", "[dsp][stems]")
{
    MaskCache c;
    c.sampleRate = kFs;
    c.startSample = 48000;
    c.frames = 100;
    CHECK (cacheFrameAt (c, 48000) == 0);
    CHECK (cacheFrameAt (c, 48000 + 255) == 0);
    CHECK (cacheFrameAt (c, 48000 + 256) == 1);
    CHECK (cacheFrameAt (c, 48000 + 512 * 10 + 100) == 10);
    CHECK (cacheFrameAt (c, 48000 + 512 * 99) == 99);
    CHECK (cacheFrameAt (c, 48000 + 512 * 99 + 256) == -1);   // frame 100 does not exist
    CHECK (cacheFrameAt (c, 48000 - 256) == 0);
    CHECK (cacheFrameAt (c, 48000 - 257) == -1);
    CHECK (maskIndex (2, kStemDrm, 5) == static_cast<size_t> ((2 * kStems + kStemDrm) * kMaskBins + 5));
}

// ------------------------------------------------------------------- separator

TEST_CASE ("stems: a sustained centred tone is vocals", "[dsp][stems]")
{
    const auto in = tone (4.0, 220.0, 16, 1.0f, 1.0f);
    const auto c = separate (in.l.data(), in.r.data(), in.size(), kFs, 0);
    double voc = 0.0, other = 0.0;
    for (int h = 2; h <= 16; ++h)   // 440 Hz .. 3.5 kHz: inside the vocal band
    {
        voc += meanMask (c, kStemVoc, binOf (220.0 * h), binOf (220.0 * h), 20, c.frames - 20) / 15.0;
        other += meanMask (c, kStemOth, binOf (220.0 * h), binOf (220.0 * h), 20, c.frames - 20) / 15.0;
    }
    CHECK (voc > 0.6);
    CHECK (voc > other);
}

TEST_CASE ("stems: the same tone panned hard is other, not vocals", "[dsp][stems]")
{
    const auto in = tone (4.0, 220.0, 16, 1.0f, 0.0f);
    const auto c = separate (in.l.data(), in.r.data(), in.size(), kFs, 0);
    double voc = 0.0, other = 0.0;
    for (int h = 2; h <= 16; ++h)
    {
        voc += meanMask (c, kStemVoc, binOf (220.0 * h), binOf (220.0 * h), 20, c.frames - 20) / 15.0;
        other += meanMask (c, kStemOth, binOf (220.0 * h), binOf (220.0 * h), 20, c.frames - 20) / 15.0;
    }
    CHECK (voc < 0.1);
    CHECK (other > 0.6);
}

TEST_CASE ("stems: the same tone out of phase is other, not vocals", "[dsp][stems]")
{
    const auto in = tone (4.0, 220.0, 16, 1.0f, -1.0f);   // equal level, opposite polarity
    const auto c = separate (in.l.data(), in.r.data(), in.size(), kFs, 0);
    double voc = 0.0, other = 0.0;
    for (int h = 2; h <= 16; ++h)
    {
        voc += meanMask (c, kStemVoc, binOf (220.0 * h), binOf (220.0 * h), 20, c.frames - 20) / 15.0;
        other += meanMask (c, kStemOth, binOf (220.0 * h), binOf (220.0 * h), 20, c.frames - 20) / 15.0;
    }
    CHECK (voc < 0.1);
    CHECK (other > 0.6);
}

TEST_CASE ("stems: a centred tone outside the vocal band is not vocals", "[dsp][stems]")
{
    for (const double hz : { 140.0, 12000.0 })
    {
        CAPTURE (hz);
        const auto in = sine (4.0, hz, 0.3f);
        const auto c = separate (in.l.data(), in.r.data(), in.size(), kFs, 0);
        CHECK (meanMask (c, kStemVoc, binOf (hz), binOf (hz), 20, c.frames - 20) < 0.05);
    }
}

TEST_CASE ("stems: a low centred sine is bass", "[dsp][stems]")
{
    const auto in = sine (4.0, 55.0, 0.5f);
    const auto c = separate (in.l.data(), in.r.data(), in.size(), kFs, 0);
    CHECK (meanMask (c, kStemBass, binOf (55.0), binOf (55.0), 20, c.frames - 20) > 0.8);
}

TEST_CASE ("stems: clicks are drums", "[dsp][stems]")
{
    const double period = 0.25;
    const auto in = clicks (4.0, period);
    const auto c = separate (in.l.data(), in.r.data(), in.size(), kFs, 0);
    double drums = 0.0;
    int counted = 0;
    for (double t = 0.1 + period; t < 3.5; t += period)
    {
        // The frame whose window centre holds the click.
        const int f = (samples (t) - kMaskFft / 2 + kMaskHop / 2) / kMaskHop;
        drums += meanMask (c, kStemDrm, binOf (2000.0), binOf (12000.0), f, f);
        ++counted;
    }
    CHECK (drums / counted > 0.6);
}

TEST_CASE ("stems: silence is other", "[dsp][stems]")
{
    const auto in = silence (2.0);
    const auto c = separate (in.l.data(), in.r.data(), in.size(), kFs, 0);
    REQUIRE (c.frames > 0);
    CHECK (meanMask (c, kStemOth, 0, kMaskBins - 1, 0, c.frames - 1) == 1.0);
}

// ------------------------------------------------------------------- fingerprint

TEST_CASE ("stems: a fingerprint streamed sample by sample is the one computed offline", "[dsp][stems]")
{
    const auto in = mix (tone (1.0, 330.0, 8, 1.0f, 0.5f), noise (1.0, 4, 0.05f));
    const auto offline = fingerprints (in.l.data(), in.r.data(), in.size(), kFs);
    PrintMeter m;
    prepare (m, kFs);
    std::vector<BlockPrint> streamed;
    for (int n = 0; n < in.size(); ++n)
        if (push (m, 0.5f * (in.l[static_cast<size_t> (n)] + in.r[static_cast<size_t> (n)])))
            streamed.push_back (lastPrint (m));
    CHECK (streamed == offline);
    CHECK (offline.size() == static_cast<size_t> (in.size() / kMaskHop));
}

TEST_CASE ("stems: the fingerprint ignores level but not the spectrum", "[dsp][stems]")
{
    const auto a = noise (1.0);
    const auto quiet = scaled (a, 0.25f);   // -12 dB: a trim change, not a new mix
    const auto dark = darkNoise (1.0);
    const auto pa = fingerprints (a.l.data(), a.r.data(), a.size(), kFs);
    const auto pq = fingerprints (quiet.l.data(), quiet.r.data(), quiet.size(), kFs);
    const auto pd = fingerprints (dark.l.data(), dark.r.data(), dark.size(), kFs);
    REQUIRE (pa.size() == pq.size());
    int sameMatched = 0, darkMatched = 0;
    for (size_t i = 0; i < pa.size(); ++i)
    {
        sameMatched += matches (pa[i], pq[i]);
        darkMatched += matches (pa[i], pd[i]);
        CHECK (matches (pa[i], pa[i]));
    }
    CHECK (sameMatched == static_cast<int> (pa.size()));
    CHECK (darkMatched == 0);
}

TEST_CASE ("stems: a print describes its own block, not what came before", "[dsp][stems]")
{
    // 48 blocks of white noise, then dark noise: the late prints are the dark noise's.
    const auto white = noise (1.024), dark = darkNoise (1.024);
    auto joined = white;
    std::copy (dark.l.begin() + 48 * kPrintBlock, dark.l.end(), joined.l.begin() + 48 * kPrintBlock);
    std::copy (dark.r.begin() + 48 * kPrintBlock, dark.r.end(), joined.r.begin() + 48 * kPrintBlock);
    const auto pj = fingerprints (joined.l.data(), joined.r.data(), joined.size(), kFs);
    const auto pd = fingerprints (dark.l.data(), dark.r.data(), dark.size(), kFs);
    const auto pw = fingerprints (white.l.data(), white.r.data(), white.size(), kFs);
    REQUIRE (pj.size() == 96);
    for (size_t i = 50; i < pj.size(); ++i)
    {
        CAPTURE (i);
        CHECK (matches (pj[i], pd[i]));
        CHECK_FALSE (matches (pj[i], pw[i]));
    }
}

TEST_CASE ("stems: a non-finite sample counts as silence in the fingerprint", "[dsp][stems]")
{
    auto bad = noise (0.2), zeroed = bad;
    bad.l[1000] = std::numeric_limits<float>::quiet_NaN();
    bad.r[3000] = std::numeric_limits<float>::infinity();
    zeroed.l[1000] = zeroed.r[1000] = 0.0f;   // the mono sum of either is 0 there
    zeroed.l[3000] = zeroed.r[3000] = 0.0f;
    CHECK (fingerprints (bad.l.data(), bad.r.data(), bad.size(), kFs) == fingerprints (zeroed.l.data(), zeroed.r.data(), zeroed.size(), kFs));
}

TEST_CASE ("stems: silence matches silence only", "[dsp][stems]")
{
    const auto quiet = silence (0.1);
    const auto loud = noise (0.1);
    const auto ps = fingerprints (quiet.l.data(), quiet.r.data(), quiet.size(), kFs);
    const auto pn = fingerprints (loud.l.data(), loud.r.data(), loud.size(), kFs);
    REQUIRE (! ps.empty());
    CHECK (isSilent (ps[0]));
    CHECK_FALSE (isSilent (pn[0]));
    CHECK (matches (ps[0], ps[1]));
    CHECK_FALSE (matches (ps[0], pn[0]));
    CHECK_FALSE (matches (pn[0], ps[0]));
}

// ------------------------------------------------------------------- layer

TEST_CASE ("stems layer: without a cache it is a pure 1024-sample delay", "[dsp][stems]")
{
    auto layer = makeLayer (nullptr);
    CHECK (latencySamples (layer) == kStemsFft);
    const auto in = noise (1.0);
    const auto out = run (layer, in, 0);
    CHECK (maxError (in, out, 0, in.size() - kStemsFft) < 1e-5f);
    CHECK (status (layer) == StemsStatus::NoCache);
    CHECK (engaged (layer) == 0.0f);
}

TEST_CASE ("stems layer: with every stem at 0 dB the mix passes unchanged", "[dsp][stems]")
{
    const auto in = mix (tone (3.0, 220.0, 12, 1.0f, 0.7f), mix (clicks (3.0, 0.25), noise (3.0, 5, 0.05f)));
    const auto cache = separate (in.l.data(), in.r.data(), in.size(), kFs, 0);
    auto layer = makeLayer (&cache);
    const auto out = run (layer, in, 0);
    CHECK (status (layer) == StemsStatus::Active);
    CHECK (engaged (layer) == 1.0f);
    CHECK (maxError (in, out, 0, in.size() - kStemsFft) < 1e-5f);
}

TEST_CASE ("stems layer: a stem's gain applies where the cache says that stem is", "[dsp][stems]")
{
    const auto in = noise (4.0);
    const int half = maskFramesFor (in.size()) / 2;
    const auto cache = handMadeCache (in, 0, [half] (int f) { return f < half ? kStemBass : kStemDrm; });
    StemsSettings s;
    s.gainDb[kStemBass] = -12.0f;
    auto layer = makeLayer (&cache, s);
    const auto out = run (layer, in, 0);
    CHECK_THAT (gainDb (in, out, samples (0.5), samples (1.5)), Catch::Matchers::WithinAbs (-12.0, 0.5));
    CHECK_THAT (gainDb (in, out, samples (2.5), samples (3.5)), Catch::Matchers::WithinAbs (0.0, 0.5));
}

TEST_CASE ("stems layer: a seek looks up the new position", "[dsp][stems]")
{
    const auto in = noise (4.0);
    const int half = maskFramesFor (in.size()) / 2;
    const auto cache = handMadeCache (in, 0, [half] (int f) { return f < half ? kStemBass : kStemDrm; });
    StemsSettings s;
    s.gainDb[kStemBass] = -12.0f;
    auto layer = makeLayer (&cache, s);

    // Play 1 s from 2.5 s (drums: 0 dB), then jump back to 0.5 s (bass: -12 dB).
    const auto first = slice (in, samples (2.5), samples (1.0));
    const auto second = slice (in, samples (0.5), samples (1.0));
    const auto out1 = run (layer, first, samples (2.5));
    const auto out2 = run (layer, second, samples (0.5));
    CHECK_THAT (gainDb (first, out1, samples (0.2), samples (0.9)), Catch::Matchers::WithinAbs (0.0, 0.5));
    CHECK_THAT (gainDb (second, out2, samples (0.3), samples (0.9)), Catch::Matchers::WithinAbs (-12.0, 0.5));
    CHECK (status (layer) == StemsStatus::Active);
}

TEST_CASE ("stems layer: it bypasses while it cannot know where it is", "[dsp][stems]")
{
    const auto in = noise (2.0);
    const auto cache = handMadeCache (in, 0, [] (int) { return kStemBass; });
    StemsSettings s;
    s.gainDb[kStemBass] = -12.0f;

    SECTION ("host stopped")
    {
        auto layer = makeLayer (&cache, s);
        const auto out = run (layer, in, 0, false, true);
        CHECK (maxError (in, out, 0, in.size() - kStemsFft) < 1e-5f);
        CHECK (status (layer) == StemsStatus::Waiting);
    }
    SECTION ("no position from the host")
    {
        auto layer = makeLayer (&cache, s);
        const auto out = run (layer, in, 0, true, false);
        CHECK (maxError (in, out, 0, in.size() - kStemsFft) < 1e-5f);
        CHECK (status (layer) == StemsStatus::Waiting);
    }
    SECTION ("playing outside the analysed stretch")
    {
        auto layer = makeLayer (&cache, s);
        const auto out = run (layer, in, samples (10.0));
        CHECK (maxError (in, out, 0, in.size() - kStemsFft) < 1e-5f);
        CHECK (status (layer) == StemsStatus::Waiting);
    }
    SECTION ("playing before the analysed stretch")
    {
        const auto late = handMadeCache (in, samples (5.0), [] (int) { return kStemBass; });
        auto layer = makeLayer (&late, s);
        const auto out = run (layer, in, 0);
        CHECK (maxError (in, out, 0, in.size() - kStemsFft) < 1e-5f);
        CHECK (status (layer) == StemsStatus::Waiting);
    }
    SECTION ("a cache made at another sample rate")
    {
        auto other = cache;
        other.sampleRate = 44100.0;
        auto layer = makeLayer (&other, s);
        const auto out = run (layer, in, 0);
        CHECK (maxError (in, out, 0, in.size() - kStemsFft) < 1e-5f);
        CHECK (status (layer) == StemsStatus::NoCache);
    }
}

TEST_CASE ("stems layer: it fades in over 50 ms, not at once", "[dsp][stems]")
{
    const auto in = noise (0.3);
    const auto cache = handMadeCache (in, 0, [] (int) { return kStemBass; });
    StemsSettings s;
    s.gainDb[kStemBass] = -12.0f;
    auto layer = makeLayer (&cache, s);
    auto buf = in;
    float at25 = -1.0f, at60 = -1.0f;
    for (int n = 0; n < samples (0.1); n += 64)
    {
        process (layer, buf.l.data() + n, buf.r.data() + n, 64, Transport { true, true, n });
        if (n + 64 == 1216) at25 = engaged (layer);   // 25.3 ms
        if (n + 64 == 2880) at60 = engaged (layer);   // 60 ms
    }
    CHECK (at25 > 0.25f);
    CHECK (at25 < 0.75f);
    CHECK (at60 == 1.0f);
    // The first 10 ms are only part of the way to -12 dB.
    const double early = gainDb (in, buf, 0, 512);
    CHECK (early > -8.0);
    CHECK (early < -0.5);
}

TEST_CASE ("stems layer: it fades out over 50 ms when the host stops", "[dsp][stems]")
{
    const auto in = noise (0.5);
    const auto cache = handMadeCache (in, 0, [] (int) { return kStemBass; });
    StemsSettings s;
    s.gainDb[kStemBass] = -12.0f;
    auto layer = makeLayer (&cache, s);
    auto buf = in;
    const int stop = samples (0.2);
    for (int n = 0; n < stop; n += 64)
        process (layer, buf.l.data() + n, buf.r.data() + n, 64, Transport { true, true, n });
    REQUIRE (engaged (layer) == 1.0f);
    float at5 = -1.0f, at60 = -1.0f;
    for (int k = 0; k < samples (0.1); k += 64)
    {
        process (layer, buf.l.data() + stop + k, buf.r.data() + stop + k, 64, Transport { false, true, stop + k });
        if (k + 64 == 256) at5 = engaged (layer);   // 5.3 ms
        if (k + 64 == 2880) at60 = engaged (layer);
    }
    CHECK (at5 > 0.5f);
    CHECK (at5 < 1.0f);
    CHECK (at60 == 0.0f);
    CHECK (status (layer) == StemsStatus::Waiting);
}

TEST_CASE ("stems layer: handing it the same cache again changes nothing", "[dsp][stems]")
{
    // The chain may hand the cache over every block.
    const auto in = noise (0.3);
    const auto cache = handMadeCache (in, 0, [] (int) { return kStemBass; });
    auto layer = makeLayer (&cache);
    auto buf = in;
    for (int n = 0; n < samples (0.1); n += 64)
    {
        setCache (layer, &cache);
        process (layer, buf.l.data() + n, buf.r.data() + n, 64, Transport { true, true, n });
    }
    CHECK (engaged (layer) == 1.0f);
    CHECK (status (layer) == StemsStatus::Active);
}

TEST_CASE ("stems layer: audio that no longer matches is stale and bypassed, until it matches again", "[dsp][stems]")
{
    const auto analysed = noise (3.0);
    const auto cache = handMadeCache (analysed, 0, [] (int) { return kStemBass; });
    StemsSettings s;
    s.gainDb[kStemBass] = -12.0f;

    SECTION ("the analysed audio, even 12 dB quieter, is never stale")
    {
        auto layer = makeLayer (&cache, s);
        const auto quiet = scaled (analysed, 0.25f);
        auto buf = quiet;
        bool everStale = false;
        for (int n = 0; n + 256 <= buf.size(); n += 256)
        {
            process (layer, buf.l.data() + n, buf.r.data() + n, 256, Transport { true, true, n });
            everStale |= status (layer) == StemsStatus::Stale;
        }
        CHECK_FALSE (everStale);
        CHECK (status (layer) == StemsStatus::Active);
    }
    SECTION ("different audio at the same positions")
    {
        auto layer = makeLayer (&cache, s);
        const auto changed = darkNoise (1.0);
        const auto out = run (layer, changed, 0);
        CHECK (status (layer) == StemsStatus::Stale);
        CHECK (engaged (layer) == 0.0f);
        CHECK_THAT (gainDb (changed, out, samples (0.5), samples (0.9)), Catch::Matchers::WithinAbs (0.0, 0.1));

        // The mix is put back: it matches again from where the host now plays.
        const auto back = slice (analysed, samples (1.0), samples (1.0));
        const auto out2 = run (layer, back, samples (1.0));
        CHECK (status (layer) == StemsStatus::Active);
        CHECK_THAT (gainDb (back, out2, samples (0.5), samples (0.9)), Catch::Matchers::WithinAbs (-12.0, 0.5));
    }
}

TEST_CASE ("stems layer: one odd block is not enough to call the mix changed", "[dsp][stems]")
{
    const auto analysed = noise (2.0);
    const auto cache = handMadeCache (analysed, 0, [] (int) { return kStemBass; });
    auto played = analysed;
    const auto odd = darkNoise (2.0);
    for (size_t n = 40 * kPrintBlock; n < 41 * kPrintBlock; ++n)
    {
        played.l[n] = odd.l[n];
        played.r[n] = odd.r[n];
    }
    auto layer = makeLayer (&cache);
    bool everStale = false;
    for (int n = 0; n + 256 <= played.size(); n += 256)
    {
        process (layer, played.l.data() + n, played.r.data() + n, 256, Transport { true, true, n });
        everStale |= status (layer) == StemsStatus::Stale;
    }
    CHECK_FALSE (everStale);
}

TEST_CASE ("stems layer: after a seek the check lines up with the cache's blocks again", "[dsp][stems]")
{
    // A mix that changes every print block: a print taken across two blocks matches neither.
    const auto analysed = alternating (noise (3.0), sine (3.0, 100.0, 0.3f));
    const auto cache = handMadeCache (analysed, 0, [] (int) { return kStemBass; });
    auto layer = makeLayer (&cache);
    run (layer, slice (analysed, 0, samples (0.5)), 0);
    REQUIRE (status (layer) == StemsStatus::Active);

    // 1 s is neither on the 512 grid nor in step with where the last block ended.
    auto buf = slice (analysed, samples (1.0), samples (1.0));
    bool everStale = false;
    for (int n = 0; n + 256 <= buf.size(); n += 256)
    {
        process (layer, buf.l.data() + n, buf.r.data() + n, 256, Transport { true, true, samples (1.0) + n });
        everStale |= status (layer) == StemsStatus::Stale;
    }
    CHECK_FALSE (everStale);
    CHECK (status (layer) == StemsStatus::Active);
}

TEST_CASE ("stems layer: a new cache starts fresh, not stale", "[dsp][stems]")
{
    const auto analysed = noise (2.0);
    const auto cache = handMadeCache (analysed, 0, [] (int) { return kStemBass; });
    auto layer = makeLayer (&cache);
    const auto changed = darkNoise (1.0);
    run (layer, changed, 0);
    REQUIRE (status (layer) == StemsStatus::Stale);

    // Analysed again: a cache of what plays now. Active from the next hop, before any print.
    const auto fresh = handMadeCache (changed, 0, [] (int) { return kStemBass; });
    setCache (layer, &fresh);
    CHECK (status (layer) == StemsStatus::Waiting);
    auto buf = slice (changed, 0, kStemsHop);
    process (layer, buf.l.data(), buf.r.data(), kStemsHop, Transport { true, true, 0 });
    CHECK (status (layer) == StemsStatus::Active);
}

TEST_CASE ("stems layer: a tilt turns a stem around 1 kHz, up to 12 dB", "[dsp][stems]")
{
    auto measure = [] (double hz, float tilt)
    {
        const auto in = sine (1.0, hz, 0.3f);
        const auto cache = handMadeCache (in, 0, [] (int) { return kStemVoc; });
        StemsSettings s;
        s.tiltDbPerOct[kStemVoc] = tilt;
        auto layer = makeLayer (&cache, s);
        const auto out = run (layer, in, 0);
        return gainDb (in, out, samples (0.4), samples (0.9));
    };
    CHECK_THAT (measure (2000.0, 3.0f), Catch::Matchers::WithinAbs (3.0, 0.5));
    CHECK_THAT (measure (500.0, 3.0f), Catch::Matchers::WithinAbs (-3.0, 0.5));
    CHECK_THAT (measure (1000.0, 3.0f), Catch::Matchers::WithinAbs (0.0, 0.5));
    CHECK_THAT (measure (16000.0, 6.0f), Catch::Matchers::WithinAbs (12.0, 0.5));   // 4 octaves up: capped
}

TEST_CASE ("stems layer: settings are clamped and a non-finite one keeps the last value", "[dsp][stems]")
{
    StemsLayer layer;
    prepare (layer, kFs);
    StemsSettings s;
    s.gainDb = { 20.0f, -20.0f, 3.0f, 0.0f };
    s.tiltDbPerOct = { 9.0f, -9.0f, 1.0f, 0.0f };
    setSettings (layer, s);
    CHECK (layer.settings.gainDb[0] == kStemsMaxGainDb);
    CHECK (layer.settings.gainDb[1] == -kStemsMaxGainDb);
    CHECK (layer.settings.tiltDbPerOct[0] == kStemsMaxTilt);
    CHECK (layer.settings.tiltDbPerOct[1] == -kStemsMaxTilt);
    s.gainDb[2] = std::numeric_limits<float>::quiet_NaN();
    s.tiltDbPerOct[2] = std::numeric_limits<float>::infinity();
    setSettings (layer, s);
    CHECK (layer.settings.gainDb[2] == 3.0f);
    CHECK (layer.settings.tiltDbPerOct[2] == 1.0f);
}

TEST_CASE ("stems layer: processing never allocates", "[dsp][stems]")
{
    const auto in = noise (1.0);
    const auto cache = handMadeCache (in, 0, [] (int f) { return f % kStems; });
    StemsSettings s;
    s.gainDb = { -3.0f, 2.0f, -6.0f, 1.0f };
    auto layer = makeLayer (&cache, s);
    auto buf = in;
    std::size_t allocations = 0;
    {
        const ScopedAllocCounter counter;
        for (int n = 0; n + 480 <= buf.size(); n += 480)
            process (layer, buf.l.data() + n, buf.r.data() + n, 480, Transport { true, true, n });
        allocations = counter.count();
    }
    CHECK (allocations == 0);
}

TEST_CASE ("stems layer: a non-finite input is cleared", "[dsp][stems]")
{
    // Shorter than the window: the NaN is still inside the STFT when the check runs (it holds
    // no feedback, so a NaN would otherwise leave on its own after two windows).
    auto layer = makeLayer (nullptr);
    auto bad = noise (0.01);
    bad.l[100] = std::numeric_limits<float>::quiet_NaN();
    run (layer, bad, 0);
    CHECK (resetIfNotFinite (layer));
    CHECK_FALSE (resetIfNotFinite (layer));
    const auto in = noise (0.2);
    const auto out = run (layer, in, samples (0.05));
    for (int n = 0; n < out.size(); ++n)
        REQUIRE (std::isfinite (out.l[static_cast<size_t> (n)]));
}
