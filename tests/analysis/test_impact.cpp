// Impact analyser (the IMPACT MAP data).
//   Audio thread: one capture ring for all N+1 tap points (layer boundaries), mid/side per tap.
//   Whole blocks are reserved or dropped together, so the taps can never drift apart.
//   Analysis thread (~30 Hz): per layer, Hann-windowed FFT of the tap before and after it, the
//   "before" window shifted back by the layer's latency; energies into 1/6-octave bands
//   (20 Hz - 20 kHz) with fractional bin overlap; one-pole smoothing over frames; then
//     gainDb  = 10 log10 (E_after / E_before)               (E = mid + side energy)
//     widthDb = 10 log10 (S/M after) - 10 log10 (S/M before)
//   both clamped to +-24 dB, published through a lock-free triple buffer.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <cmath>
#include <complex>
#include <functional>
#include <limits>
#include <memory>
#include <numbers>
#include <thread>
#include <vector>

#include "analysis/ImpactAnalyser.h"
#include "support/Analysis.h"

using namespace strat3::analysis;
using namespace strat3::test;

namespace {

constexpr double kFs = 48000.0;
constexpr double kTwoPi = 2.0 * std::numbers::pi;

struct StereoSample
{
    float l = 0.0f, r = 0.0f;
};

using Layer = std::function<StereoSample (StereoSample)>;

/** Runs a one-layer chain through capture + analyser, computing a frame every 1/30 s. */
struct Rig
{
    std::unique_ptr<ImpactCapture> cap = std::make_unique<ImpactCapture>();
    std::unique_ptr<ImpactAnalyser> an = std::make_unique<ImpactAnalyser>();
    double fs;
    int frameEvery;
    int sinceFrame = 0;

    explicit Rig (int layers = 1, double sampleRate = kFs, int latency = 0)
        : Rig (std::vector<int> (static_cast<size_t> (layers), latency), sampleRate) {}

    explicit Rig (const std::vector<int>& lat, double sampleRate = kFs) : fs (sampleRate)
    {
        const int layers = static_cast<int> (lat.size());
        prepare (*cap, layers + 1);
        prepare (*an, fs, layers);
        setLatencies (*an, lat);
        frameEvery = static_cast<int> (std::lround (fs / kImpactFrameHz));
    }

    /** Feeds `n` samples from `source` through `layers`, 64-sample host blocks. */
    void run (const std::function<StereoSample()>& source, const std::vector<Layer>& layers, int n)
    {
        constexpr int block = 64;
        for (int done = 0; done < n; done += block)
        {
            REQUIRE (beginBlock (*cap, block));
            for (int i = 0; i < block; ++i)
            {
                StereoSample s = source();
                write (*cap, 0, i, s.l, s.r);
                for (size_t k = 0; k < layers.size(); ++k)
                {
                    s = layers[k] (s);
                    write (*cap, static_cast<int> (k) + 1, i, s.l, s.r);
                }
            }
            endBlock (*cap);
            pull (*an, *cap);
            if ((sinceFrame += block) >= frameEvery)
            {
                sinceFrame = 0;
                computeFrame (*an);
            }
        }
    }

    ImpactFrame latest()
    {
        ImpactFrame f;
        readLatest (*an, f);
        return f;
    }
};

Layer delayLine (int delay)
{
    auto line = std::make_shared<std::vector<StereoSample>> (static_cast<size_t> (delay));
    auto pos = std::make_shared<size_t> (0);
    return [=] (StereoSample s) {
        const auto out = (*line)[*pos];
        (*line)[*pos] = s;
        *pos = (*pos + 1) % line->size();
        return out;
    };
}

/** Sparse loud clicks over a faint bed: before/after windows only match if they are aligned. */
std::function<StereoSample()> clicks (std::uint64_t seed)
{
    auto n = std::make_shared<int> (0);
    auto w = std::make_shared<WhiteNoise>();
    w->counter = seed;
    return [=] {
        const int i = (*n)++;
        const float v = (i % 1999) < 3 ? 0.8f * w->next() : 0.001f * w->next();
        return StereoSample { v, -v };
    };
}

std::function<StereoSample()> noise (float amp, std::uint64_t seed, bool independent = true)
{
    auto w = std::make_shared<WhiteNoise>();
    w->counter = seed;
    return [w, amp, independent] {
        const float l = amp * w->next();
        return StereoSample { l, independent ? amp * w->next() : l };
    };
}

Layer gain (float g) { return [g] (StereoSample s) { return StereoSample { s.l * g, s.r * g }; }; }

/** Bands whose centre lies between lo and hi. */
std::vector<int> bandsBetween (double lo, double hi)
{
    std::vector<int> b;
    for (int i = 0; i < kImpactBands; ++i)
        if (impactBandCentreHz (i) >= lo && impactBandCentreHz (i) <= hi)
            b.push_back (i);
    return b;
}

} // namespace

// ------------------------------------------------------------------- layout

TEST_CASE ("impact bands: 1/6 octave, centred on 1 kHz, covering 20 Hz - 20 kHz", "[analysis][impact][layout]")
{
    CHECK (kImpactBands == 61);
    CHECK (std::abs (impactBandCentreHz (34) - 1000.0) < 1e-9);
    CHECK (impactBandCentreHz (0) >= 19.0);   // the lowest band's upper edge passes 20 Hz
    CHECK (impactBandCentreHz (0) * std::pow (2.0, 1.0 / 12.0) > 20.0);
    CHECK (impactBandCentreHz (kImpactBands - 1) <= 20200.0);
    CHECK (impactBandCentreHz (kImpactBands - 1) > 20000.0);
    for (int b = 1; b < kImpactBands; ++b)
        CHECK (std::abs (impactBandCentreHz (b) / impactBandCentreHz (b - 1) - std::pow (2.0, 1.0 / 6.0)) < 1e-12);
}

TEST_CASE ("FFT size keeps ~23 Hz resolution or better as the rate rises", "[analysis][impact][layout]")
{
    CHECK (impactFftSize (44100.0) == 2048);
    CHECK (impactFftSize (48000.0) == 2048);
    CHECK (impactFftSize (88200.0) == 4096);
    CHECK (impactFftSize (96000.0) == 4096);
    CHECK (impactFftSize (192000.0) == 8192);
}

// ------------------------------------------------------------------- gain impact

TEST_CASE ("an identity layer has exactly zero impact everywhere", "[analysis][impact][null]")
{
    Rig rig;
    rig.run (noise (0.3f, 1), { gain (1.0f) }, 48000);
    const auto f = rig.latest();
    CHECK (f.layers == 1);
    for (int b = 0; b < kImpactBands; ++b)
    {
        CHECK (f.gainDb[0][static_cast<size_t> (b)] == 0.0f);
        CHECK (f.widthDb[0][static_cast<size_t> (b)] == 0.0f);
    }
}

TEST_CASE ("a -6 dB gain layer reads -6.02 dB in every band", "[analysis][impact][gain]")
{
    const double fs = GENERATE (44100.0, 48000.0, 96000.0, 192000.0);
    CAPTURE (fs);
    Rig rig (1, fs);
    rig.run (noise (0.3f, 2), { gain (0.5f) }, static_cast<int> (fs));
    const auto f = rig.latest();
    for (int b = 0; b < kImpactBands; ++b)
    {
        CAPTURE (b, f.gainDb[0][static_cast<size_t> (b)]);
        CHECK (std::abs (f.gainDb[0][static_cast<size_t> (b)] - -6.0206f) < 0.01f);
        CHECK (std::abs (f.widthDb[0][static_cast<size_t> (b)]) < 0.01f);
    }
}

TEST_CASE ("a tonal layer reads its own magnitude response, band by band", "[analysis][impact][gain]")
{
    // One-pole low-pass at 1 kHz on both channels: |H| is known in closed form.
    const double fc = 1000.0;
    const float a = static_cast<float> (std::exp (-kTwoPi * fc / kFs));
    auto zl = std::make_shared<float> (0.0f), zr = std::make_shared<float> (0.0f);
    Layer lp = [=] (StereoSample s) {
        *zl = (1.0f - a) * s.l + a * *zl;
        *zr = (1.0f - a) * s.r + a * *zr;
        return StereoSample { *zl, *zr };
    };
    Rig rig;
    rig.run (noise (0.3f, 3), { lp }, 96000);
    const auto f = rig.latest();
    auto magDb = [&] (double hz) {
        const auto z = std::polar (1.0, -kTwoPi * hz / kFs);
        return 20.0 * std::log10 (std::abs ((1.0 - static_cast<double> (a)) / (1.0 - static_cast<double> (a) * z)));
    };
    for (int b : bandsBetween (60.0, 8000.0))
    {
        const double expected = magDb (impactBandCentreHz (b));
        CAPTURE (impactBandCentreHz (b), expected, f.gainDb[0][static_cast<size_t> (b)]);
        CHECK (std::abs (static_cast<double> (f.gainDb[0][static_cast<size_t> (b)]) - expected) < 0.1);   // measured worst 0.035 dB (60 Hz - 8 kHz)
    }
}

TEST_CASE ("latency compensation: a pure delay layer has no impact on transient material", "[analysis][impact][latency]")
{
    // Sparse clicks: without compensation the before/after windows hold different clicks.
    constexpr int delay = 240;   // 5 ms, the LIMIT look-ahead
    Rig rig (1, kFs, delay);
    rig.run (clicks (1), { delayLine (delay) }, 96000);
    const auto f = rig.latest();
    double worst = 0.0;
    for (int b : bandsBetween (100.0, 16000.0))
        worst = std::max (worst, std::abs (static_cast<double> (f.gainDb[0][static_cast<size_t> (b)])));
    CAPTURE (worst);
    CHECK (worst < 0.1);
}

TEST_CASE ("latency compensation per layer: a delay after a zero-latency layer", "[analysis][impact][latency]")
{
    // Layer 1 has no latency, layer 2 is a 240-sample delay: layer 2's "before" window must be
    // tap 1 shifted by 240, not the window just computed for layer 1's "after".
    Rig rig (std::vector<int> { 0, 240 });
    rig.run (clicks (2), { gain (0.5f), delayLine (240) }, 96000);
    const auto f = rig.latest();
    double worst = 0.0;
    for (int b : bandsBetween (100.0, 16000.0))
        worst = std::max (worst, std::abs (static_cast<double> (f.gainDb[1][static_cast<size_t> (b)])));
    CAPTURE (worst);
    CHECK (worst < 0.1);
    CHECK (std::abs (f.gainDb[0][34] - -6.0206f) < 0.01f);
}

TEST_CASE ("impact clamps at +-24 dB and silence reads zero", "[analysis][impact][gain]")
{
    Rig rig;
    rig.run (noise (0.3f, 4), { gain (0.001f) }, 48000);
    const auto clamped = rig.latest();
    for (float v : clamped.gainDb[0])
        CHECK (v == -kImpactClampDb);

    Rig quiet;
    quiet.run ([] { return StereoSample {}; }, { gain (0.5f) }, 48000);
    const auto q = quiet.latest();
    for (int b = 0; b < kImpactBands; ++b)
    {
        CHECK (q.gainDb[0][static_cast<size_t> (b)] == 0.0f);
        CHECK (q.widthDb[0][static_cast<size_t> (b)] == 0.0f);
    }
}

// ------------------------------------------------------------------- width impact

TEST_CASE ("doubling the side signal reads +6.02 dB width; gain follows the energy sum", "[analysis][impact][width]")
{
    Layer wide = [] (StereoSample s) {
        const float m = 0.5f * (s.l + s.r), side = 0.5f * (s.l - s.r) * 2.0f;
        return StereoSample { m + side, m - side };
    };
    // Side = mid one sample later: the same power spectrum, so E_mid = E_side in every band
    // (independent channels would only match statistically).
    auto w = std::make_shared<WhiteNoise>();
    w->counter = 5;
    auto prev = std::make_shared<float> (0.0f);
    std::function<StereoSample()> src = [=] {
        const float m = 0.3f * w->next(), side = *prev;
        *prev = m;
        return StereoSample { m + side, m - side };
    };
    Rig rig;
    rig.run (src, { wide }, 96000);
    const auto f = rig.latest();
    for (int b : bandsBetween (100.0, 16000.0))
    {
        CAPTURE (b);
        CHECK (std::abs (f.widthDb[0][static_cast<size_t> (b)] - 6.0206f) < 0.05f);   // mid is untouched
        CHECK (std::abs (f.gainDb[0][static_cast<size_t> (b)] - 3.979f) < 0.1f);      // (1 + 4) / (1 + 1)
    }
}

TEST_CASE ("mono in, mono out: width impact is zero, never NaN", "[analysis][impact][width]")
{
    Rig rig;
    rig.run (noise (0.3f, 6, false), { gain (0.5f) }, 48000);
    const auto f = rig.latest();
    for (float v : f.widthDb[0])
        CHECK (v == 0.0f);
}

// ------------------------------------------------------------------- time behaviour

TEST_CASE ("impact is smoothed over frames, then settles", "[analysis][impact][smoothing]")
{
    auto g = std::make_shared<float> (1.0f);
    Layer step = [g] (StereoSample s) { return StereoSample { s.l * *g, s.r * *g }; };
    Rig rig;
    auto src = noise (0.3f, 7);
    rig.run (src, { step }, 48000);
    *g = 0.5f;
    rig.run (src, { step }, 4800);   // 100 ms: about three frames
    const float early = rig.latest().gainDb[0][34];
    CHECK (early < -0.5f);
    CHECK (early > -5.5f);
    rig.run (src, { step }, 96000);
    CHECK (std::abs (rig.latest().gainDb[0][34] - -6.0206f) < 0.05f);
}

TEST_CASE ("reading again without a new frame returns the same frame, not an empty one", "[analysis][impact]")
{
    Rig rig;
    rig.run (noise (0.3f, 10), { gain (0.5f) }, 48000);
    ImpactFrame a, b;
    CHECK (readLatest (*rig.an, a));
    CHECK_FALSE (readLatest (*rig.an, b));
    CHECK (b.layers == 1);
    CHECK (a.gainDb == b.gainDb);
}

TEST_CASE ("several layers: each one sees only its own change", "[analysis][impact][layers]")
{
    Rig rig (3);
    rig.run (noise (0.3f, 8), { gain (0.5f), gain (1.0f), gain (2.0f) }, 48000);
    const auto f = rig.latest();
    CHECK (f.layers == 3);
    CHECK (std::abs (f.gainDb[0][34] - -6.0206f) < 0.01f);
    CHECK (f.gainDb[1][34] == 0.0f);
    CHECK (std::abs (f.gainDb[2][34] - 6.0206f) < 0.01f);
}

// ------------------------------------------------------------------- capture ring

TEST_CASE ("capture reserves or drops whole blocks, counts drops, and never blocks", "[analysis][impact][fifo]")
{
    auto cap = std::make_unique<ImpactCapture>();
    prepare (*cap, 2);
    int accepted = 0, refused = 0;
    for (int k = 0; k < 2 * kImpactCaptureFrames / 512; ++k)
    {
        if (beginBlock (*cap, 512))
        {
            for (int i = 0; i < 512; ++i)
            {
                write (*cap, 0, i, 0.1f, 0.1f);
                write (*cap, 1, i, 0.1f, 0.1f);
            }
            ++accepted;
        }
        else
            ++refused;
        endBlock (*cap);   // a refused block's end is a no-op
    }
    CHECK (accepted == kImpactCaptureFrames / 512);
    CHECK (refused == kImpactCaptureFrames / 512);
    CHECK (droppedSamples (*cap) == static_cast<std::uint32_t> (refused * 512));
}

TEST_CASE ("writes into a refused block never touch unread frames", "[analysis][impact][fifo]")
{
    auto cap = std::make_unique<ImpactCapture>();
    prepare (*cap, 1);
    for (int k = 0; k < kImpactCaptureFrames / 512; ++k)   // fill exactly, every frame = 0.25
    {
        REQUIRE (beginBlock (*cap, 512));
        for (int i = 0; i < 512; ++i)
            write (*cap, 0, i, 0.25f, 0.25f);
        endBlock (*cap);
    }
    REQUIRE_FALSE (beginBlock (*cap, 512));
    for (int i = 0; i < 512; ++i)
        write (*cap, 0, i, -1.0f, 1.0f);   // dropped: must be a no-op
    endBlock (*cap);
    std::array<float, 2> frame {};
    int bad = 0, count = 0;
    while (popFrame (*cap, frame.data()))
    {
        bad += (frame[0] != 0.25f || frame[1] != 0.0f) ? 1 : 0;
        ++count;
    }
    CHECK (count == kImpactCaptureFrames);
    CHECK (bad == 0);
}

TEST_CASE ("capture: a concurrent consumer sees every sample of every tap, aligned", "[analysis][impact][fifo][threads]")
{
    auto cap = std::make_unique<ImpactCapture>();
    prepare (*cap, 3);
    constexpr int blocks = 4000, block = 128;
    std::atomic<bool> ok { true };
    std::thread consumer ([&] {
        std::vector<float> frame (6);
        for (int expected = 0; expected < blocks * block;)
            if (popFrame (*cap, frame.data()))
            {
                for (int t = 0; t < 3; ++t)   // mid of tap t carries sampleIndex + t
                    if (frame[static_cast<size_t> (2 * t)] != static_cast<float> (expected + t))
                        ok = false;
                ++expected;
            }
    });
    for (int k = 0; k < blocks;)
    {
        if (! beginBlock (*cap, block))
            continue;
        for (int i = 0; i < block; ++i)
            for (int t = 0; t < 3; ++t)
            {
                const float v = static_cast<float> (k * block + i + t);
                write (*cap, t, i, v, v);   // l = r = v: mid = v, side = 0
            }
        endBlock (*cap);
        ++k;
    }
    consumer.join();
    CHECK (ok);
}

// ------------------------------------------------------------------- triple buffer

TEST_CASE ("triple buffer: the reader always gets a whole, newest-or-newer frame", "[analysis][impact][threads]")
{
    TripleBuffer<std::array<int, 64>> tb;
    constexpr int n = 100000;
    std::atomic<bool> done { false }, ok { true };
    std::atomic<int> lastSeen { -1 };
    std::thread reader ([&] {
        std::array<int, 64> v {};
        int last = -1;
        while (! done.load())
            if (fetch (tb, v))
            {
                for (int x : v)
                    if (x != v[0])
                        ok = false;   // torn frame
                if (v[0] < last)
                    ok = false;       // went backwards
                last = v[0];
                lastSeen = last;
            }
    });
    for (int i = 0; i < n; ++i)
    {
        back (tb).fill (i);
        publish (tb);
    }
    done = true;
    reader.join();
    std::array<int, 64> v {};
    const int final = fetch (tb, v) ? v[0] : lastSeen.load();   // the reader may already have it
    CHECK (final == n - 1);
    CHECK_FALSE (fetch (tb, v));   // nothing newer
    CHECK (ok);
}

// ------------------------------------------------------------------- stability

TEST_CASE ("a NaN in the capture never reaches the published frame", "[analysis][impact][stability]")
{
    Rig rig;
    auto n = std::make_shared<int> (0);
    auto src = noise (0.3f, 9);
    std::function<StereoSample()> poisoned = [=] {
        auto s = src();
        if ((*n)++ == 20000)
            s.l = std::numeric_limits<float>::quiet_NaN();
        return s;
    };
    rig.run (poisoned, { gain (0.5f) }, 24000);
    const auto f = rig.latest();
    for (float v : f.gainDb[0])
        REQUIRE (std::isfinite (v));
    rig.run (src, { gain (0.5f) }, 96000);
    CHECK (std::abs (rig.latest().gainDb[0][34] - -6.0206f) < 0.05f);
}

// Module 20b: the EQ editor draws the spectrum entering the EQ layer behind its nodes. The
// analyser already holds the smoothed "before" band energies, so it publishes them as levels:
//   levelDb = 10 log10 (E_before), E = mid + side mean square per 1/6-octave band, floored at
//   kImpactLevelFloorDb. Same smoothing as the impact, so the backdrop and the map agree.
TEST_CASE ("level spectrum: a sine reads its mean square in its band, others far below", "[analysis][impact][level]")
{
    Rig rig (2);
    const double amp = 0.5;   // mono: mid = amp, side = 0 -> E = amp^2 / 2 = -9.03 dB
    auto phase = std::make_shared<double> (0.0);
    const auto sine = [=] {
        const float v = static_cast<float> (amp * std::sin (*phase));
        *phase += kTwoPi * 1000.0 / kFs;
        return StereoSample { v, v };
    };
    rig.run (sine, { gain (1.0f), gain (0.5f) }, static_cast<int> (kFs * 3.0));
    const auto f = rig.latest();
    const int band1k = 34;
    const double expect = 10.0 * std::log10 (amp * amp / 2.0);
    // The 1 kHz band takes most of the Hann main lobe; the neighbours split the rest.
    CHECK (std::abs (static_cast<double> (f.levelDb[0][band1k]) - expect) < 1.0);
    // Layer 1's input is tap 1, after the unity gain of layer 0: the same level.
    CHECK (std::abs (static_cast<double> (f.levelDb[1][band1k]) - expect) < 1.0);
    for (int b : bandsBetween (20.0, 500.0))
        CHECK (static_cast<double> (f.levelDb[0][static_cast<size_t> (b)]) < expect - 60.0);
    for (int b : bandsBetween (2000.0, 20000.0))
        CHECK (static_cast<double> (f.levelDb[0][static_cast<size_t> (b)]) < expect - 60.0);
}

TEST_CASE ("level spectrum: input level per layer follows the chain", "[analysis][impact][level]")
{
    Rig rig (2);
    rig.run (noise (0.25f, 7), { gain (0.5f), gain (1.0f) }, static_cast<int> (kFs * 3.0));
    const auto f = rig.latest();
    // Layer 1's input is layer 0's output: 6.02 dB lower in every band.
    for (int b : bandsBetween (100.0, 16000.0))
        CHECK (std::abs (static_cast<double> (f.levelDb[0][static_cast<size_t> (b)] - f.levelDb[1][static_cast<size_t> (b)]) - 6.02) < 0.2);
}

TEST_CASE ("level spectrum: silence reads the floor, never -inf", "[analysis][impact][level]")
{
    Rig rig (1);
    rig.run ([] { return StereoSample {}; }, { gain (1.0f) }, static_cast<int> (kFs * 1.0));
    const auto f = rig.latest();
    for (int b = 0; b < kImpactBands; ++b)
    {
        CHECK (std::isfinite (f.levelDb[0][static_cast<size_t> (b)]));
        CHECK (f.levelDb[0][static_cast<size_t> (b)] == kImpactLevelFloorDb);
    }
}
