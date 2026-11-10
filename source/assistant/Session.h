#pragma once
// Assistant session: the state behind the assistant and targets panels, with no UI and no
// parameter access. One capture at a time:
//   ANALYSE    30/60 s of the plugin's output -> proposals (Rules.h) appended to the log
//   REFERENCE  30/60 s of a reference track playing through the plugin -> one more reference
//              in the user's target (the average shape of all references)
// A capture is counted in analysis frames (30 per second of audio), not wall-clock time: the
// feed only delivers a frame when new audio arrived, so stopping the transport pauses it.
//
// Moves are adjustable (accept, reject, depth) until the next analysis completes. That
// analysis replaces the undecided proposals and locks the decided ones as history: their
// from/to describe the mix as it was before, so undoing one later would write stale values
// over whatever newer moves or the user set since. Host undo still reaches them.
//
// The session never sets parameters. accept / reject / setDepth / applyAll return the values
// to set and the caller (the processor, message thread) applies them inside host gestures.
//
// Threads: none of this is real-time safe (it allocates) and none of it is synchronised. The
// processor guards one Session with a mutex shared by the analysis and message threads; the
// audio thread never touches it.
//
// Limits are own design [INFERRED]: 16 references, 64 log entries.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "analysis/Loudness1770.h"
#include "assistant/Rules.h"

namespace strat3::assistant {

inline constexpr double kMaxCaptureSeconds = 60.0;
inline constexpr int kMaxReferences = 16;
inline constexpr int kMaxLog = 64;
// A capture whose median band level (100 Hz - 10 kHz) is below this had no signal. -100 dB
// is far under any programme material and far above the -200 dB that digital silence reads.
inline constexpr float kSilentLevelDb = -100.0f;

enum class CaptureKind { None, Analyse, Reference };
enum class MoveStatus { Pending, Accepted, Rejected };
/** What the last capture ended in (the panel's one-line status). */
enum class CaptureResult { None, Moves, NoMoves, ReferenceAdded, Silent, Cancelled };

struct LoggedMove
{
    Move move;
    MoveStatus status = MoveStatus::Pending;
    float depth = 1.0f;   // remembered while pending or rejected, applied while accepted
    bool locked = false;  // decided before the latest analysis: history, no longer adjustable
};

/** Parameter values to set, in order. */
using ParamSet = std::vector<std::pair<std::string, float>>;

struct Session
{
    CaptureKind capturing = CaptureKind::None;
    double captureSeconds = 0.0;
    SnapshotBuilder builder {};
    analysis::LoudnessMeter loud {};   // the capture's own LUFS-I: the output meter's runs since prepare
    double correlationSum = 0.0;
    int correlationCount = 0;

    std::vector<LoggedMove> log {};
    int nextId = 1;                    // ids are never reused: the panel addresses moves by id
    std::vector<Snapshot> refs {};
    TargetCurve target {};
    bool hasTarget = false;
    CaptureResult result = CaptureResult::None;
    std::uint32_t revision = 0;        // bumped by every change the panel can see
};

/** Allocates the loudness histogram. Call once before anything else. */
inline void prepare (Session& s) { analysis::prepare (s.loud); }

inline float progress (const Session& s) noexcept
{
    if (s.capturing == CaptureKind::None || s.builder.maxFrames == 0)
        return 0.0f;
    return static_cast<float> (s.builder.mid.size()) / static_cast<float> (s.builder.maxFrames);
}

inline bool captureComplete (const Session& s) noexcept
{
    return s.capturing != CaptureKind::None && s.builder.mid.size() >= s.builder.maxFrames;
}

/** False (and nothing changes) if a capture is running, the length is outside (0, 60] s, or
 *  the reference list is full. */
inline bool startCapture (Session& s, CaptureKind kind, double seconds)
{
    if (s.capturing != CaptureKind::None || kind == CaptureKind::None)
        return false;
    if (! (seconds > 0.0) || ! (seconds <= kMaxCaptureSeconds))   // also refuses NaN
        return false;
    if (kind == CaptureKind::Reference && static_cast<int> (s.refs.size()) >= kMaxReferences)
        return false;
    begin (s.builder, seconds);
    if (s.builder.maxFrames == 0)
        return false;
    analysis::reset (s.loud);
    s.correlationSum = 0.0;
    s.correlationCount = 0;
    s.capturing = kind;
    s.captureSeconds = seconds;
    ++s.revision;
    return true;
}

inline void cancelCapture (Session& s)
{
    if (s.capturing == CaptureKind::None)
        return;
    s.capturing = CaptureKind::None;
    s.captureSeconds = 0.0;
    s.builder = {};
    s.result = CaptureResult::Cancelled;
    ++s.revision;
}

/** One 100 ms K-weighted block of the output (see Loudness1770). Ignored outside a capture. */
inline void addLoudnessBlock (Session& s, double meanSquare) noexcept
{
    if (s.capturing != CaptureKind::None && ! captureComplete (s))
        analysis::addBlock (s.loud, meanSquare);
}

/** One analysis frame of the output. Ignored outside a capture. */
inline void addFrame (Session& s, const BandEnergies& mid, const BandEnergies& side, float correlation)
{
    if (s.capturing == CaptureKind::None)
        return;
    const auto before = s.builder.mid.size();
    addFrame (s.builder, mid, side);
    if (s.builder.mid.size() == before)
        return;
    if (std::isfinite (correlation))
    {
        s.correlationSum += static_cast<double> (correlation);
        ++s.correlationCount;
    }
    ++s.revision;   // progress moved
}

namespace sessiondetail {

inline LoggedMove* find (Session& s, int id) noexcept
{
    for (auto& m : s.log)
        if (m.move.id == id)
            return &m;
    return nullptr;
}

/** True if another adjustable, accepted move changes this parameter too. Locked moves hold
 *  nothing: they are history, and a newer move's `from` was read after they were applied. */
inline bool heldByAnother (const Session& s, const LoggedMove& self, const std::string& paramId) noexcept
{
    for (const auto& other : s.log)
        if (&other != &self && other.status == MoveStatus::Accepted && ! other.locked)
            for (const auto& c : other.move.changes)
                if (c.paramId == paramId)
                    return true;
    return false;
}

/** The values `m` sets at `depth`. A change that would put a parameter back to its `from`
 *  is left out while another accepted move changes the same parameter: two EQ moves both
 *  switch a bypassed EQ layer on, and undoing one must not silence the other. */
inline void append (ParamSet& out, const Session& s, const LoggedMove& m, float depth)
{
    for (const auto& c : m.move.changes)
    {
        const float v = applyDepth (c, depth);
        if (v == c.from && heldByAnother (s, m, c.paramId))
            continue;
        out.emplace_back (c.paramId, v);
    }
}

/** Oldest first. Pending moves are always the newest entries (a new analysis drops the old
 *  ones before appending), so this only ever removes decided history. */
inline void trimLog (Session& s)
{
    if (s.log.size() > static_cast<std::size_t> (kMaxLog))
        s.log.erase (s.log.begin(), s.log.end() - kMaxLog);
}

} // namespace sessiondetail

/** The logged move with this id, or nullptr. */
inline const LoggedMove* findMove (const Session& s, int id) noexcept
{
    for (const auto& m : s.log)
        if (m.move.id == id)
            return &m;
    return nullptr;
}

/** Ends a complete capture: proposals (ANALYSE) or one more reference (REFERENCE). `p` holds
 *  the current parameter values (the proposals' `from`). A no-op unless captureComplete(). */
inline void completeCapture (Session& s, const ParamValues& p)
{
    if (! captureComplete (s))
        return;
    const float correlation = s.correlationCount > 0 ? static_cast<float> (s.correlationSum / s.correlationCount) : 1.0f;
    // True peak is not measured per capture and no rule reads it.
    const auto snap = finish (s.builder, analysis::integratedLufs (s.loud), 0.0f, correlation);
    const auto kind = s.capturing;
    s.capturing = CaptureKind::None;
    s.captureSeconds = 0.0;
    s.builder = {};
    ++s.revision;

    const float median = ruledetail::normMedian (snap.levelDb);
    const bool silent = ! std::isfinite (median) || median < kSilentLevelDb;

    if (kind == CaptureKind::Reference)
    {
        if (silent)
        {
            s.result = CaptureResult::Silent;
            return;
        }
        s.refs.push_back (snap);
        s.target = captureTarget (s.refs);
        s.hasTarget = true;
        s.result = CaptureResult::ReferenceAdded;
        return;
    }

    if (silent)
    {
        s.result = CaptureResult::Silent;   // nothing was learned: the log stays as it is
        return;
    }
    // A new analysis supersedes the proposals nobody decided on; decided moves stay, locked.
    std::erase_if (s.log, [] (const LoggedMove& m) { return m.status == MoveStatus::Pending; });
    for (auto& m : s.log)
        m.locked = true;
    auto moves = propose (snap, s.hasTarget ? &s.target : nullptr, p);
    s.result = moves.empty() ? CaptureResult::NoMoves : CaptureResult::Moves;
    for (auto& m : moves)
    {
        m.id = s.nextId++;
        s.log.push_back ({ std::move (m), MoveStatus::Pending, 1.0f, false });
    }
    sessiondetail::trimLog (s);
}

inline void clearTarget (Session& s)
{
    s.refs.clear();
    s.target = {};
    s.hasTarget = false;
    ++s.revision;
}

/** Accepts a pending or rejected move (or re-applies an accepted one) at `depth`, clamped
 *  to 0..1. Returns the values to set; empty (and nothing changes) for an unknown id, a
 *  locked move or a non-finite depth. */
inline ParamSet accept (Session& s, int id, float depth)
{
    ParamSet out;
    auto* m = sessiondetail::find (s, id);
    if (m == nullptr || m->locked || ! std::isfinite (depth))
        return out;
    m->depth = std::clamp (depth, 0.0f, 1.0f);
    m->status = MoveStatus::Accepted;
    sessiondetail::append (out, s, *m, m->depth);
    ++s.revision;
    return out;
}

/** Rejects a move. An accepted one is undone: the returned values restore its `from`s
 *  (except parameters another accepted move still needs). Nothing changes for an unknown id
 *  or a locked move.
 *  Note: `from` is the value at analysis time; edits made by hand since are overwritten. */
inline ParamSet reject (Session& s, int id)
{
    ParamSet out;
    auto* m = sessiondetail::find (s, id);
    if (m == nullptr || m->locked)
        return out;
    if (m->status == MoveStatus::Accepted)
        sessiondetail::append (out, s, *m, 0.0f);
    m->status = MoveStatus::Rejected;
    ++s.revision;
    return out;
}

/** Stores the depth; for an accepted move also returns the values to set. */
inline ParamSet setDepth (Session& s, int id, float depth)
{
    ParamSet out;
    auto* m = sessiondetail::find (s, id);
    if (m == nullptr || m->locked || ! std::isfinite (depth))
        return out;
    m->depth = std::clamp (depth, 0.0f, 1.0f);
    if (m->status == MoveStatus::Accepted)
        sessiondetail::append (out, s, *m, m->depth);
    ++s.revision;
    return out;
}

/** Accepts every pending move at its own depth, in log (= chain) order. */
inline ParamSet applyAll (Session& s)
{
    ParamSet out;
    for (auto& m : s.log)
    {
        if (m.status != MoveStatus::Pending)
            continue;
        m.status = MoveStatus::Accepted;
        sessiondetail::append (out, s, m, m.depth);
        ++s.revision;
    }
    return out;
}

} // namespace strat3::assistant
