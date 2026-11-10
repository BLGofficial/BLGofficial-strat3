// Assistant rule engine (v1: rules, not a model). Every move carries its reason and the
// measurement behind it, and can be accepted, rejected or dialled down on its own.
//   Snapshot (30/60 s capture): per 1/6-octave band mean level, dynamics spread (P95/P50 of
//   ~100 ms block energies), side/mid ratio; plus LUFS-I, true peak, correlation.
//   Target: a curve captured from the user's own references (no built-in genre data).
//   Rules: TONAL (EQ toward the target; cuts on dynamic bands become dynamic EQ), LOW MONO
//   (side energy below 120 Hz), LOUDNESS (limiter threshold toward the platform target).
//   Depth: scaled changes go from + depth * (to - from); switches apply at any depth > 0.
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <string>

#include "assistant/Rules.h"

using namespace strat3::assistant;
using strat3::analysis::impactBandCentreHz;

namespace {

constexpr int kBand250 = 22;   // 1 kHz * 2^(-12/6) = 250 Hz
constexpr int kBand4k = 46;    // 1 kHz * 2^(12/6) = 4 kHz

Snapshot flatSnapshot (float levelDb = -30.0f)
{
    Snapshot s;
    s.levelDb.fill (levelDb);
    s.spreadDb.fill (3.0f);
    s.sideToMidDb.fill (-20.0f);
    s.integratedLufs = -14.0f;
    s.truePeakDb = -1.0f;
    s.correlation = 0.8f;
    s.seconds = 30.0;
    return s;
}

TargetCurve flatTarget()
{
    TargetCurve t;
    t.levelDb.fill (0.0f);
    return t;
}

/** Adds a plateau of `db` over bands [centre - half, centre + half]. */
void bump (Snapshot& s, int centre, int half, float db)
{
    for (int b = centre - half; b <= centre + half; ++b)
        s.levelDb[static_cast<size_t> (b)] += db;
}

ParamValues defaults()
{
    ParamValues p;
    for (int n = 1; n <= kEqBands; ++n)
        p["eq.b" + std::to_string (n) + ".on"] = 0.0f;
    p["eq.on"] = 1.0f;
    p["global.target"] = 1.0f;   // -14 LUFS
    p["limit.on"] = 1.0f;
    p["limit.thresh"] = -3.0f;
    p["low.on"] = 1.0f;
    p["low.monoOn"] = 0.0f;
    p["low.monoFreq"] = 80.0f;
    return p;
}

const ParamChange* find (const Move& m, const std::string& id)
{
    for (const auto& c : m.changes)
        if (c.paramId == id)
            return &c;
    return nullptr;
}

std::set<std::string> goldenIds()
{
    std::set<std::string> ids;
    std::ifstream in (std::string (STRAT3_GOLDEN_DIR) + "/param_ids_v1.txt");
    std::string line;
    while (std::getline (in, line))
        if (! line.empty() && line[0] != '#')
            ids.insert (line.substr (0, line.find ('|')));
    return ids;
}

} // namespace

// ------------------------------------------------------------------- depth

TEST_CASE ("depth scales a move: from + depth * (to - from), clamped to 0..1", "[assistant][depth]")
{
    const ParamChange gain { "eq.b1.gain", 0.0f, -3.0f, true };
    CHECK (applyDepth (gain, 0.0f) == 0.0f);
    CHECK (applyDepth (gain, 0.5f) == -1.5f);
    CHECK (applyDepth (gain, 1.0f) == -3.0f);
    CHECK (applyDepth (gain, 2.0f) == -3.0f);
    CHECK (applyDepth (gain, -1.0f) == 0.0f);
    const ParamChange sw { "eq.b1.on", 0.0f, 1.0f, false };
    CHECK (applyDepth (sw, 0.0f) == 0.0f);    // depth 0 is "rejected"
    CHECK (applyDepth (sw, 0.01f) == 1.0f);   // switches do not interpolate
}

// ------------------------------------------------------------------- tonal

TEST_CASE ("tonal: a 4 dB bump at 250 Hz becomes one bell cut on the first free EQ band", "[assistant][tonal]")
{
    auto s = flatSnapshot();
    bump (s, kBand250, 2, 4.0f);
    const auto target = flatTarget();
    const auto moves = propose (s, &target, defaults());
    REQUIRE (moves.size() == 1);
    const auto& m = moves[0];
    CHECK (m.layer == "eq");
    REQUIRE (find (m, "eq.b1.on") != nullptr);
    CHECK (find (m, "eq.b1.on")->to == 1.0f);
    CHECK (find (m, "eq.b1.type") == nullptr);   // already Bell: unchanged settings are not listed
    REQUIRE (find (m, "eq.b1.freq") != nullptr);
    const double hz = static_cast<double> (find (m, "eq.b1.freq")->to);
    CHECK (hz > impactBandCentreHz (kBand250 - 1));
    CHECK (hz < impactBandCentreHz (kBand250 + 1));
    const auto* g = find (m, "eq.b1.gain");
    REQUIRE (g != nullptr);
    CHECK (g->scaled);
    CHECK (std::abs (g->to - -2.8f) < 0.15f);   // 70% of the 4 dB excess, hard-coded on purpose
    const float q = find (m, "eq.b1.q")->to;
    CHECK (q >= 0.5f);
    CHECK (q <= 4.0f);
    CHECK (std::abs (m.metricBefore - 4.0f) < 0.15f);
    CHECK (m.metricTarget == 0.0f);
    CHECK (m.reason.find ("Hz") != std::string::npos);
}

TEST_CASE ("tonal: a bypassed EQ layer is switched on as part of the move", "[assistant][tonal]")
{
    auto s = flatSnapshot();
    bump (s, kBand250, 2, 4.0f);
    const auto target = flatTarget();
    auto p = defaults();
    CHECK (find (propose (s, &target, p)[0], "eq.on") == nullptr);   // already on: not listed
    p["eq.on"] = 0.0f;
    const auto moves = propose (s, &target, p);
    REQUIRE (moves.size() == 1);
    const auto* on = find (moves[0], "eq.on");
    REQUIRE (on != nullptr);   // without it, accepting the move would change nothing audible
    CHECK (on->from == 0.0f);
    CHECK (on->to == 1.0f);
    CHECK_FALSE (on->scaled);
}

TEST_CASE ("tonal: overall level does not matter, only the shape against the target", "[assistant][tonal]")
{
    auto s = flatSnapshot (-12.0f);   // 18 dB hotter than the flat case, same shape
    const auto target = flatTarget();
    CHECK (propose (s, &target, defaults()).empty());
    auto t2 = flatTarget();
    for (auto& v : t2.levelDb)
        v -= 40.0f;
    CHECK (propose (s, &t2, defaults()).empty());
}

TEST_CASE ("tonal: deviations under the threshold produce no move", "[assistant][tonal]")
{
    auto s = flatSnapshot();
    bump (s, kBand250, 2, 1.5f);   // literal, so a retuned threshold shows up here
    const auto target = flatTarget();
    CHECK (propose (s, &target, defaults()).empty());
}

TEST_CASE ("tonal: two problems, largest first, each on its own free band; cuts and boosts", "[assistant][tonal]")
{
    auto s = flatSnapshot();
    bump (s, kBand250, 2, 4.0f);
    bump (s, kBand4k, 2, -5.0f);
    auto p = defaults();
    p["eq.b1.on"] = 1.0f;   // b1 is taken: use b2 and b3
    const auto target = flatTarget();
    const auto moves = propose (s, &target, p);
    REQUIRE (moves.size() == 2);
    CHECK (find (moves[0], "eq.b2.gain") != nullptr);
    CHECK (find (moves[0], "eq.b2.gain")->to > 0.0f);   // the -5 dB dip is the bigger problem
    CHECK (find (moves[1], "eq.b3.gain") != nullptr);
    CHECK (find (moves[1], "eq.b3.gain")->to < 0.0f);
    CHECK (moves[0].id != moves[1].id);
}

TEST_CASE ("tonal: no free EQ band, no tonal move", "[assistant][tonal]")
{
    auto s = flatSnapshot();
    bump (s, kBand250, 2, 6.0f);
    auto p = defaults();
    for (int n = 1; n <= kEqBands; ++n)
        p["eq.b" + std::to_string (n) + ".on"] = 1.0f;
    const auto target = flatTarget();
    CHECK (propose (s, &target, p).empty());
}

TEST_CASE ("tonal: at most kMaxTonalMoves, and no tonal moves without a target", "[assistant][tonal]")
{
    auto s = flatSnapshot();
    for (int c : { 8, 18, 28, 38, 48 })
        bump (s, c, 1, (c % 20 == 8) ? 5.0f : -5.0f);
    const auto target = flatTarget();
    CHECK (propose (s, &target, defaults()).size() == 3u);
    CHECK (propose (s, nullptr, defaults()).empty());
}

TEST_CASE ("tonal: a cut on a band with wide dynamics becomes a dynamic EQ dip", "[assistant][tonal][dynamic]")
{
    // Intermittent excess (high spread) is what dynamic EQ is for; a constant excess is
    // better served by a static cut. Boosts always stay static.
    auto s = flatSnapshot();
    bump (s, kBand250, 2, 4.0f);
    for (int b = kBand250 - 2; b <= kBand250 + 2; ++b)
        s.spreadDb[static_cast<size_t> (b)] = kDynamicSpreadDb + 3.0f;
    const auto target = flatTarget();
    const auto moves = propose (s, &target, defaults());
    REQUIRE (moves.size() == 1);
    const auto& m = moves[0];
    CHECK (find (m, "eq.b1.dyn")->to == 2.0f);   // Down
    CHECK (find (m, "eq.b1.gain") == nullptr);   // static gain untouched
    const auto* r = find (m, "eq.b1.range");
    REQUIRE (r != nullptr);
    CHECK (r->scaled);
    CHECK (std::abs (r->to - -2.8f) < 0.15f);
    CHECK (std::abs (find (m, "eq.b1.thresh")->to - s.levelDb[kBand250]) < 0.01f);

    auto boost = flatSnapshot();
    bump (boost, kBand250, 2, -4.0f);
    for (auto& v : boost.spreadDb)
        v = kDynamicSpreadDb + 3.0f;
    const auto bm = propose (boost, &target, defaults());
    REQUIRE (bm.size() == 1);
    CHECK (find (bm[0], "eq.b1.dyn") == nullptr);
    CHECK (find (bm[0], "eq.b1.gain")->to > 0.0f);
}

TEST_CASE ("tonal: corrections are capped at +-6 dB", "[assistant][tonal]")
{
    auto s = flatSnapshot();
    bump (s, kBand250, 2, 20.0f);
    const auto target = flatTarget();
    const auto moves = propose (s, &target, defaults());
    REQUIRE (moves.size() == 1);
    CHECK (find (moves[0], "eq.b1.gain")->to == -kMaxTonalGainDb);
}

TEST_CASE ("tonal: a single-band spike is smoothed away, a 1/2-octave plateau is not", "[assistant][tonal]")
{
    // One 1/6-octave band 2.5 dB up is measurement noise at this resolution: the +-1 band
    // average brings it to 0.83 dB, under the threshold.
    auto s = flatSnapshot();
    s.levelDb[kBand250] += 2.5f;
    const auto target = flatTarget();
    CHECK (propose (s, &target, defaults()).empty());
}

TEST_CASE ("tonal: a bump next to a dip with no gap is two moves, not one", "[assistant][tonal]")
{
    // +8 dB over bands 18..24, -8 dB over 25..31: after smoothing both edges stay at
    // +-2.67 dB, so the regions touch. They must still split where the sign changes.
    auto s = flatSnapshot();
    bump (s, 21, 3, 8.0f);
    bump (s, 28, 3, -8.0f);
    const auto target = flatTarget();
    const auto moves = propose (s, &target, defaults());
    REQUIRE (moves.size() == 2);
    const auto* g1 = find (moves[0], "eq.b1.gain");
    const auto* g2 = find (moves[1], "eq.b2.gain");
    REQUIRE (g1 != nullptr);
    REQUIRE (g2 != nullptr);
    CHECK (g1->to * g2->to < 0.0f);   // one cut, one boost
}

TEST_CASE ("tonal: the low end down to 40 Hz is in range", "[assistant][tonal]")
{
    auto s = flatSnapshot();
    bump (s, 8, 1, 4.0f);   // band 8 = 50.4 Hz
    const auto target = flatTarget();
    const auto moves = propose (s, &target, defaults());
    REQUIRE (moves.size() == 1);
    const float hz = find (moves[0], "eq.b1.freq")->to;
    CHECK (hz > 44.0f);
    CHECK (hz < 57.0f);
}

// ------------------------------------------------------------------- low mono

TEST_CASE ("low mono: wide low end gets mono bass below 120 Hz", "[assistant][low]")
{
    auto s = flatSnapshot();
    for (int b = 0; impactBandCentreHz (b) < 120.0; ++b)
        s.sideToMidDb[static_cast<size_t> (b)] = -3.0f;
    const auto moves = propose (s, nullptr, defaults());
    REQUIRE (moves.size() == 1);
    CHECK (moves[0].layer == "low");
    CHECK (find (moves[0], "low.monoOn")->to == 1.0f);
    CHECK (find (moves[0], "low.monoFreq")->to == kLowMonoHz);
    CHECK (std::abs (moves[0].metricBefore - -3.0f) < 0.01f);
    CHECK (moves[0].metricTarget == kLowSideMaxDb);

    auto p = defaults();
    p["low.monoOn"] = 1.0f;   // already mono: nothing to propose
    CHECK (propose (s, nullptr, p).empty());
    CHECK (propose (flatSnapshot(), nullptr, defaults()).empty());   // narrow low end
}

// ------------------------------------------------------------------- loudness

TEST_CASE ("loudness: limiter threshold moves by the distance to the platform target", "[assistant][loudness]")
{
    auto s = flatSnapshot();
    s.integratedLufs = -18.0f;
    auto moves = propose (s, nullptr, defaults());
    REQUIRE (moves.size() == 1);
    CHECK (moves[0].layer == "limit");
    const auto* t = find (moves[0], "limit.thresh");
    REQUIRE (t != nullptr);
    CHECK (t->from == -3.0f);
    CHECK (t->to == -7.0f);   // 4 LU short: pull the threshold down 4 dB (threshold = makeup)
    CHECK (t->scaled);
    CHECK (moves[0].metricBefore == -18.0f);
    CHECK (moves[0].metricTarget == -14.0f);

    s.integratedLufs = -14.3f;   // within tolerance
    CHECK (propose (s, nullptr, defaults()).empty());

    s.integratedLufs = -40.0f;   // clamped to the parameter range
    CHECK (find (propose (s, nullptr, defaults())[0], "limit.thresh")->to == -24.0f);

    auto off = defaults();
    off["global.target"] = 0.0f;   // OFF
    CHECK (propose (s, nullptr, off).empty());
    s.integratedLufs = -std::numeric_limits<float>::infinity();
    CHECK (propose (s, nullptr, defaults()).empty());
}

TEST_CASE ("loudness: a bypassed limiter is switched on as part of the move", "[assistant][loudness]")
{
    auto s = flatSnapshot();
    s.integratedLufs = -20.0f;
    auto p = defaults();
    p["limit.on"] = 0.0f;
    const auto moves = propose (s, nullptr, p);
    REQUIRE (moves.size() == 1);
    REQUIRE (find (moves[0], "limit.on") != nullptr);
    CHECK (find (moves[0], "limit.on")->to == 1.0f);
}

// ------------------------------------------------------------------- whole engine

TEST_CASE ("every move names real parameters, with from values taken from the current state", "[assistant]")
{
    auto s = flatSnapshot();
    bump (s, kBand250, 2, 4.0f);
    bump (s, kBand4k, 2, -4.0f);
    for (int b = 0; impactBandCentreHz (b) < 120.0; ++b)
        s.sideToMidDb[static_cast<size_t> (b)] = 0.0f;
    s.integratedLufs = -20.0f;
    const auto target = flatTarget();
    const auto moves = propose (s, &target, defaults());
    REQUIRE (moves.size() == 4);
    CHECK (moves[0].layer == "eq");   // chain order: EQ, then LOW, then LIMIT
    CHECK (moves[2].layer == "low");
    CHECK (moves[3].layer == "limit");
    const auto ids = goldenIds();
    REQUIRE (ids.size() > 100);
    const auto p = defaults();
    for (const auto& m : moves)
    {
        CHECK_FALSE (m.reason.empty());
        CHECK_FALSE (m.metric.empty());
        for (const auto& c : m.changes)
        {
            CAPTURE (c.paramId);
            CHECK (ids.count (c.paramId) == 1);
            if (p.count (c.paramId))
                CHECK (c.from == p.at (c.paramId));
        }
    }
    // Deterministic: the same capture proposes the same moves.
    const auto again = propose (s, &target, defaults());
    REQUIRE (again.size() == moves.size());
    for (size_t i = 0; i < moves.size(); ++i)
        CHECK (again[i].reason == moves[i].reason);
}

TEST_CASE ("non-finite measurements never produce moves", "[assistant][stability]")
{
    auto s = flatSnapshot();
    s.levelDb[kBand250] = std::numeric_limits<float>::quiet_NaN();
    s.sideToMidDb[3] = std::numeric_limits<float>::infinity();
    s.integratedLufs = std::numeric_limits<float>::quiet_NaN();
    const auto target = flatTarget();
    for (const auto& m : propose (s, &target, defaults()))
        for (const auto& c : m.changes)
        {
            CHECK (std::isfinite (c.from));
            CHECK (std::isfinite (c.to));
        }
}

// ------------------------------------------------------------------- snapshot and target capture

TEST_CASE ("snapshot builder: mean level, dynamics spread and side/mid per band", "[assistant][snapshot]")
{
    SnapshotBuilder b;
    begin (b, 60.0);
    BandEnergies mid {}, side {};
    // 300 frames at 30 Hz (10 s): band 10 steady, band 20 loud one block in ten.
    for (int f = 0; f < 300; ++f)
    {
        mid.fill (1e-4);
        side.fill (1e-5);
        mid[20] = (f / kFramesPerBlock) % 10 == 0 ? 1e-2 : 1e-4;
        addFrame (b, mid, side);
    }
    const auto s = finish (b, -14.0f, -1.0f, 0.9f);
    CHECK (std::abs (s.seconds - 10.0) < 1e-9);
    CHECK (std::abs (s.levelDb[10] - -39.586f) < 0.01f);   // mid + side: 1.1e-4
    CHECK (std::abs (s.sideToMidDb[10] - -10.0f) < 0.01f);
    CHECK (std::abs (s.spreadDb[10]) < 0.01f);              // steady: P95 = P50
    CHECK (std::abs (s.spreadDb[20] - 19.59f) < 0.05f);     // 1 block in 10 is ~20 dB up
    CHECK (s.integratedLufs == -14.0f);
    CHECK (s.truePeakDb == -1.0f);
    CHECK (s.correlation == 0.9f);
}

TEST_CASE ("snapshot builder stops at its capture length and skips non-finite frames", "[assistant][snapshot]")
{
    SnapshotBuilder b;
    begin (b, 2.0);
    BandEnergies e {};
    e.fill (1e-4);
    for (int f = 0; f < 300; ++f)
        addFrame (b, e, e);
    BandEnergies bad = e;
    bad[5] = std::numeric_limits<double>::quiet_NaN();
    addFrame (b, bad, e);
    const auto s = finish (b, -14.0f, -1.0f, 1.0f);
    CHECK (std::abs (s.seconds - 2.0) < 0.05);
    for (float v : s.levelDb)
        CHECK (std::isfinite (v));
}

TEST_CASE ("snapshot builder skips non-finite and negative frames before the cap", "[assistant][snapshot]")
{
    SnapshotBuilder b;
    begin (b, 60.0);
    BandEnergies e {};
    e.fill (1e-4);
    for (int f = 0; f < 30; ++f)
        addFrame (b, e, e);
    BandEnergies nan = e, neg = e;
    nan[5] = std::numeric_limits<double>::quiet_NaN();
    neg[7] = -1e-4;
    addFrame (b, nan, e);
    addFrame (b, e, neg);
    const auto s = finish (b, -14.0f, -1.0f, 1.0f);
    CHECK (s.seconds == 1.0);   // 30 good frames at 30 Hz; the bad two are not counted
    for (float v : s.levelDb)
        CHECK (std::isfinite (v));
}

TEST_CASE ("target capture: references at different levels give the same curve", "[assistant][target]")
{
    auto a = flatSnapshot (-20.0f), b = flatSnapshot (-26.0f);
    bump (a, kBand4k, 2, 3.0f);
    bump (b, kBand4k, 2, 3.0f);
    const std::vector<Snapshot> refs { a, b };
    const auto t = captureTarget (refs);
    const auto ta = captureTarget (std::vector<Snapshot> { a });
    for (int i = 0; i < kBands; ++i)
        CHECK (std::abs (t.levelDb[static_cast<size_t> (i)] - ta.levelDb[static_cast<size_t> (i)]) < 1e-4f);
    // A mix equal to the references has nothing to correct.
    CHECK (propose (a, &t, defaults()).empty());
}
