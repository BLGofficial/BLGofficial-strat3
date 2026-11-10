// Assistant session (module 21a): the headless state behind the assistant and targets panels.
// Contracts tested here:
//   - a capture is ANALYSE (ends in proposals) or REFERENCE (adds one reference to the user's
//     target); it completes after seconds * 30 analysis frames, never on wall-clock time, so a
//     stopped transport stalls it instead of analysing silence
//   - the capture measures its own LUFS-I (not the meter's programme-long one)
//   - a silent capture adds no reference and proposes nothing
//   - a new analysis replaces PENDING moves and keeps decided ones as locked history (their
//     from/to belong to an older state of the mix); ids are never reused
//   - moves that share a parameter (two EQ moves both switching a bypassed EQ on): undoing one
//     leaves that parameter alone while another accepted move still needs it
//   - accept / reject / depth / apply-all return the parameter values to set; the session
//     never touches parameters itself (the processor does, with host gestures)
//   - bad input (unknown id, non-finite depth, capture length out of range) changes nothing
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <string>

#include "assistant/Session.h"

using namespace strat3::assistant;
using Catch::Approx;

namespace {

constexpr int kBand250 = 22;   // 1 kHz * 2^(-12/6) = 250 Hz

BandEnergies flat (double e)
{
    BandEnergies b;
    b.fill (e);
    return b;
}

/** A flat spectrum with `db` more energy over bands [centre - half, centre + half]. */
BandEnergies bumped (double e, int centre, int half, double db)
{
    auto b = flat (e);
    for (int k = centre - half; k <= centre + half; ++k)
        b[static_cast<std::size_t> (k)] *= std::pow (10.0, db / 10.0);
    return b;
}

double meanSquareFor (double lufs) { return std::pow (10.0, (lufs + 0.691) / 10.0); }

/** Runs a whole capture: `seconds` of identical frames and matching loudness blocks. */
void runCapture (Session& s, CaptureKind kind, double seconds, const BandEnergies& mid, const BandEnergies& side,
                 double lufs, const ParamValues& p)
{
    REQUIRE (startCapture (s, kind, seconds));
    const int frames = static_cast<int> (std::lround (seconds * kSnapshotFrameHz));
    for (int f = 0; f < frames; ++f)
    {
        if (f % 3 == 0)
            addLoudnessBlock (s, meanSquareFor (lufs));
        addFrame (s, mid, side, 0.9f);
    }
    REQUIRE (captureComplete (s));
    completeCapture (s, p);
}

ParamValues defaults()
{
    ParamValues p;
    for (int n = 1; n <= kEqBands; ++n)
        p["eq.b" + std::to_string (n) + ".on"] = 0.0f;
    p["eq.on"] = 1.0f;
    p["global.target"] = 0.0f;   // OFF: no loudness move unless a test asks for one
    p["limit.on"] = 1.0f;
    p["limit.thresh"] = 0.0f;
    p["low.on"] = 1.0f;
    p["low.monoOn"] = 1.0f;      // already mono: no low move unless a test asks for one
    return p;
}

Session makeSession()
{
    Session s;
    prepare (s);
    return s;
}

/** A session with a flat target and one pending tonal move (a 250 Hz cut on eq.b1). */
Session sessionWithTonalMove()
{
    auto s = makeSession();
    runCapture (s, CaptureKind::Reference, 1.0, flat (1e-3), flat (1e-5), -14.0, defaults());
    runCapture (s, CaptureKind::Analyse, 1.0, bumped (1e-3, kBand250, 2, 4.0), flat (1e-5), -14.0, defaults());
    return s;
}

bool has (const ParamSet& set, const std::string& id)
{
    return std::any_of (set.begin(), set.end(), [&] (const auto& kv) { return kv.first == id; });
}

/** A flat target and two pending tonal moves (cut at 250 Hz on eq.b1, boost at 4 kHz on
 *  eq.b2), analysed with the EQ layer bypassed: both moves also switch eq.on. */
Session sessionWithTwoEqMoves()
{
    auto s = makeSession();
    auto p = defaults();
    p["eq.on"] = 0.0f;
    runCapture (s, CaptureKind::Reference, 1.0, flat (1e-3), flat (1e-5), -14.0, p);
    auto mix = bumped (1e-3, kBand250, 2, 4.0);
    for (int k = 44; k <= 48; ++k)   // 4 kHz +- 2 bands
        mix[static_cast<std::size_t> (k)] *= std::pow (10.0, -0.4);
    runCapture (s, CaptureKind::Analyse, 1.0, mix, flat (1e-5), -14.0, p);
    return s;
}

float valueOf (const ParamSet& set, const std::string& id)
{
    for (const auto& [k, v] : set)
        if (k == id)
            return v;
    FAIL ("missing " << id);
    return 0.0f;
}

} // namespace

TEST_CASE ("session: a capture completes after seconds * 30 frames and reports progress", "[assistant][session]")
{
    auto s = makeSession();
    CHECK (s.capturing == CaptureKind::None);
    CHECK (progress (s) == 0.0f);
    REQUIRE (startCapture (s, CaptureKind::Analyse, 30.0));
    CHECK (s.capturing == CaptureKind::Analyse);
    for (int f = 0; f < 450; ++f)
        addFrame (s, flat (1e-3), flat (1e-5), 1.0f);
    CHECK (progress (s) == Approx (0.5f));
    CHECK_FALSE (captureComplete (s));
    for (int f = 0; f < 450; ++f)
        addFrame (s, flat (1e-3), flat (1e-5), 1.0f);
    CHECK (progress (s) == Approx (1.0f));
    CHECK (captureComplete (s));
    completeCapture (s, defaults());
    CHECK (s.capturing == CaptureKind::None);
    CHECK (progress (s) == 0.0f);
}

TEST_CASE ("session: frames and loudness blocks outside a capture are ignored", "[assistant][session]")
{
    auto s = makeSession();
    const auto rev = s.revision;
    addFrame (s, flat (1e-3), flat (1e-5), 1.0f);
    addLoudnessBlock (s, 0.1);
    CHECK (s.builder.mid.empty());
    CHECK (s.loud.received == 0);
    CHECK_FALSE (captureComplete (s));
    completeCapture (s, defaults());   // nothing to complete: a no-op
    CHECK (s.log.empty());
    CHECK (s.revision == rev);
}

TEST_CASE ("session: capture length must be in (0, 60] s; a capture cannot start over another", "[assistant][session]")
{
    auto s = makeSession();
    CHECK_FALSE (startCapture (s, CaptureKind::Analyse, 0.0));
    CHECK_FALSE (startCapture (s, CaptureKind::Analyse, -5.0));
    CHECK_FALSE (startCapture (s, CaptureKind::Analyse, 60.5));
    CHECK_FALSE (startCapture (s, CaptureKind::Analyse, std::numeric_limits<double>::quiet_NaN()));
    CHECK_FALSE (startCapture (s, CaptureKind::None, 30.0));
    CHECK (s.capturing == CaptureKind::None);
    REQUIRE (startCapture (s, CaptureKind::Analyse, 60.0));
    CHECK_FALSE (startCapture (s, CaptureKind::Reference, 30.0));
    CHECK (s.capturing == CaptureKind::Analyse);
}

TEST_CASE ("session: cancel drops the capture and keeps the log and the target", "[assistant][session]")
{
    auto s = sessionWithTonalMove();
    const auto moves = s.log.size();
    REQUIRE (startCapture (s, CaptureKind::Analyse, 30.0));
    addFrame (s, flat (1e-3), flat (1e-5), 1.0f);
    cancelCapture (s);
    CHECK (s.capturing == CaptureKind::None);
    CHECK (progress (s) == 0.0f);
    CHECK (s.log.size() == moves);
    CHECK (s.hasTarget);
    CHECK (s.result == CaptureResult::Cancelled);
}

TEST_CASE ("session: reference captures build the target; analysis against it proposes a tonal move", "[assistant][session]")
{
    auto s = makeSession();
    CHECK_FALSE (s.hasTarget);
    runCapture (s, CaptureKind::Reference, 1.0, flat (1e-3), flat (1e-5), -14.0, defaults());
    CHECK (s.hasTarget);
    CHECK (s.refs.size() == 1);
    CHECK (s.result == CaptureResult::ReferenceAdded);
    CHECK (s.log.empty());

    runCapture (s, CaptureKind::Analyse, 1.0, bumped (1e-3, kBand250, 2, 4.0), flat (1e-5), -14.0, defaults());
    REQUIRE (s.log.size() == 1);
    CHECK (s.result == CaptureResult::Moves);
    const auto& m = s.log[0];
    CHECK (m.status == MoveStatus::Pending);
    CHECK (m.depth == 1.0f);
    CHECK (m.move.layer == "eq");
    CHECK (m.move.id == 1);
}

TEST_CASE ("session: the target is the average of all references", "[assistant][session]")
{
    auto s = makeSession();
    runCapture (s, CaptureKind::Reference, 1.0, flat (1e-3), flat (1e-5), -14.0, defaults());
    runCapture (s, CaptureKind::Reference, 1.0, bumped (1e-3, kBand250, 2, 6.0), flat (1e-5), -9.0, defaults());
    REQUIRE (s.refs.size() == 2);
    const auto expected = captureTarget (s.refs);
    for (std::size_t b = 0; b < static_cast<std::size_t> (kBands); ++b)
        CHECK (s.target.levelDb[b] == expected.levelDb[b]);
    // Half of the 6 dB bump (two references, one flat), relative to the flat part.
    CHECK (s.target.levelDb[kBand250] - s.target.levelDb[40] == Approx (3.0f).margin (0.05f));
}

TEST_CASE ("session: references are capped; clearTarget forgets them", "[assistant][session]")
{
    auto s = makeSession();
    for (int i = 0; i < kMaxReferences; ++i)
        runCapture (s, CaptureKind::Reference, 0.5, flat (1e-3), flat (1e-5), -14.0, defaults());
    CHECK (static_cast<int> (s.refs.size()) == kMaxReferences);
    CHECK_FALSE (startCapture (s, CaptureKind::Reference, 30.0));
    CHECK (startCapture (s, CaptureKind::Analyse, 30.0));   // analysis is still allowed
    cancelCapture (s);
    clearTarget (s);
    CHECK_FALSE (s.hasTarget);
    CHECK (s.refs.empty());
    CHECK (startCapture (s, CaptureKind::Reference, 30.0));
}

TEST_CASE ("session: a silent capture adds no reference and proposes nothing", "[assistant][session]")
{
    auto s = makeSession();
    auto p = defaults();
    p["global.target"] = 1.0f;
    p["low.monoOn"] = 0.0f;
    runCapture (s, CaptureKind::Reference, 1.0, flat (0.0), flat (0.0), -200.0, p);
    CHECK_FALSE (s.hasTarget);
    CHECK (s.refs.empty());
    CHECK (s.result == CaptureResult::Silent);
    runCapture (s, CaptureKind::Analyse, 1.0, flat (0.0), flat (0.0), -200.0, p);
    CHECK (s.log.empty());
    CHECK (s.result == CaptureResult::Silent);
}

TEST_CASE ("session: the capture measures its own integrated loudness", "[assistant][session]")
{
    auto s = makeSession();
    auto p = defaults();
    p["global.target"] = 1.0f;   // -14 LUFS
    runCapture (s, CaptureKind::Analyse, 2.0, flat (1e-3), flat (1e-5), -20.0, p);
    REQUIRE (s.log.size() == 1);
    CHECK (s.log[0].move.layer == "limit");
    CHECK (s.log[0].move.metricBefore == Approx (-20.0f).margin (0.05f));

    // A second capture starts from an empty meter: -14 is on target, so nothing to propose.
    runCapture (s, CaptureKind::Analyse, 2.0, flat (1e-3), flat (1e-5), -14.0, p);
    CHECK (s.log.empty());
    CHECK (s.result == CaptureResult::NoMoves);
}

TEST_CASE ("session: accept returns every change at the given depth", "[assistant][session]")
{
    auto s = sessionWithTonalMove();
    const auto& move = s.log[0].move;
    const auto set = accept (s, move.id, 0.5f);
    REQUIRE (set.size() == move.changes.size());
    for (const auto& c : move.changes)
        CHECK (valueOf (set, c.paramId) == applyDepth (c, 0.5f));
    CHECK (valueOf (set, "eq.b1.on") == 1.0f);
    const float full = std::find_if (move.changes.begin(), move.changes.end(), [] (const ParamChange& c) { return c.paramId == "eq.b1.gain"; })->to;
    CHECK (full < 0.0f);
    CHECK (valueOf (set, "eq.b1.gain") == Approx (0.5f * full));
    CHECK (s.log[0].status == MoveStatus::Accepted);
    CHECK (s.log[0].depth == 0.5f);
}

TEST_CASE ("session: depth is clamped to 0..1; a non-finite depth changes nothing", "[assistant][session]")
{
    auto s = sessionWithTonalMove();
    const int id = s.log[0].move.id;
    const auto rev = s.revision;
    CHECK (accept (s, id, std::numeric_limits<float>::quiet_NaN()).empty());
    CHECK (setDepth (s, id, std::numeric_limits<float>::infinity()).empty());
    CHECK (s.log[0].status == MoveStatus::Pending);
    CHECK (s.revision == rev);
    CHECK_FALSE (accept (s, id, 7.0f).empty());
    CHECK (s.log[0].depth == 1.0f);
    CHECK_FALSE (setDepth (s, id, -3.0f).empty());
    CHECK (s.log[0].depth == 0.0f);
}

TEST_CASE ("session: reject of a pending move sets nothing; reject of an accepted one restores it", "[assistant][session]")
{
    auto s = sessionWithTonalMove();
    const int id = s.log[0].move.id;
    CHECK (reject (s, id).empty());
    CHECK (s.log[0].status == MoveStatus::Rejected);

    // Changing your mind: a rejected move can still be accepted, and rejected again.
    CHECK_FALSE (accept (s, id, 1.0f).empty());
    const auto back = reject (s, id);
    REQUIRE (back.size() == s.log[0].move.changes.size());
    for (const auto& c : s.log[0].move.changes)
        CHECK (valueOf (back, c.paramId) == c.from);
    CHECK (s.log[0].status == MoveStatus::Rejected);
}

TEST_CASE ("session: the depth slider re-applies an accepted move and only stores on a pending one", "[assistant][session]")
{
    auto s = sessionWithTonalMove();
    const int id = s.log[0].move.id;
    CHECK (setDepth (s, id, 0.25f).empty());
    CHECK (s.log[0].depth == 0.25f);
    CHECK (s.log[0].status == MoveStatus::Pending);

    REQUIRE_FALSE (accept (s, id, s.log[0].depth).empty());
    const auto set = setDepth (s, id, 0.75f);
    REQUIRE_FALSE (set.empty());
    for (const auto& c : s.log[0].move.changes)
        CHECK (valueOf (set, c.paramId) == applyDepth (c, 0.75f));

    // A rejected move keeps its depth for a later accept, and sets nothing.
    reject (s, id);
    CHECK (setDepth (s, id, 0.5f).empty());
    CHECK (s.log[0].depth == 0.5f);
}

TEST_CASE ("session: apply all accepts every pending move at its own depth, and only those", "[assistant][session]")
{
    auto s = makeSession();
    auto p = defaults();
    p["global.target"] = 1.0f;
    p["low.monoOn"] = 0.0f;
    runCapture (s, CaptureKind::Reference, 1.0, flat (1e-3), flat (1e-5), -14.0, p);
    // Tonal (250 Hz bump), low mono (wide side) and loudness (-20 vs -14): three moves.
    runCapture (s, CaptureKind::Analyse, 2.0, bumped (1e-3, kBand250, 2, 4.0), flat (1e-3), -20.0, p);
    REQUIRE (s.log.size() == 3);
    reject (s, s.log[1].move.id);
    setDepth (s, s.log[2].move.id, 0.5f);

    const auto set = applyAll (s);
    CHECK (set.size() == s.log[0].move.changes.size() + s.log[2].move.changes.size());
    CHECK (s.log[0].status == MoveStatus::Accepted);
    CHECK (s.log[1].status == MoveStatus::Rejected);
    CHECK (s.log[2].status == MoveStatus::Accepted);
    const float from = 0.0f, to = s.log[2].move.changes.back().to;
    CHECK (valueOf (set, "limit.thresh") == Approx (from + 0.5f * (to - from)));
    CHECK (applyAll (s).empty());   // nothing pending any more
}

TEST_CASE ("session: an unknown move id changes nothing", "[assistant][session]")
{
    auto s = sessionWithTonalMove();
    const auto rev = s.revision;
    CHECK (accept (s, 999, 1.0f).empty());
    CHECK (reject (s, 999).empty());
    CHECK (setDepth (s, 0, 0.5f).empty());
    CHECK (s.log[0].status == MoveStatus::Pending);
    CHECK (s.revision == rev);
}

TEST_CASE ("session: a new analysis replaces pending moves, keeps decided ones, never reuses an id", "[assistant][session]")
{
    auto s = makeSession();
    auto p = defaults();
    p["global.target"] = 1.0f;
    p["low.monoOn"] = 0.0f;
    runCapture (s, CaptureKind::Reference, 1.0, flat (1e-3), flat (1e-5), -14.0, p);
    runCapture (s, CaptureKind::Analyse, 2.0, bumped (1e-3, kBand250, 2, 4.0), flat (1e-3), -20.0, p);
    REQUIRE (s.log.size() == 3);
    const int acceptedId = s.log[0].move.id, rejectedId = s.log[1].move.id, pendingId = s.log[2].move.id;
    accept (s, acceptedId, 1.0f);
    reject (s, rejectedId);

    runCapture (s, CaptureKind::Analyse, 2.0, bumped (1e-3, kBand250, 2, 4.0), flat (1e-3), -20.0, p);
    std::set<int> ids;
    for (const auto& m : s.log)
        CHECK (ids.insert (m.move.id).second);
    CHECK (ids.count (acceptedId) == 1);
    CHECK (ids.count (rejectedId) == 1);
    CHECK (ids.count (pendingId) == 0);
    CHECK (s.log[0].status == MoveStatus::Accepted);
    CHECK (s.log[1].status == MoveStatus::Rejected);
    REQUIRE (s.log.size() == 5);   // 2 decided + 3 new
    for (std::size_t i = 2; i < s.log.size(); ++i)
    {
        CHECK (s.log[i].status == MoveStatus::Pending);
        CHECK_FALSE (s.log[i].locked);
        CHECK (s.log[i].move.id > pendingId);
    }
}

TEST_CASE ("session: a new analysis locks the decided moves: history, no longer adjustable", "[assistant][session]")
{
    // Their from/to describe the mix before the newer analysis. Undoing one now would write
    // old values over whatever the newer moves (or the user) set since.
    auto s = makeSession();
    auto p = defaults();
    p["global.target"] = 1.0f;
    p["low.monoOn"] = 0.0f;
    runCapture (s, CaptureKind::Reference, 1.0, flat (1e-3), flat (1e-5), -14.0, p);
    runCapture (s, CaptureKind::Analyse, 2.0, bumped (1e-3, kBand250, 2, 4.0), flat (1e-3), -20.0, p);
    REQUIRE (s.log.size() == 3);
    const int acceptedId = s.log[0].move.id, rejectedId = s.log[1].move.id;
    accept (s, acceptedId, 0.5f);
    reject (s, rejectedId);
    CHECK_FALSE (s.log[0].locked);   // still adjustable until the next analysis
    CHECK_FALSE (s.log[1].locked);

    runCapture (s, CaptureKind::Analyse, 2.0, bumped (1e-3, kBand250, 2, 4.0), flat (1e-3), -20.0, p);
    REQUIRE (s.log[0].locked);
    REQUIRE (s.log[1].locked);
    const auto rev = s.revision;
    CHECK (reject (s, acceptedId).empty());
    CHECK (setDepth (s, acceptedId, 1.0f).empty());
    CHECK (accept (s, acceptedId, 1.0f).empty());
    CHECK (accept (s, rejectedId, 1.0f).empty());
    CHECK (setDepth (s, rejectedId, 0.25f).empty());
    CHECK (s.log[0].status == MoveStatus::Accepted);
    CHECK (s.log[0].depth == 0.5f);
    CHECK (s.log[1].status == MoveStatus::Rejected);
    CHECK (s.log[1].depth == 1.0f);
    CHECK (s.revision == rev);
    CHECK (applyAll (s).size() > 0);   // the new proposals are unaffected
    CHECK (s.log[0].depth == 0.5f);
}

TEST_CASE ("session: a silent analysis leaves the log alone", "[assistant][session]")
{
    // No signal means nothing was learned: the proposals on screen are still the best ones.
    auto s = sessionWithTonalMove();
    const int id = s.log[0].move.id;
    runCapture (s, CaptureKind::Analyse, 1.0, flat (0.0), flat (0.0), -200.0, defaults());
    CHECK (s.result == CaptureResult::Silent);
    REQUIRE (s.log.size() == 1);
    CHECK (s.log[0].move.id == id);
    CHECK (s.log[0].status == MoveStatus::Pending);
    CHECK_FALSE (s.log[0].locked);
    CHECK_FALSE (accept (s, id, 1.0f).empty());
}

TEST_CASE ("session: undoing a move leaves a parameter another accepted move still needs", "[assistant][session]")
{
    auto s = sessionWithTwoEqMoves();
    REQUIRE (s.log.size() == 2);
    const int a = s.log[0].move.id, b = s.log[1].move.id;
    // Both switch the bypassed EQ layer on.
    for (const auto& m : s.log)
        REQUIRE (std::any_of (m.move.changes.begin(), m.move.changes.end(), [] (const ParamChange& c) { return c.paramId == "eq.on" && c.from == 0.0f && c.to == 1.0f; }));

    CHECK (valueOf (accept (s, a, 1.0f), "eq.on") == 1.0f);
    CHECK (valueOf (accept (s, b, 1.0f), "eq.on") == 1.0f);

    // Rejecting A must not bypass the EQ: B is still accepted and would go silent with it.
    auto set = reject (s, a);
    CHECK_FALSE (has (set, "eq.on"));
    CHECK (valueOf (set, "eq.b1.on") == 0.0f);
    CHECK (valueOf (set, "eq.b1.gain") == 0.0f);
    CHECK_FALSE (has (set, "eq.b2.on"));

    // B is the last one holding it: now the layer goes back to bypassed.
    set = reject (s, b);
    CHECK (valueOf (set, "eq.on") == 0.0f);
    CHECK (valueOf (set, "eq.b2.on") == 0.0f);
}

TEST_CASE ("session: a locked move holds nothing: undoing a newer move restores exactly its own from", "[assistant][session]")
{
    // First analysis: A switches the bypassed EQ on and is accepted. The user then bypasses
    // the EQ again by hand. Second analysis: B also switches it on. Undoing B must put the
    // EQ back to bypassed (B's from), although A, now history, once changed the same switch.
    auto s = sessionWithTwoEqMoves();
    const int a = s.log[0].move.id;
    accept (s, a, 1.0f);
    reject (s, s.log[1].move.id);

    auto p = defaults();
    p["eq.on"] = 0.0f;       // bypassed again by hand
    p["eq.b1.on"] = 1.0f;    // A's band is taken
    runCapture (s, CaptureKind::Analyse, 1.0, bumped (1e-3, kBand250, 2, 4.0), flat (1e-5), -14.0, p);
    REQUIRE (s.log.size() == 3);
    REQUIRE (s.log[0].locked);
    REQUIRE (s.log[0].status == MoveStatus::Accepted);
    const int b = s.log[2].move.id;
    CHECK (valueOf (accept (s, b, 1.0f), "eq.on") == 1.0f);
    const auto set = reject (s, b);
    CHECK (valueOf (set, "eq.on") == 0.0f);
    CHECK (valueOf (set, "eq.b2.on") == 0.0f);
}

TEST_CASE ("session: depth 0 on one move does not undo a switch another accepted move shares", "[assistant][session]")
{
    auto s = sessionWithTwoEqMoves();
    const int a = s.log[0].move.id, b = s.log[1].move.id;
    accept (s, a, 1.0f);
    accept (s, b, 1.0f);
    auto set = setDepth (s, a, 0.0f);
    CHECK_FALSE (has (set, "eq.on"));
    CHECK (valueOf (set, "eq.b1.on") == 0.0f);   // its own switches still follow the slider
    set = setDepth (s, a, 0.5f);
    CHECK (valueOf (set, "eq.on") == 1.0f);      // setting it again is harmless
    CHECK (valueOf (set, "eq.b1.on") == 1.0f);

    // A move that is only pending or rejected holds nothing.
    reject (s, b);
    set = reject (s, a);
    CHECK (valueOf (set, "eq.on") == 0.0f);
}

TEST_CASE ("session: the log is capped; the oldest decided moves go first", "[assistant][session]")
{
    auto s = makeSession();
    auto p = defaults();
    p["global.target"] = 1.0f;
    int firstId = 0;
    for (int i = 0; i < kMaxLog + 5; ++i)
    {
        runCapture (s, CaptureKind::Analyse, 2.0, flat (1e-3), flat (1e-5), -20.0, p);
        REQUIRE (s.log.back().status == MoveStatus::Pending);
        if (i == 0)
            firstId = s.log.back().move.id;
        reject (s, s.log.back().move.id);
    }
    CHECK (static_cast<int> (s.log.size()) == kMaxLog);
    for (const auto& m : s.log)
        CHECK (m.move.id != firstId);
    CHECK (s.log.back().move.id == firstId + kMaxLog + 4);
}

TEST_CASE ("session: every change that the panel can see bumps the revision", "[assistant][session]")
{
    auto s = makeSession();
    auto rev = s.revision;
    auto bumped_ = [&] { const bool b = s.revision != rev; rev = s.revision; return b; };
    startCapture (s, CaptureKind::Reference, 1.0);
    CHECK (bumped_());
    addFrame (s, flat (1e-3), flat (1e-5), 1.0f);   // progress moved
    CHECK (bumped_());
    cancelCapture (s);
    CHECK (bumped_());
    runCapture (s, CaptureKind::Reference, 1.0, flat (1e-3), flat (1e-5), -14.0, defaults());
    CHECK (bumped_());
    runCapture (s, CaptureKind::Analyse, 1.0, bumped (1e-3, kBand250, 2, 4.0), flat (1e-5), -14.0, defaults());
    CHECK (bumped_());
    const int id = s.log[0].move.id;
    setDepth (s, id, 0.5f);
    CHECK (bumped_());
    accept (s, id, 0.5f);
    CHECK (bumped_());
    reject (s, id);
    CHECK (bumped_());
    clearTarget (s);
    CHECK (bumped_());
}
