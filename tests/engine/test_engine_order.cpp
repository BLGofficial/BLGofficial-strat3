// Engine layer order (module 18). Phase 5 routing rule, chosen by Blago 2026-11-04:
//   - the six depth layers run in any order; IN TRIM first, OUT TRIM / CEILING last
//   - neighbouring band layers (MB, WIDTH, SAT) share one split; a broadband layer between
//     them starts a new split, so MASTER latency grows by one FIR split per extra domain
//   - an order change is applied by prepare() (re-prepare on the message thread)
// Contracts tested here:
//   - an invalid order falls back to the default; rows follow the chain and sum to latency
//   - the impulse is never early and a neutral chain is flat in any order and mode
//   - the order changes the result (limiter before vs after EQ)
//   - delta stays aligned; "topmost" delta means first in the chain
//   - the sidechain meets MB on time wherever MB sits (MASTER)
//   - a MASTER crossover redesign reaches every split
//   - no allocation and block-size invariance with three splits
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>
#include <vector>

#include "engine/Engine.h"
#include "support/AllocCounter.h"
#include "support/Analysis.h"

using namespace strat3::engine;
namespace dsp = strat3::dsp;

namespace {

constexpr double kFs = 48000.0;
constexpr int kBlock = 512;

constexpr LayerOrder kTwoSplits { kMb, kEq, kWidth, kSat, kLow, kLimit };
constexpr LayerOrder kThreeSplits { kMb, kEq, kWidth, kLow, kSat, kLimit };
constexpr LayerOrder kReversed { kLimit, kSat, kWidth, kMb, kLow, kEq };
constexpr LayerOrder kBroadbandFirst { kEq, kLow, kLimit, kMb, kWidth, kSat };
constexpr LayerOrder kMbAfterFirs { kLow, kWidth, kEq, kMb, kSat, kLimit };   // MB in the 2nd split
constexpr LayerOrder kLimitFirst { kLimit, kEq, kLow, kMb, kWidth, kSat };

constexpr LatencyMode kModes[] { LatencyMode::Track, LatencyMode::Mix, LatencyMode::Master };
constexpr LayerOrder kOrders[] { kDefaultOrder, kTwoSplits, kThreeSplits, kReversed, kBroadbandFirst, kMbAfterFirs };

EngineParams neutral()
{
    EngineParams p;
    for (auto& s : p.layer)
        s = { true, 100.0f, false };
    p.ceilingOn = true;
    p.ceiling.ceilingDb = -1.0f;
    return p;
}

std::unique_ptr<Engine> make (LatencyMode mode, const EngineParams& p, const LayerOrder& order, int maxBlock = kBlock)
{
    auto e = std::make_unique<Engine>();
    EngineConfig c;
    c.sampleRate = kFs;
    c.maxBlock = maxBlock;
    c.mode = mode;
    c.order = order;
    prepare (*e, c, p);
    return e;
}

void run (Engine& e, std::vector<float>& l, std::vector<float>& r, int block = kBlock,
          const std::vector<float>* scL = nullptr, const std::vector<float>* scR = nullptr)
{
    const int n = static_cast<int> (l.size());
    for (int i = 0; i < n; i += block)
    {
        const int len = std::min (block, n - i);
        process (e, l.data() + i, r.data() + i, len,
                 scL ? scL->data() + i : nullptr, scR ? scR->data() + i : nullptr);
    }
}

std::vector<float> sine (double hz, float amp, int n)
{
    std::vector<float> x (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
        x[static_cast<size_t> (i)] = amp * static_cast<float> (std::sin (2.0 * std::numbers::pi * hz * i / kFs));
    return x;
}

double rms (const std::vector<float>& x, int from, int to)
{
    double s = 0.0;
    for (int i = from; i < to; ++i)
        s += static_cast<double> (x[static_cast<size_t> (i)]) * static_cast<double> (x[static_cast<size_t> (i)]);
    return std::sqrt (s / std::max (1, to - from));
}

std::vector<float> impulseResponse (Engine& e, int len, float amp = 0.25f)
{
    std::vector<float> l (static_cast<size_t> (len), 0.0f), r;
    l[0] = amp;
    r = l;
    run (e, l, r);
    for (auto& v : l)
        v /= amp;
    return l;
}

int argmaxAbs (const std::vector<float>& x)
{
    int m = 0;
    for (int i = 0; i < static_cast<int> (x.size()); ++i)
        if (std::abs (x[static_cast<size_t> (i)]) > std::abs (x[static_cast<size_t> (m)]))
            m = i;
    return m;
}

double sineGainDb (Engine& e, double hz, float amp = 0.1f, double settle = 0.4)
{
    const int n = static_cast<int> ((settle + 0.2) * kFs);
    auto l = sine (hz, amp, n), r = l;
    const auto in = l;
    run (e, l, r);
    const int from = static_cast<int> (settle * kFs);
    return 20.0 * std::log10 (rms (l, from, n) / rms (in, from, n) + 1e-30);
}

dsp::EqBand bell (float hz, float db, float q = 1.0f)
{
    dsp::EqBand b;
    b.on = true;
    b.type = dsp::EqType::Bell;
    b.freq = hz;
    b.gainDb = db;
    b.q = q;
    return b;
}

int sum (const std::array<int, kRows>& rows)
{
    int s = 0;
    for (int r : rows)
        s += r;
    return s;
}

} // namespace

// ------------------------------------------------------------------- structure

TEST_CASE ("order: an invalid order falls back to the default", "[engine][order]")
{
    constexpr LayerOrder duplicate { kEq, kEq, kMb, kWidth, kSat, kLimit };
    CHECK_FALSE (isValidOrder (duplicate));
    CHECK (isValidOrder (kThreeSplits));
    auto e = make (LatencyMode::Mix, neutral(), duplicate);
    CHECK (layerOrder (*e) == kDefaultOrder);
    auto f = make (LatencyMode::Mix, neutral(), kReversed);
    CHECK (layerOrder (*f) == kReversed);
}

TEST_CASE ("order: band layers share a split until a broadband layer separates them", "[engine][order]")
{
    const std::pair<LayerOrder, int> cases[] {
        { kDefaultOrder, 1 }, { kTwoSplits, 2 }, { kThreeSplits, 3 },
        { kReversed, 1 }, { kBroadbandFirst, 1 }, { kMbAfterFirs, 2 },
    };
    for (const auto& [order, splits] : cases)
    {
        auto e = make (LatencyMode::Mix, neutral(), order);
        CHECK (bandDomainCount (*e) == splits);
    }
}

TEST_CASE ("order: rows follow the chain and sum to the reported latency", "[engine][order][latency]")
{
    for (auto mode : kModes)
        for (const auto& order : kOrders)
        {
            auto e = make (mode, neutral(), order);
            const auto rows = rowLatencies (*e);
            CHECK (sum (rows) == latencySamples (*e));
            for (int i = 0; i < kDepthLayers; ++i)
                CHECK (rowOf (*e, order[static_cast<std::size_t> (i)]) == 1 + i);
            // The EQ row carries the FIR EQ latency wherever EQ sits.
            auto d = make (mode, neutral(), kDefaultOrder);
            CHECK (rows[static_cast<std::size_t> (rowOf (*e, kEq))] == rowLatencies (*d)[kRowEq]);
        }
}

TEST_CASE ("order: each extra split adds one FIR split of latency in MASTER only", "[engine][order][latency]")
{
    for (auto mode : kModes)
    {
        auto one = make (mode, neutral(), kDefaultOrder);
        auto two = make (mode, neutral(), kTwoSplits);
        auto three = make (mode, neutral(), kThreeSplits);
        const int split = rowLatencies (*one)[kRowMb];   // default order: the split heads MB's row
        INFO ("mode " << static_cast<int> (mode) << " split " << split);
        if (mode == LatencyMode::Master)
            CHECK (split > 1000);
        CHECK (latencySamples (*two) == latencySamples (*one) + split);
        CHECK (latencySamples (*three) == latencySamples (*one) + 2 * split);
    }
}

// ------------------------------------------------------------------- audio

TEST_CASE ("order: the impulse is never early and a neutral chain is flat, any order and mode", "[engine][order][latency]")
{
    for (auto mode : kModes)
        for (const auto& order : kOrders)
        {
            auto e = make (mode, neutral(), order);
            const int lat = latencySamples (*e);
            const auto ir = impulseResponse (*e, lat + 16384);
            const int at = argmaxAbs (ir);
            INFO ("mode " << static_cast<int> (mode) << " order head " << order[0] << " latency " << lat << " peak " << at);
            CHECK (at >= lat);
            // Each IIR split is an all-pass: it moves the impulse peak later without being
            // latency (measured: +7 for one split, +10 for two, +11 for three; FIR splits +5).
            CHECK (at - lat <= 8 + 3 * (bandDomainCount (*e) - 1));
            for (double hz : { 60.0, 250.0, 1000.0, 5000.0, 12000.0 })
            {
                INFO (hz << " Hz");
                CHECK (std::abs (strat3::test::magnitudeDb (ir, hz, kFs)) < 0.1);
            }
        }
}

TEST_CASE ("order: the order changes the result (limiter before vs after an EQ cut)", "[engine][order]")
{
    auto p = neutral();
    for (auto& s : p.layer)
        s.on = false;
    p.layer[kEq].on = true;
    p.layer[kLimit].on = true;
    p.eq[0] = bell (1000.0f, -12.0f);
    p.limit.threshDb = -12.0f;   // +12 dB makeup into a -1 dB ceiling

    auto eqFirst = make (LatencyMode::Mix, p, kDefaultOrder);
    auto limitFirst = make (LatencyMode::Mix, p, kLimitFirst);
    const double a = sineGainDb (*eqFirst, 1000.0, 0.5f);
    const double b = sineGainDb (*limitFirst, 1000.0, 0.5f);
    INFO ("eq first " << a << " dB, limit first " << b << " dB");
    CHECK (std::abs (a) < 1.0);   // -12 cut, +12 makeup, under the ceiling
    CHECK (b < -5.0);             // limited to the ceiling, then cut
}

TEST_CASE ("order: delta stays time-aligned when EQ sits between two splits (MASTER)", "[engine][order][delta]")
{
    for (bool global : { false, true })
    {
        auto p = neutral();
        p.eq[0] = bell (1000.0f, 6.0f, 0.7f);
        p.eqPhase = 100.0f;
        p.layer[kEq].delta = ! global;
        p.globalDelta = global;
        auto e = make (LatencyMode::Master, p, kThreeSplits);
        const int lat = latencySamples (*e);
        const auto ir = impulseResponse (*e, lat + 8192);
        const int at = argmaxAbs (ir);
        INFO ("global " << global << " latency " << lat << " peak " << at);
        CHECK (at >= lat);
        CHECK (at - lat <= 10);
    }
}

TEST_CASE ("order: global delta of a neutral chain is silent in any order", "[engine][order][delta]")
{
    for (const auto& order : kOrders)
    {
        auto p = neutral();
        p.globalDelta = true;
        auto e = make (LatencyMode::Mix, p, order);
        CHECK (sineGainDb (*e, 1000.0) < -80.0);
    }
}

TEST_CASE ("order: the topmost delta is the first layer with delta on in the chain", "[engine][order][delta]")
{
    auto p = neutral();
    p.eq[0] = bell (1000.0f, 6.0f);
    p.layer[kEq].delta = true;
    p.layer[kLimit].delta = true;   // a quiet sine: the limiter adds nothing
    auto eqTop = make (LatencyMode::Mix, p, kDefaultOrder);
    auto limitTop = make (LatencyMode::Mix, p, kLimitFirst);
    const double a = sineGainDb (*eqTop, 1000.0);
    const double b = sineGainDb (*limitTop, 1000.0);
    INFO ("eq on top " << a << " dB, limit on top " << b << " dB");
    CHECK (std::abs (a) < 0.3);   // EQ's addition: (2 - 1) x the input at +6 dB
    CHECK (b < -60.0);
}

TEST_CASE ("order: the sidechain meets the audio MB controls wherever MB sits (MASTER)", "[engine][order][sidechain]")
{
    for (const auto& order : { kDefaultOrder, kThreeSplits, kMbAfterFirs, kReversed })
    {
        auto p = neutral();
        for (auto& b : p.mb.bands)
        {
            b.threshDb = -30.0f;
            b.ratio = 8.0f;
            b.attackMs = 0.5f;
            b.releaseMs = 50.0f;
            b.detector = dsp::MbDetector::Peak;
        }
        p.mb.sidechain = dsp::MbSidechain::External;
        auto e = make (LatencyMode::Master, p, order);
        const int lat = latencySamples (*e);
        const int onset = 24000;
        const int n = onset + lat + 9600;
        auto l = sine (1000.0, 0.01f, n), r = l;
        const auto in = l;
        std::vector<float> sc (static_cast<size_t> (n), 0.0f);
        for (int i = onset; i < n; ++i)
            sc[static_cast<size_t> (i)] = 0.5f * static_cast<float> (std::sin (2.0 * std::numbers::pi * 1000.0 * i / kFs));
        run (*e, l, r, kBlock, &sc, &sc);
        const int ms = 48;
        const int at = onset + lat;
        const double before = 20.0 * std::log10 (rms (l, at - 20 * ms, at - 2 * ms) / rms (in, at - 20 * ms, at - 2 * ms));
        const double after = 20.0 * std::log10 (rms (l, at + 20 * ms, at + 60 * ms) / rms (in, at + 20 * ms, at + 60 * ms));
        INFO ("order head " << order[0] << " MB row " << rowOf (*e, kMb) << " latency " << lat
              << " before " << before << " after " << after);
        CHECK (std::abs (before) < 0.5);
        CHECK (after < -6.0);
    }
}

TEST_CASE ("order: a MASTER crossover redesign reaches every split", "[engine][order][master]")
{
    auto p = neutral();
    auto e = make (LatencyMode::Master, p, kThreeSplits);
    REQUIRE (bandDomainCount (*e) == 3);
    p.xover.f2 = 2000.0f;
    setParams (*e, p);
    CHECK (requestDesigns (*e, p));
    std::vector<float> l (static_cast<size_t> (kFs), 0.01f), r = l;
    run (*e, l, r);   // the swaps fade in over 50 ms
    for (int d = 0; d < 3; ++d)
    {
        INFO ("split " << d);
        CHECK (dsp::activeCrossovers (bandDomain (*e, d)).f2 == 2000.0f);
    }
    CHECK_FALSE (requestDesigns (*e, p));   // all designed: nothing left to do
}

TEST_CASE ("order: three splits never allocate and block size never changes the output", "[engine][order][rt]")
{
    auto p = neutral();
    p.eq[1] = bell (300.0f, 4.0f);
    p.mb.bands[1].threshDb = -30.0f;
    p.mb.bands[1].ratio = 4.0f;
    p.width.widthPercent[3] = 50.0f;
    p.sat.bands[2].driveDb = 6.0f;
    p.limit.threshDb = -6.0f;

    auto a = make (LatencyMode::Mix, p, kThreeSplits);
    auto b = make (LatencyMode::Mix, p, kThreeSplits);
    auto l1 = sine (440.0, 0.3f, 20000), r1 = sine (660.0, 0.3f, 20000);
    auto l2 = l1, r2 = r1;
    run (*a, l1, r1, 512);
    run (*b, l2, r2, 37);
    CHECK (l1 == l2);
    CHECK (r1 == r2);

    auto m = make (LatencyMode::Master, p, kThreeSplits);
    std::vector<float> l (kBlock, 0.1f), r (kBlock, 0.1f);
    strat3::test::ScopedAllocCounter counter;
    for (int k = 0; k < 16; ++k)
    {
        setParams (*m, p);
        process (*m, l.data(), r.data(), kBlock);
    }
    CHECK (counter.count() == 0);
}

TEST_CASE ("order: IIR crossover changes reach every split", "[engine][order]")
{
    auto p = neutral();
    auto e = make (LatencyMode::Mix, p, kThreeSplits);
    p.xover.f2 = 2000.0f;
    setParams (*e, p);
    for (int d = 0; d < bandDomainCount (*e); ++d)
    {
        INFO ("split " << d);
        CHECK (dsp::activeCrossovers (bandDomain (*e, d)).f2 == 2000.0f);
    }
}

TEST_CASE ("order: one non-finite detection clears every split, IIR and FIR", "[engine][order][stability]")
{
    for (auto mode : { LatencyMode::Mix, LatencyMode::Master })
    for (const auto& order : { kDefaultOrder, kThreeSplits })
    {
        auto p = neutral();
        auto e = make (mode, p, order);
        auto hot = p;
        hot.inTrimDb = 24.0f;
        setParams (*e, hot);
        std::vector<float> big (kBlock, 3.0e38f), big2 = big;
        for (int b = 0; b < 4; ++b)
        {
            auto x = big, y = big2;
            process (*e, x.data(), y.data(), kBlock);
        }
        takeNonFiniteFlag (*e);
        setParams (*e, p);
        // Only the output is checked, so a value still inside the latency (up to ~200 ms in
        // MASTER) is caught when it arrives. One detection must clear every split: a second
        // one means a state was left holding it.
        const int n = 48000;
        auto l = sine (440.0, 0.3f, n), r = l;
        int detections = 0;
        for (int i = 0; i < n; i += kBlock)
        {
            const int len = std::min (kBlock, n - i);
            process (*e, l.data() + i, r.data() + i, len);
            detections += takeNonFiniteFlag (*e) ? 1 : 0;
        }
        INFO ("mode " << static_cast<int> (mode) << " splits " << bandDomainCount (*e));
        CHECK (detections <= 1);
        CHECK (rms (l, n / 2, n) > 0.1);
    }
}

TEST_CASE ("order: a band layer at depth 0 passes its input on to the next band layer", "[engine][order][depth]")
{
    // The next band layer must get the blended bands, not the layer's raw (wet) output.
    for (auto k : { kMb, kWidth, kSat })
    {
        auto a = neutral();
        for (auto& b : a.mb.bands)
        {
            b.threshDb = -40.0f;
            b.ratio = 10.0f;
        }
        for (auto& w : a.width.widthPercent)
            w = 80.0f;
        for (auto& b : a.sat.bands)
            b.driveDb = 12.0f;
        // Reference: the same layer at depth 0 with neutral settings. Off is not a reference:
        // the layer still runs (and rewrites its bands) when off, exactly as at depth 0.
        auto b = a;
        a.layer[static_cast<std::size_t> (k)].depth = 0.0f;
        b.layer[static_cast<std::size_t> (k)].depth = 0.0f;
        if (k == kMb)
            b.mb = {};
        else if (k == kWidth)
            b.width = {};
        else
            b.sat = {};
        // k first in its run, then the other two band layers.
        LayerOrder order { kEq, kLow, k, kMb, kWidth, kSat };
        std::size_t w = 3;
        for (auto x : { kMb, kWidth, kSat })
            if (x != k)
                order[w++] = x;
        order[5] = kLimit;
        REQUIRE (isValidOrder (order));
        auto ea = make (LatencyMode::Mix, a, order), eb = make (LatencyMode::Mix, b, order);
        auto la = sine (1000.0, 0.3f, 9600), ra = sine (700.0, 0.3f, 9600), lb = la, rb = ra;
        run (*ea, la, ra);
        run (*eb, lb, rb);
        float worst = 0.0f;
        for (std::size_t i = 0; i < la.size(); ++i)
            worst = std::max ({ worst, std::abs (la[i] - lb[i]), std::abs (ra[i] - rb[i]) });
        INFO ("layer " << k << " worst difference " << worst);
        CHECK (worst < 1e-5f);
    }
}

