#pragma once
// The assistant session in saved plugin state: an ASSISTANT child of EXTRA.
//   ASSISTANT
//     REF   { levels = "61 comma-separated dB values" }      one per reference capture
//     MOVE  { id, layer, status, depth, locked, reason, metric, before, target }
//       CHANGE { param, from, to, scaled }
// The target curve itself is not stored: it is recomputed from the references, so the two
// can never disagree. A running capture is not stored either (its audio is gone on reload).
//
// Loading is defensive, entry by entry: host-saved state can be old, hand-edited or from a
// newer build, and a ValueTree read back from XML holds every property as a string. A
// damaged entry is skipped and the rest loads. Parameter ids are not checked here; the
// processor skips changes whose parameter it does not have.
#include <juce_data_structures/juce_data_structures.h>

#include <cmath>
#include <optional>
#include <set>

#include "assistant/Session.h"

namespace strat3::assistant {

inline const juce::Identifier kAssistantType { "ASSISTANT" };
inline const juce::Identifier kReferenceType { "REF" };
inline const juce::Identifier kMoveType      { "MOVE" };
inline const juce::Identifier kChangeType    { "CHANGE" };
inline const juce::Identifier kLevelsProp    { "levels" };
inline const juce::Identifier kIdProp        { "id" };
inline const juce::Identifier kLayerProp     { "layer" };
inline const juce::Identifier kStatusProp    { "status" };
inline const juce::Identifier kDepthProp     { "depth" };
inline const juce::Identifier kLockedProp    { "locked" };
inline const juce::Identifier kReasonProp    { "reason" };
inline const juce::Identifier kMetricProp    { "metric" };
inline const juce::Identifier kBeforeProp    { "before" };
inline const juce::Identifier kTargetProp    { "target" };
inline const juce::Identifier kParamProp     { "param" };
inline const juce::Identifier kFromProp      { "from" };
inline const juce::Identifier kToProp        { "to" };
inline const juce::Identifier kScaledProp    { "scaled" };

namespace statedetail {

/** A finite number from a numeric var or a plain decimal string ("nan", "inf", "" fail). */
inline std::optional<double> number (const juce::var& v)
{
    double d = 0.0;
    if (v.isInt() || v.isInt64() || v.isDouble())
        d = static_cast<double> (v);
    else if (v.isString())
    {
        const auto text = v.toString().trim();
        if (text.isEmpty() || ! text.containsOnly ("0123456789.+-eE") || ! text.containsAnyOf ("0123456789"))
            return std::nullopt;
        d = text.getDoubleValue();
    }
    else
        return std::nullopt;
    return std::isfinite (d) ? std::optional<double> (d) : std::nullopt;
}

inline const char* statusName (MoveStatus s) noexcept
{
    return s == MoveStatus::Accepted ? "accepted" : s == MoveStatus::Rejected ? "rejected" : "pending";
}

} // namespace statedetail

/** The session's saved part (references and log). */
inline juce::ValueTree toTree (const Session& s)
{
    juce::ValueTree tree (kAssistantType);
    for (const auto& r : s.refs)
    {
        juce::StringArray levels;
        for (const float v : r.levelDb)
            levels.add (juce::String (static_cast<double> (std::isfinite (v) ? v : 0.0f), 4));
        juce::ValueTree ref (kReferenceType);
        ref.setProperty (kLevelsProp, levels.joinIntoString (","), nullptr);
        tree.appendChild (ref, nullptr);
    }
    for (const auto& m : s.log)
    {
        juce::ValueTree move (kMoveType);
        move.setProperty (kIdProp, m.move.id, nullptr);
        move.setProperty (kLayerProp, juce::String (m.move.layer), nullptr);
        move.setProperty (kStatusProp, statedetail::statusName (m.status), nullptr);
        move.setProperty (kDepthProp, static_cast<double> (m.depth), nullptr);
        move.setProperty (kLockedProp, m.locked, nullptr);
        move.setProperty (kReasonProp, juce::String (m.move.reason), nullptr);
        move.setProperty (kMetricProp, juce::String (m.move.metric), nullptr);
        move.setProperty (kBeforeProp, static_cast<double> (m.move.metricBefore), nullptr);
        move.setProperty (kTargetProp, static_cast<double> (m.move.metricTarget), nullptr);
        for (const auto& c : m.move.changes)
        {
            juce::ValueTree change (kChangeType);
            change.setProperty (kParamProp, juce::String (c.paramId), nullptr);
            change.setProperty (kFromProp, static_cast<double> (c.from), nullptr);
            change.setProperty (kToProp, static_cast<double> (c.to), nullptr);
            change.setProperty (kScaledProp, c.scaled, nullptr);
            move.appendChild (change, nullptr);
        }
        tree.appendChild (move, nullptr);
    }
    return tree;
}

/** Replaces the session's saved part with `tree` (an invalid tree = an empty session) and
 *  cancels a running capture: the loaded state belongs to a different moment. */
inline void fromTree (Session& s, const juce::ValueTree& tree)
{
    using statedetail::number;
    s.capturing = CaptureKind::None;
    s.captureSeconds = 0.0;
    s.builder = {};
    s.result = CaptureResult::None;
    s.log.clear();
    s.refs.clear();
    s.target = {};
    s.hasTarget = false;
    s.nextId = 1;
    ++s.revision;

    std::set<int> ids;
    for (const auto& child : tree)
    {
        if (child.hasType (kReferenceType) && static_cast<int> (s.refs.size()) < kMaxReferences)
        {
            juce::StringArray tokens;
            tokens.addTokens (child[kLevelsProp].toString(), ",", "");
            if (tokens.size() != kBands)
                continue;
            Snapshot ref;
            bool ok = true;
            for (int b = 0; b < kBands && ok; ++b)
            {
                const auto v = number (juce::var (tokens[b]));
                ok = v.has_value();
                if (ok)
                    ref.levelDb[static_cast<std::size_t> (b)] = static_cast<float> (*v);
            }
            if (ok)
                s.refs.push_back (ref);
        }
        else if (child.hasType (kMoveType) && static_cast<int> (s.log.size()) < kMaxLog)
        {
            const auto id = number (child[kIdProp]);
            if (! id.has_value() || *id < 1.0 || *id > 1e9 || *id != std::floor (*id) || ids.count (static_cast<int> (*id)) > 0)
                continue;
            LoggedMove m;
            m.move.id = static_cast<int> (*id);
            m.move.layer = child[kLayerProp].toString().toStdString();
            m.move.reason = child[kReasonProp].toString().toStdString();
            m.move.metric = child[kMetricProp].toString().toStdString();
            m.move.metricBefore = static_cast<float> (number (child[kBeforeProp]).value_or (0.0));
            m.move.metricTarget = static_cast<float> (number (child[kTargetProp]).value_or (0.0));
            const auto status = child[kStatusProp].toString();
            m.status = status == "accepted" ? MoveStatus::Accepted : status == "rejected" ? MoveStatus::Rejected : MoveStatus::Pending;
            const auto depth = number (child[kDepthProp]);
            m.depth = depth.has_value() && *depth >= 0.0 && *depth <= 1.0 ? static_cast<float> (*depth) : 1.0f;
            m.locked = static_cast<bool> (child[kLockedProp]);
            bool ok = true;
            for (const auto& c : child)
            {
                if (! c.hasType (kChangeType))
                    continue;
                const auto from = number (c[kFromProp]), to = number (c[kToProp]);
                const auto param = c[kParamProp].toString();
                if (! from.has_value() || ! to.has_value() || param.isEmpty())
                {
                    ok = false;   // half a move would apply half a correction: drop it whole
                    break;
                }
                m.move.changes.push_back ({ param.toStdString(), static_cast<float> (*from), static_cast<float> (*to),
                                            static_cast<bool> (c[kScaledProp]) });
            }
            if (! ok || m.move.changes.empty())
                continue;
            ids.insert (m.move.id);
            s.nextId = std::max (s.nextId, m.move.id + 1);
            s.log.push_back (std::move (m));
        }
    }
    if (! s.refs.empty())
    {
        s.target = captureTarget (s.refs);
        s.hasTarget = true;
    }
}

} // namespace strat3::assistant
