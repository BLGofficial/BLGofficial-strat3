#pragma once
// An assistant move: one explainable proposal, accepted, rejected or dialled down on its own.
// A move can touch several parameters (an EQ move sets on, type, freq, Q and gain together),
// so it carries a list of changes. Only "scaled" changes follow the depth slider; the rest are
// switches and settings that apply as soon as the move is accepted at any depth.
#include <algorithm>
#include <string>
#include <vector>

namespace strat3::assistant {

struct ParamChange
{
    std::string paramId;
    float from = 0.0f, to = 0.0f;
    bool scaled = false;   // true: from + depth * (to - from); false: a switch/setting
};

struct Move
{
    int id = 0;
    std::string layer;          // "eq", "low", "limit"
    std::vector<ParamChange> changes;
    std::string reason;         // "-2.8 dB @ 250 Hz: 4.0 dB above target"
    std::string metric;         // what was measured
    float metricBefore = 0.0f;  // measured value
    float metricTarget = 0.0f;  // value the move aims for
};

/** The value to set for a change at the given depth (0 = rejected, 1 = as proposed). */
inline float applyDepth (const ParamChange& c, float depth) noexcept
{
    const float d = std::clamp (depth, 0.0f, 1.0f);
    if (! c.scaled)
        return d > 0.0f ? c.to : c.from;
    return c.from + d * (c.to - c.from);
}

} // namespace strat3::assistant
