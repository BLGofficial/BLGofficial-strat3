#pragma once
// JSON protocol between the assistant / targets panels (WebView) and the session.
//   page -> native   one command object: { "type": "...", ... }
//   native -> page   the whole session as one object (stateToVar); the page re-renders from
//                    it, so there is no incremental update to drift out of sync
// The page is untrusted input: a command is parsed strictly and refused whole when anything
// about it is off (wrong type, missing field, out-of-range number). Nothing is coerced.
#include <juce_core/juce_core.h>

#include <cmath>
#include <limits>
#include <optional>

#include "assistant/Session.h"

namespace strat3::assistant {

enum class CommandType { Analyse, CaptureReference, Cancel, Accept, Reject, Depth, ApplyAll, ClearTarget };

struct Command
{
    CommandType type = CommandType::Cancel;
    int id = 0;
    float depth = 1.0f;
    bool hasDepth = false;   // accept without a depth uses the move's stored one
    double seconds = 0.0;
};

namespace protocoldetail {

inline std::optional<double> number (const juce::var& v)
{
    if (! (v.isInt() || v.isInt64() || v.isDouble()))   // not bool, not a numeric string
        return std::nullopt;
    const auto d = static_cast<double> (v);
    return std::isfinite (d) ? std::optional<double> (d) : std::nullopt;
}

inline std::optional<int> moveId (const juce::var& v)
{
    const auto d = number (v);
    if (! d.has_value() || *d < 1.0 || *d > static_cast<double> (std::numeric_limits<int>::max()) || *d != std::floor (*d))
        return std::nullopt;
    return static_cast<int> (*d);
}

inline std::optional<float> depth (const juce::var& v)
{
    const auto d = number (v);
    if (! d.has_value() || *d < 0.0 || *d > 1.0)
        return std::nullopt;
    return static_cast<float> (*d);
}

/** JSON has no NaN or Infinity: a non-finite value would make the whole message unparseable. */
inline double finite (double x) noexcept { return std::isfinite (x) ? x : 0.0; }

inline const char* name (CaptureKind k) noexcept
{
    switch (k)
    {
        case CaptureKind::Analyse:   return "analyse";
        case CaptureKind::Reference: return "reference";
        case CaptureKind::None:      break;
    }
    return "none";
}

inline const char* name (MoveStatus s) noexcept
{
    switch (s)
    {
        case MoveStatus::Accepted: return "accepted";
        case MoveStatus::Rejected: return "rejected";
        case MoveStatus::Pending:  break;
    }
    return "pending";
}

inline const char* name (CaptureResult r) noexcept
{
    switch (r)
    {
        case CaptureResult::Moves:          return "moves";
        case CaptureResult::NoMoves:        return "noMoves";
        case CaptureResult::ReferenceAdded: return "referenceAdded";
        case CaptureResult::Silent:         return "silent";
        case CaptureResult::Cancelled:      return "cancelled";
        case CaptureResult::None:           break;
    }
    return "none";
}

} // namespace protocoldetail

/** A command from the page, or nullopt if it is not exactly well-formed. */
inline std::optional<Command> parseCommand (const juce::var& v)
{
    using namespace protocoldetail;
    if (v.getDynamicObject() == nullptr)
        return std::nullopt;
    const auto type = v["type"].toString();   // a non-string type converts to text that matches no command
    Command c;

    if (type == "cancel")      { c.type = CommandType::Cancel;      return c; }
    if (type == "applyAll")    { c.type = CommandType::ApplyAll;    return c; }
    if (type == "clearTarget") { c.type = CommandType::ClearTarget; return c; }

    if (type == "analyse" || type == "captureReference")
    {
        const auto seconds = number (v["seconds"]);
        if (! seconds.has_value() || (*seconds != 30.0 && *seconds != 60.0))   // the two lengths the panel offers
            return std::nullopt;
        c.type = type == "analyse" ? CommandType::Analyse : CommandType::CaptureReference;
        c.seconds = *seconds;
        return c;
    }

    if (type == "accept" || type == "reject" || type == "depth")
    {
        const auto id = moveId (v["id"]);
        if (! id.has_value())
            return std::nullopt;
        c.id = *id;
        c.type = type == "accept" ? CommandType::Accept : type == "reject" ? CommandType::Reject : CommandType::Depth;
        if (c.type == CommandType::Reject)
            return c;
        if (v.hasProperty ("depth"))
        {
            const auto d = depth (v["depth"]);
            if (! d.has_value())
                return std::nullopt;
            c.depth = *d;
            c.hasDepth = true;
        }
        else if (c.type == CommandType::Depth)
            return std::nullopt;
        return c;
    }
    return std::nullopt;
}

/** The whole session for the page. `paused`: the feed is holding the running capture back
 *  (the processor knows why; today only a delta listen). Allocates; call under the session's
 *  lock. */
inline juce::var stateToVar (const Session& s, bool paused = false)
{
    using namespace protocoldetail;
    auto obj = [] { return new juce::DynamicObject(); };

    auto* capture = obj();
    capture->setProperty ("kind", name (s.capturing));
    capture->setProperty ("progress", finite (static_cast<double> (progress (s))));
    capture->setProperty ("seconds", finite (s.captureSeconds));
    capture->setProperty ("paused", paused && s.capturing != CaptureKind::None);

    auto* target = obj();
    target->setProperty ("captured", s.hasTarget);
    target->setProperty ("references", static_cast<int> (s.refs.size()));
    target->setProperty ("maxReferences", kMaxReferences);
    juce::Array<juce::var> levels, hz;
    if (s.hasTarget)
        for (int b = 0; b < kBands; ++b)
        {
            levels.add (finite (static_cast<double> (s.target.levelDb[static_cast<std::size_t> (b)])));
            hz.add (analysis::impactBandCentreHz (b));
        }
    target->setProperty ("levelDb", levels);
    target->setProperty ("bandHz", hz);

    juce::Array<juce::var> moves;
    for (const auto& m : s.log)
    {
        auto* mo = obj();
        mo->setProperty ("id", m.move.id);
        mo->setProperty ("layer", juce::String (m.move.layer));
        mo->setProperty ("status", name (m.status));
        mo->setProperty ("depth", finite (static_cast<double> (m.depth)));
        mo->setProperty ("locked", m.locked);   // history: the panel shows it without controls
        mo->setProperty ("reason", juce::String (m.move.reason));
        mo->setProperty ("metric", juce::String (m.move.metric));
        mo->setProperty ("metricBefore", finite (static_cast<double> (m.move.metricBefore)));
        mo->setProperty ("metricTarget", finite (static_cast<double> (m.move.metricTarget)));
        juce::Array<juce::var> changes;
        for (const auto& c : m.move.changes)
        {
            auto* co = obj();
            co->setProperty ("paramId", juce::String (c.paramId));
            co->setProperty ("from", finite (static_cast<double> (c.from)));
            co->setProperty ("to", finite (static_cast<double> (c.to)));
            co->setProperty ("scaled", c.scaled);
            changes.add (juce::var (co));
        }
        mo->setProperty ("changes", changes);
        moves.add (juce::var (mo));
    }

    auto* root = obj();
    root->setProperty ("revision", static_cast<juce::int64> (s.revision));
    root->setProperty ("capture", juce::var (capture));
    root->setProperty ("result", name (s.result));
    root->setProperty ("target", juce::var (target));
    root->setProperty ("moves", moves);
    return juce::var (root);
}

} // namespace strat3::assistant
