// Engine: the fixed-order chain that wires every layer (module 16, headless; the plugin
// wrapper is module 17).
//   IN TRIM -> EQ -> LOW -> [MB -> WIDTH -> SAT: one band split] -> LIMIT -> OUT TRIM
//   -> monitor (normal / delta) -> CEILING -> gain match (monitor only)
// Contracts tested here:
//   - reported latency = sum of the per-row latencies, fixed per mode: toggling a layer or
//     its depth never changes it, and the impulse lands where the host is told it lands
//   - a neutral chain is flat; depth scales a layer without comb filtering (dry aligned)
//   - LOW, SAT and LIMIT blend against their reference output, others against their input
//   - ceiling.dBTP also sets the limiter's sample-peak ceiling
//   - delta: the topmost layer with delta on is heard as what it adds; the layers below it
//     are bypassed (but keep their latency). Global delta = sum of every layer's addition.
//   - gain match is applied after the ceiling, and never to a delta signal
//   - a non-finite sample zeroes the block, raises a flag, and the chain recovers
//   - no allocation in setParams / process; block size never changes the output
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <numbers>
#include <numeric>
#include <thread>
#include <vector>

#include "analysis/TruePeak.h"
#include "engine/Engine.h"
#include "support/AllocCounter.h"
#include "support/Analysis.h"

using namespace strat3::engine;
namespace dsp = strat3::dsp;

namespace {

constexpr double kFs = 48000.0;
constexpr int kBlock = 512;

EngineParams neutral()
{
    EngineParams p;
    for (auto& s : p.layer)
        s = { true, 100.0f, false };
    p.ceilingOn = true;
    p.ceiling.ceilingDb = -1.0f;
    return p;
}

std::unique_ptr<Engine> make (LatencyMode mode, const EngineParams& p, double fs = kFs, int satOs = 2)
{
    auto e = std::make_unique<Engine>();
    EngineConfig c;
    c.sampleRate = fs;
    c.maxBlock = kBlock;
    c.mode = mode;
    c.satOs = satOs;
    prepare (*e, c, p);
    return e;
}

/** Runs l/r through the engine in blocks (same params every block). */
void run (Engine& e, std::vector<float>& l, std::vector<float>& r, int block = kBlock,
          const std::vector<float>* scL = nullptr, const std::vector<float>* scR = nullptr)
{
    const int n = static_cast<int> (l.size());
    for (int i = 0; i < n; i += block)
    {
        const int len = std::min (block, n - i);
        const float* sl = scL ? scL->data() + i : nullptr;
        const float* sr = scR ? scR->data() + i : nullptr;
        process (e, l.data() + i, r.data() + i, len, sl, sr);
    }
}

std::vector<float> sine (double hz, float amp, int n, double fs = kFs)
{
    std::vector<float> x (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
        x[static_cast<size_t> (i)] = amp * static_cast<float> (std::sin (2.0 * std::numbers::pi * hz * i / fs));
    return x;
}

double rms (const std::vector<float>& x, int from, int to)
{
    double s = 0.0;
    for (int i = from; i < to; ++i)
        s += static_cast<double> (x[static_cast<size_t> (i)]) * static_cast<double> (x[static_cast<size_t> (i)]);
    return std::sqrt (s / std::max (1, to - from));
}

float peak (const std::vector<float>& x, int from, int to)
{
    float m = 0.0f;
    for (int i = from; i < to; ++i)
        m = std::max (m, std::abs (x[static_cast<size_t> (i)]));
    return m;
}

/** Left-channel impulse response (L = R input), normalised to the impulse amplitude. */
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

/** Steady-state gain in dB of a sine through the engine (after `settle` seconds). */
double sineGainDb (Engine& e, double hz, float amp = 0.1f, double settle = 0.4)
{
    const int n = static_cast<int> ((settle + 0.2) * kFs);
    auto l = sine (hz, amp, n), r = l;
    const auto in = l;
    run (e, l, r);
    const int from = static_cast<int> (settle * kFs);
    return 20.0 * std::log10 (rms (l, from, n) / rms (in, from, n));
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

constexpr LatencyMode kModes[] { LatencyMode::Track, LatencyMode::Mix, LatencyMode::Master };

} // namespace

// ------------------------------------------------------------------- latency

TEST_CASE ("engine: reported latency is the sum of the rows and never moves with on/off or depth", "[engine][latency]")
{
    int previous = -1;
    for (auto mode : kModes)
    {
        auto p = neutral();
        auto e = make (mode, p);
        const int lat = latencySamples (*e);
        const auto rows = rowLatencies (*e);
        CHECK (std::accumulate (rows.begin(), rows.end(), 0) == lat);
        CHECK (lat > previous);   // TRACK < MIX < MASTER
        previous = lat;

        for (auto& s : p.layer)
            s = { false, 0.0f, false };
        p.ceilingOn = false;
        setParams (*e, p);
        CHECK (latencySamples (*e) == lat);
        p.layer[kEq].depth = 150.0f;
        p.layer[kSat].delta = true;
        setParams (*e, p);
        CHECK (latencySamples (*e) == lat);
    }
}

TEST_CASE ("engine: the impulse never arrives before the reported latency", "[engine][latency]")
{
    for (auto mode : kModes)
    {
        auto e = make (mode, neutral());
        const int lat = latencySamples (*e);
        const auto ir = impulseResponse (*e, lat + 8192);
        int argmax = 0;
        for (int i = 0; i < static_cast<int> (ir.size()); ++i)
            if (std::abs (ir[static_cast<size_t> (i)]) > std::abs (ir[static_cast<size_t> (argmax)]))
                argmax = i;
        INFO ("mode " << static_cast<int> (mode) << " latency " << lat << " peak at " << argmax);
        // Never early: reporting more latency than the chain has would put the plugin ahead of
        // the host's other tracks. Late only by the IIR all-pass smear (phase, not delay): the
        // limiter's fixed 4 kHz split and the IIR halfbands move the peak by ~5-7 samples
        // (measured per component). 10 samples = 0.2 ms [INFERRED bound].
        CHECK (argmax >= lat);
        CHECK (argmax - lat <= 10);

        // Everything off, ceiling included: still never early (the bypasses keep the delay).
        auto off = neutral();
        for (auto& s : off.layer)
            s.on = false;
        off.ceilingOn = false;
        auto eo = make (mode, off);
        const auto iro = impulseResponse (*eo, lat + 8192);
        int am = 0;
        for (int i = 0; i < static_cast<int> (iro.size()); ++i)
            if (std::abs (iro[static_cast<size_t> (i)]) > std::abs (iro[static_cast<size_t> (am)]))
                am = i;
        CHECK (latencySamples (*eo) == lat);
        CHECK (am >= lat);
        CHECK (am - lat <= 10);
    }
}

TEST_CASE ("engine: a delta signal is time-aligned with the normal output (MASTER)", "[engine][latency][delta]")
{
    for (bool global : { false, true })
    {
        auto p = neutral();
        p.eq[0] = bell (1000.0f, 6.0f, 0.7f);
        p.eqPhase = 100.0f;   // linear phase: the difference kernel peaks at its centre
        p.layer[kEq].delta = ! global;
        p.globalDelta = global;
        auto e = make (LatencyMode::Master, p);
        const int lat = latencySamples (*e);
        const auto ir = impulseResponse (*e, lat + 8192);
        int argmax = 0;
        for (int i = 0; i < static_cast<int> (ir.size()); ++i)
            if (std::abs (ir[static_cast<size_t> (i)]) > std::abs (ir[static_cast<size_t> (argmax)]))
                argmax = i;
        INFO ("global " << global << " latency " << lat << " peak at " << argmax);
        CHECK (argmax >= lat);
        CHECK (argmax - lat <= 10);
    }
}

// ------------------------------------------------------------------- neutral and depth

TEST_CASE ("engine: a neutral chain is flat in every mode", "[engine][neutral]")
{
    for (auto mode : kModes)
    {
        auto e = make (mode, neutral());
        const auto ir = impulseResponse (*e, latencySamples (*e) + 16384);
        for (double hz : { 25.0, 60.0, 100.0, 120.0, 250.0, 1000.0, 4000.0, 5000.0, 10000.0, 18000.0 })
        {
            INFO ("mode " << static_cast<int> (mode) << " at " << hz << " Hz");
            CHECK (std::abs (strat3::test::magnitudeDb (ir, hz, kFs)) < 0.1);
        }
    }
}

TEST_CASE ("engine: depth 0 on a layer is bit-exact with the layer off", "[engine][depth]")
{
    auto a = neutral(), b = neutral();
    a.eq[0] = b.eq[0] = bell (1000.0f, 6.0f);
    a.layer[kEq].depth = 0.0f;
    b.layer[kEq].on = false;
    auto ea = make (LatencyMode::Mix, a), eb = make (LatencyMode::Mix, b);
    auto la = sine (1000.0, 0.1f, 9600), ra = la, lb = la, rb = la;
    run (*ea, la, ra);
    run (*eb, lb, rb);
    CHECK (la == lb);
    CHECK (ra == rb);
}

TEST_CASE ("engine: EQ depth scales the boost with the dry path aligned (no comb), MIX and MASTER", "[engine][depth]")
{
    for (auto mode : { LatencyMode::Mix, LatencyMode::Master })
    {
        auto p = neutral();
        p.eq[0] = bell (1000.0f, 6.0f);
        p.eqPhase = 100.0f;
        auto full = make (mode, p);
        INFO ("mode " << static_cast<int> (mode));
        CHECK (std::abs (sineGainDb (*full, 1000.0) - 6.0) < 0.1);   // applied once, not IIR + FIR
        // Depth is in place from the first sample after prepare (no fade-in from 0).
        auto first = make (mode, p);
        const auto irFirst = impulseResponse (*first, latencySamples (*first) + 16384);
        CHECK (std::abs (strat3::test::magnitudeDb (irFirst, 1000.0, kFs) - 6.0) < 0.1);

        p.layer[kEq].depth = 50.0f;
        auto half = make (mode, p);
        CHECK (std::abs (sineGainDb (*half, 1000.0) - 20.0 * std::log10 (1.5)) < 0.1);
        // Far from the bell both paths are ~0 dB; a misaligned dry path would comb here.
        auto fresh = make (mode, p);   // no sine still in flight inside the latency
        const auto ir = impulseResponse (*fresh, latencySamples (*fresh) + 16384);
        for (double hz : { 60.0, 150.0, 8000.0, 12000.0 })
            CHECK (std::abs (strat3::test::magnitudeDb (ir, hz, kFs)) < 0.2);
    }
}

TEST_CASE ("engine: dynamic EQ nodes work in every mode (MASTER keeps them in the IIR)", "[engine][eq]")
{
    for (auto mode : kModes)
    {
        auto p = neutral();
        p.eq[0] = bell (1000.0f, 0.0f);
        p.eq[0].dyn = dsp::EqDynMode::Down;
        p.eq[0].threshDb = -40.0f;
        p.eq[0].rangeDb = -6.0f;
        p.eq[0].attackMs = 1.0f;
        auto e = make (mode, p);
        INFO ("mode " << static_cast<int> (mode));
        CHECK (sineGainDb (*e, 1000.0, 0.3f) < -4.0);
    }
}

TEST_CASE ("engine: WIDTH scales the side signal inside the band split", "[engine][width]")
{
    auto p = neutral();
    p.width.widthPercent = { 100.0f, 100.0f, 100.0f, 100.0f };   // S x 2
    auto e = make (LatencyMode::Mix, p);
    const int n = 24000;
    auto l = sine (1000.0, 0.2f, n);
    std::vector<float> r (static_cast<size_t> (n), 0.0f);   // M = S = l / 2
    const auto in = l;
    run (*e, l, r);
    // L = M + 2S = 1.5 x, R = M - 2S = -0.5 x
    CHECK (std::abs (rms (r, n / 2, n) / rms (in, n / 2, n) - 0.5) < 0.03);
    CHECK (std::abs (rms (l, n / 2, n) / rms (in, n / 2, n) - 1.5) < 0.03);
}

// ------------------------------------------------------------------- limiter / ceiling wiring

TEST_CASE ("engine: ceiling.dBTP is also the limiter's sample-peak ceiling", "[engine][limit]")
{
    auto p = neutral();
    p.limit.threshDb = -6.0f;     // +6 dB makeup into a 0 dBFS sine
    p.ceiling.ceilingDb = -2.0f;
    p.ceilingOn = false;          // the limiter alone must hold -2 dB
    auto e = make (LatencyMode::Mix, p);
    const int n = static_cast<int> (0.8 * kFs);
    auto l = sine (997.0, 1.0f, n), r = l;
    run (*e, l, r);
    CHECK (peak (l, n / 2, n) <= std::pow (10.0f, -2.0f / 20.0f) * 1.002f);
}

TEST_CASE ("engine: the ceiling layer guards true peak; off, it only delays", "[engine][ceiling]")
{
    auto p = neutral();
    p.ceiling.ceilingDb = -1.0f;
    p.layer[kLimit].on = false;   // nothing upstream limits
    const int n = static_cast<int> (0.6 * kFs);
    auto measureTp = [&] (bool on, double phase)
    {
        p.ceilingOn = on;
        auto e = make (LatencyMode::Mix, p);
        // 0 dBTP at fs/4: depending on where the samples fall they sit up to 3 dB under the
        // true peak. The upstream all-passes rotate the phase, so sweep it: for some phases
        // only a true-peak detector (the look-ahead ceiling) sees the overs.
        std::vector<float> l (static_cast<size_t> (n));
        for (int i = 0; i < n; ++i)
            l[static_cast<size_t> (i)] = static_cast<float> (std::sin (2.0 * std::numbers::pi * 12000.0 * i / kFs + phase));
        auto r = l;
        run (*e, l, r);
        strat3::analysis::TruePeakDetector d;
        float tp = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float v = strat3::analysis::push (d, l[static_cast<size_t> (i)]);
            if (i > n / 2)
                tp = std::max (tp, v);
        }
        return 20.0f * std::log10 (tp);
    };
    float worstOn = -100.0f, bestOff = 100.0f;
    for (int k = 0; k < 8; ++k)   // 11.25 degree steps over a quarter period
    {
        const double phase = 0.5 * std::numbers::pi * k / 8.0;
        worstOn = std::max (worstOn, measureTp (true, phase));
        bestOff = std::min (bestOff, measureTp (false, phase));
    }
    CHECK (worstOn <= -0.9f);
    CHECK (bestOff > -0.3f);   // off really is off
}

// ------------------------------------------------------------------- trims

TEST_CASE ("engine: in and out trims are gains in dB, ramped over 20 ms", "[engine][trim]")
{
    auto p = neutral();
    p.inTrimDb = 6.0f;
    auto e = make (LatencyMode::Mix, p);
    CHECK (std::abs (sineGainDb (*e, 1000.0) - 6.0) < 0.05);
    p.inTrimDb = 0.0f;
    p.outTrimDb = -6.0f;
    auto f = make (LatencyMode::Mix, p);
    CHECK (std::abs (sineGainDb (*f, 1000.0) - -6.0) < 0.05);
    p.outTrimDb = 0.0f;
    p.inTrimDb = 40.0f;   // out of range: clamped to +24
    auto h = make (LatencyMode::Mix, p);
    CHECK (std::abs (sineGainDb (*h, 1000.0, 0.001f) - 24.0) < 0.05);
    p.inTrimDb = 0.0f;

    // A change during playback ramps: 5 ms in, ~+3 dB of +12; 30 ms in, all of it.
    p.outTrimDb = 0.0f;
    auto g = make (LatencyMode::Track, p);
    const int lat = latencySamples (*g);
    const int n = static_cast<int> (0.3 * kFs);
    auto l = sine (1000.0, 0.01f, n), r = l;
    const int change = 4800;
    process (*g, l.data(), r.data(), change);
    p.inTrimDb = 12.0f;
    setParams (*g, p);
    for (int i = change; i < n; i += kBlock)
        process (*g, l.data() + i, r.data() + i, std::min (kBlock, n - i));
    const int ms = static_cast<int> (kFs / 1000.0);
    const double early = 20.0 * std::log10 (rms (l, change + lat + 4 * ms, change + lat + 6 * ms) / (0.01 / std::sqrt (2.0)));
    const double late = 20.0 * std::log10 (rms (l, change + lat + 30 * ms, change + lat + 50 * ms) / (0.01 / std::sqrt (2.0)));
    CHECK (early > 1.5);
    CHECK (early < 5.0);
    CHECK (std::abs (late - 12.0) < 0.1);
}

// ------------------------------------------------------------------- delta monitoring

TEST_CASE ("engine: layer delta plays what the layer adds, with the layers below bypassed", "[engine][delta]")
{
    auto p = neutral();
    p.eq[0] = bell (1000.0f, 6.0f);
    p.layer[kEq].delta = true;
    p.limit.threshDb = -12.0f;   // +12 dB makeup: must not reach the delta signal
    auto e = make (LatencyMode::Mix, p);
    // At the bell centre the IIR bell has zero phase: wet - dry = (2 - 1) * x
    CHECK (std::abs (sineGainDb (*e, 1000.0) - 0.0) < 0.2);
    auto f = make (LatencyMode::Mix, p);
    CHECK (sineGainDb (*f, 15000.0) < -30.0);   // the bell adds nothing up there

    // The topmost layer with delta on wins.
    p.layer[kLimit].delta = true;
    auto g = make (LatencyMode::Mix, p);
    CHECK (std::abs (sineGainDb (*g, 1000.0) - 0.0) < 0.2);
}

TEST_CASE ("engine: global delta is the sum of every layer's addition; a neutral chain is silent", "[engine][delta]")
{
    auto p = neutral();
    p.globalDelta = true;
    auto e = make (LatencyMode::Mix, p);
    CHECK (sineGainDb (*e, 1000.0) < -80.0);

    p.eq[0] = bell (1000.0f, 6.0f);
    auto f = make (LatencyMode::Mix, p);
    CHECK (std::abs (sineGainDb (*f, 1000.0) - 0.0) < 0.2);

    // A layer's own delta wins over global: the limiter's +12 dB makeup is not heard.
    p.layer[kEq].delta = true;
    p.limit.threshDb = -12.0f;
    auto both = make (LatencyMode::Mix, p);
    CHECK (std::abs (sineGainDb (*both, 1000.0) - 0.0) < 0.2);
    p.layer[kEq].delta = false;
    p.limit.threshDb = 0.0f;

    p.eq[0].on = false;
    p.inTrimDb = 6.0f;   // trims are not layers: not in the delta
    auto g = make (LatencyMode::Mix, p);
    CHECK (sineGainDb (*g, 1000.0) < -80.0);
}

TEST_CASE ("engine: switching delta crossfades instead of jumping", "[engine][delta]")
{
    auto p = neutral();
    p.eq[0] = bell (1000.0f, 6.0f);
    auto e = make (LatencyMode::Track, p);
    const int n = 24000;
    auto l = sine (1000.0, 0.1f, n), r = l;
    // Switch where the monitored sine is at its peak (12 samples = a quarter period, plus
    // the TRACK latency), so a hard switch would jump by the full 0.1 difference.
    const int at = 9612 + latencySamples (*e);
    process (*e, l.data(), r.data(), at);
    p.layer[kEq].delta = true;
    setParams (*e, p);
    for (int i = at; i < n; i += kBlock)
        process (*e, l.data() + i, r.data() + i, std::min (kBlock, n - i));
    float jump = 0.0f;
    for (int i = at - 100; i < n; ++i)
        jump = std::max (jump, std::abs (l[static_cast<size_t> (i)] - l[static_cast<size_t> (i - 1)]));
    // A 1 kHz sine of 0.2 peak moves at most 0.026 per sample; a hard switch from 0.2 to 0.1
    // amplitude mid-cycle would jump by up to ~0.1.
    CHECK (jump < 0.03f);
}

// ------------------------------------------------------------------- gain match

TEST_CASE ("engine: gain match applies the measured offset after the ceiling, never to deltas", "[engine][gainmatch]")
{
    auto p = neutral();
    p.gainMatch = true;
    auto e = make (LatencyMode::Mix, p);
    setGainMatchDb (*e, -6.0f);
    CHECK (std::abs (sineGainDb (*e, 1000.0) - -6.0) < 0.05);

    p.gainMatch = false;
    auto f = make (LatencyMode::Mix, p);
    setGainMatchDb (*f, -6.0f);
    CHECK (std::abs (sineGainDb (*f, 1000.0)) < 0.05);

    p.gainMatch = true;
    p.eq[0] = bell (1000.0f, 6.0f);
    p.layer[kEq].delta = true;
    auto g = make (LatencyMode::Mix, p);
    setGainMatchDb (*g, -6.0f);
    CHECK (std::abs (sineGainDb (*g, 1000.0)) < 0.2);

    auto h = make (LatencyMode::Mix, neutral());
    setGainMatchDb (*h, 60.0f);                                     // clamped to +24
    setGainMatchDb (*h, std::numeric_limits<float>::quiet_NaN());   // ignored: +24 stays
    auto q = neutral();
    q.gainMatch = true;
    setParams (*h, q);
    CHECK (std::abs (sineGainDb (*h, 1000.0, 0.001f) - 24.0) < 0.1);
}

// ------------------------------------------------------------------- robustness

TEST_CASE ("engine: a non-finite sample zeroes the block, raises the flag, and the chain recovers", "[engine][stability]")
{
    auto p = neutral();
    p.eq[0] = bell (300.0f, 4.0f);
    p.limit.threshDb = -3.0f;
    auto e = make (LatencyMode::Mix, p);
    auto l = sine (440.0, 0.3f, kBlock), r = l;
    l[100] = std::numeric_limits<float>::quiet_NaN();
    process (*e, l.data(), r.data(), kBlock);
    for (int i = 0; i < kBlock; ++i)
    {
        CHECK (l[static_cast<size_t> (i)] == 0.0f);
        CHECK (r[static_cast<size_t> (i)] == 0.0f);
    }
    CHECK (takeNonFiniteFlag (*e));
    CHECK_FALSE (takeNonFiniteFlag (*e));
    // The bad sample never reached the analysis taps either.
    std::vector<float> frame (static_cast<size_t> (e->capture.stride));
    while (strat3::analysis::popFrame (e->capture, frame.data()))
        for (float v : frame)
            REQUIRE (std::isfinite (v));

    const int n = 48000;
    auto l2 = sine (440.0, 0.3f, n), r2 = l2;
    run (*e, l2, r2);
    for (float v : l2)
        REQUIRE (std::isfinite (v));
    CHECK (rms (l2, n / 2, n) > 0.1);
    CHECK_FALSE (takeNonFiniteFlag (*e));

    // An overflow inside the chain (finite input, +24 dB trim): muted, then the states
    // recover instead of staying non-finite.
    auto hot = p;
    hot.inTrimDb = 24.0f;
    setParams (*e, hot);
    std::vector<float> big (kBlock, 3.0e38f), big2 = big;
    for (int b = 0; b < 4; ++b)
    {
        auto x = big, y = big2;
        process (*e, x.data(), y.data(), kBlock);
    }
    CHECK (takeNonFiniteFlag (*e));
    setParams (*e, p);
    auto l4 = sine (440.0, 0.3f, n), r4 = l4;
    run (*e, l4, r4);
    CHECK (rms (l4, n / 2, n) > 0.1);
    takeNonFiniteFlag (*e);

    // A non-finite sidechain is ignored, not passed on (no muting, no flag).
    auto q = neutral();
    q.mb.sidechain = dsp::MbSidechain::External;
    setParams (*e, q);
    auto l3 = sine (440.0, 0.3f, n), r3 = l3;
    std::vector<float> sc (static_cast<size_t> (n), std::numeric_limits<float>::infinity());
    run (*e, l3, r3, kBlock, &sc, &sc);
    for (float v : l3)
        REQUIRE (std::isfinite (v));
    CHECK (rms (l3, n / 2, n) > 0.1);
    CHECK_FALSE (takeNonFiniteFlag (*e));
}

TEST_CASE ("engine: the external sidechain drives the multiband detector", "[engine][sidechain]")
{
    auto p = neutral();
    for (auto& b : p.mb.bands)
    {
        b.threshDb = -30.0f;
        b.ratio = 8.0f;
        b.attackMs = 1.0f;
        b.releaseMs = 50.0f;
        b.detector = dsp::MbDetector::Peak;
    }
    p.mb.sidechain = dsp::MbSidechain::External;
    const int n = static_cast<int> (0.6 * kFs);
    const auto loud = sine (1000.0, 0.5f, n);
    auto gainWith = [&] (const dsp::MbSettings& mb, bool feed)
    {
        auto q = p;
        q.mb = mb;
        auto e = make (LatencyMode::Mix, q);
        auto l = sine (1000.0, 0.01f, n), r = l;   // -40 dBFS: below threshold on its own
        const auto in = l;
        run (*e, l, r, kBlock, feed ? &loud : nullptr, feed ? &loud : nullptr);
        return 20.0 * std::log10 (rms (l, n / 2, n) / rms (in, n / 2, n));
    };
    CHECK (gainWith (p.mb, true) < -6.0);
    CHECK (std::abs (gainWith (p.mb, false)) < 0.1);   // no sidechain bus: internal
    auto internal = p.mb;
    internal.sidechain = dsp::MbSidechain::Internal;
    CHECK (std::abs (gainWith (internal, true)) < 0.1);
}

TEST_CASE ("engine: the sidechain meets the audio it controls on time (MASTER)", "[engine][sidechain][latency]")
{
    // MASTER delays the main path by the FIR EQ and the FIR split before MB; a sidechain
    // burst must start compressing exactly when the audio of that moment comes out.
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
    auto e = make (LatencyMode::Master, p);
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
    INFO ("latency " << lat << " before " << before << " after " << after);
    CHECK (std::abs (before) < 0.5);
    CHECK (after < -6.0);
}

TEST_CASE ("engine: block size never changes the output", "[engine][blocks]")
{
    auto p = neutral();
    p.eq[0] = bell (2000.0f, -4.0f);
    p.eq[1] = bell (150.0f, 3.0f);
    p.eq[1].dyn = dsp::EqDynMode::Down;
    p.low.punch = 40.0f;
    p.mb.bands[1].threshDb = -20.0f;
    p.mb.bands[1].ratio = 4.0f;
    p.width.widthPercent = { 0.0f, 20.0f, 40.0f, 60.0f };
    p.sat.bands[2].driveDb = 6.0f;
    p.limit.threshDb = -6.0f;
    for (auto mode : kModes)
    {
        auto a = make (mode, p), b = make (mode, p);
        const int n = 24000;
        auto la = sine (220.0, 0.4f, n), ra = sine (330.0, 0.3f, n);
        auto lb = la, rb = ra;
        run (*a, la, ra, kBlock);
        run (*b, lb, rb, 1);
        float d = 0.0f;
        for (int i = 0; i < n; ++i)
            d = std::max ({ d, std::abs (la[static_cast<size_t> (i)] - lb[static_cast<size_t> (i)]),
                            std::abs (ra[static_cast<size_t> (i)] - rb[static_cast<size_t> (i)]) });
        INFO ("mode " << static_cast<int> (mode));
        CHECK (d < 1e-6f);
    }
}

TEST_CASE ("engine: prepare again at another rate and mode is safe", "[engine][prepare]")
{
    auto p = neutral();
    p.eq[0] = bell (1000.0f, 3.0f);
    auto e = make (LatencyMode::Mix, p, 44100.0);
    const int lat441 = latencySamples (*e);
    auto l = sine (1000.0, 0.2f, 8820, 44100.0), r = l;
    run (*e, l, r);
    EngineConfig c;
    c.sampleRate = 96000.0;
    c.maxBlock = 1024;
    c.mode = LatencyMode::Master;
    c.satOs = 4;
    prepare (*e, c, p);
    CHECK (latencySamples (*e) != lat441);
    const auto rows = rowLatencies (*e);
    CHECK (std::accumulate (rows.begin(), rows.end(), 0) == latencySamples (*e));
    auto l2 = sine (1000.0, 0.2f, 19200, 96000.0), r2 = l2;
    run (*e, l2, r2, 1024);   // last block is short: never past the buffer
    for (float v : l2)
        REQUIRE (std::isfinite (v));
    CHECK (rms (l2, 9600, 19200) > 0.1);
}

TEST_CASE ("engine: saturation oversampling is capped so the oversampled rate stays <= 192 kHz", "[engine][prepare]")
{
    CHECK (effectiveSatOs (*make (LatencyMode::Mix, neutral(), 48000.0, 4)) == 4);
    CHECK (effectiveSatOs (*make (LatencyMode::Mix, neutral(), 44100.0, 2)) == 2);
    CHECK (effectiveSatOs (*make (LatencyMode::Mix, neutral(), 96000.0, 4)) == 2);
    CHECK (effectiveSatOs (*make (LatencyMode::Mix, neutral(), 88200.0, 4)) == 2);
    CHECK (effectiveSatOs (*make (LatencyMode::Mix, neutral(), 192000.0, 4)) == 1);
    CHECK (effectiveSatOs (*make (LatencyMode::Mix, neutral(), 192000.0, 1)) == 1);
    CHECK (effectiveSatOs (*make (LatencyMode::Mix, neutral(), 48000.0, 3)) == 2);   // not a power of two
}

// ------------------------------------------------------------------- MASTER designs

TEST_CASE ("engine: MASTER static EQ and crossovers change only through an off-thread design", "[engine][master]")
{
    auto p = neutral();
    auto e = make (LatencyMode::Master, p);
    p.eq[0] = bell (1000.0f, 6.0f);
    setParams (*e, p);
    // The static band lives in the FIR: nothing changes until a design is requested.
    CHECK (std::abs (sineGainDb (*e, 1000.0, 0.1f, 0.2)) < 0.1);
    CHECK (requestDesigns (*e, p));
    CHECK (std::abs (sineGainDb (*e, 1000.0, 0.1f, 0.3) - 6.0) < 0.1);   // 50 ms crossfade done
    CHECK_FALSE (requestDesigns (*e, p));                                 // nothing new

    p.xover.f2 = 2000.0f;
    CHECK (requestDesigns (*e, p));
    sineGainDb (*e, 1000.0, 0.1f, 0.2);   // let the swap run
    CHECK (strat3::dsp::activeCrossovers (bandDomain (*e, 0)).f2 == 2000.0f);
    auto mix = make (LatencyMode::Mix, p);
    CHECK_FALSE (requestDesigns (*mix, p));   // IIR modes never need a design
}

TEST_CASE ("engine: the designer thread runs while the audio thread processes (MASTER)", "[engine][master][threads]")
{
    // Run under TSAN: requestDesigns() and setGainMatchDb() race process() by design.
    auto p = neutral();
    p.eq[0] = bell (1000.0f, 0.0f);
    p.gainMatch = true;
    auto e = make (LatencyMode::Master, p);
    std::atomic<bool> stop { false };
    std::atomic<int> designs { 0 };
    std::thread designer ([&]
    {
        auto q = p;
        for (int i = 0; ! stop.load(); ++i)
        {
            q.eq[0].gainDb = static_cast<float> (i % 7) - 3.0f;
            q.xover.f2 = 900.0f + static_cast<float> (i % 5) * 50.0f;
            if (requestDesigns (*e, q))
                designs.fetch_add (1);
            setGainMatchDb (*e, -static_cast<float> (i % 3));
            std::this_thread::sleep_for (std::chrono::milliseconds (2));
        }
    });
    auto l = sine (700.0, 0.2f, kBlock), r = l;
    for (int b = 0; b < 1500 && designs.load() < 4; ++b)
    {
        auto x = l, y = r;
        process (*e, x.data(), y.data(), kBlock);
        for (float v : x)
            REQUIRE (std::isfinite (v));
    }
    stop.store (true);
    designer.join();
    CHECK (designs.load() >= 4);   // swaps completed and the designer got the slot back
}

// ------------------------------------------------------------------- analysis taps

TEST_CASE ("engine: impact taps carry every row boundary; loudness taps see input and output", "[engine][taps]")
{
    auto p = neutral();
    p.eq[0] = bell (1000.0f, 6.0f);
    p.inTrimDb = 6.0f;
    auto e = make (LatencyMode::Mix, p);
    CHECK (e->capture.taps == kRows + 1);
    const int n = 9600;
    auto l = sine (1000.0, 0.05f, n), r = l;
    run (*e, l, r);

    std::vector<float> frame (static_cast<size_t> (e->capture.stride));
    std::vector<double> m0, m1, m2;   // raw input, after IN TRIM, after EQ (mid)
    int frames = 0;
    while (strat3::analysis::popFrame (e->capture, frame.data()))
    {
        if (++frames > n / 2)
        {
            m0.push_back (frame[0]);
            m1.push_back (frame[2]);
            m2.push_back (frame[4]);
        }
    }
    CHECK (frames == n);
    auto r2 = [] (const std::vector<double>& v) { double s = 0; for (double x : v) s += x * x; return std::sqrt (s / static_cast<double> (v.size())); };
    CHECK (std::abs (20.0 * std::log10 (r2 (m1) / r2 (m0)) - 6.0) < 0.1);
    CHECK (std::abs (20.0 * std::log10 (r2 (m2) / r2 (m1)) - 6.0) < 0.15);

    double in = 0.0, out = 0.0;
    int blocks = 0;
    while (strat3::analysis::pop (e->loudIn, in))
        ++blocks;
    CHECK (blocks == 2);   // 9600 samples = two 100 ms blocks
    while (strat3::analysis::pop (e->loudOut, out)) {}
    CHECK (out > in * 3.0);   // +6 dB trim, +6 dB bell on a 1 kHz tone
}

// ------------------------------------------------------------------- real-time safety

TEST_CASE ("engine: setParams and process never allocate", "[engine][rt]")
{
    auto p = neutral();
    p.eq[0] = bell (1000.0f, 3.0f);
    p.eq[1] = bell (200.0f, 2.0f);
    p.eq[1].dyn = dsp::EqDynMode::Down;
    p.limit.style = dsp::LimitStyle::Dense;
    p.limit.threshDb = -4.0f;
    p.mb.sidechain = dsp::MbSidechain::External;
    p.layer[kSat].delta = true;
    p.globalDelta = true;
    p.gainMatch = true;
    for (auto mode : kModes)
    {
        auto e = make (mode, p);
        auto l = sine (500.0, 0.5f, kBlock), r = l, sc = l;
        strat3::test::ScopedAllocCounter count;
        for (int b = 0; b < 50; ++b)
        {
            p.layer[kEq].depth = static_cast<float> (b);
            p.layer[kSat].delta = (b & 1) != 0;
            setParams (*e, p);
            process (*e, l.data(), r.data(), kBlock, sc.data(), sc.data());
        }
        // Read before INFO: Catch's message stack allocates on its first push in a fresh
        // process (ctest runs each case alone), and the counter is still live here.
        const auto allocs = count.count();
        INFO ("mode " << static_cast<int> (mode));
        CHECK (allocs == 0u);
    }
}
