#pragma once
// Single source of truth for every host-visible parameter.
// Specs are plain data so they can be tested, diffed against the frozen golden file and
// turned into an APVTS layout without the plugin wrapper.
#include <string>
#include <vector>

namespace strat3::params {

enum class Kind  { Float, Choice, Bool };
enum class Taper { Linear, Log };

/** Plain description of one host parameter.
 *  For Choice: min = 0, max = choices.size() - 1, def = default index.
 *  For Bool:   min = 0, max = 1, def = 0 or 1. */
struct ParamSpec
{
    std::string id;
    std::string name;
    Kind  kind  = Kind::Float;
    float min   = 0.0f;
    float max   = 1.0f;
    float def   = 0.0f;
    Taper taper = Taper::Linear;
    std::vector<std::string> choices {};
    std::string unit {};
    bool automatable = true;
    int  versionHint = 1;   // bump for params added after the first public release; never lower
};

/** All parameters in registration order. Built once on first call (message thread);
 *  never call from the audio thread. */
const std::vector<ParamSpec>& allSpecs();

/** Reorderable processing layers, in default chain order. The ceiling is excluded:
 *  it is always last and ignores depth. */
const std::vector<std::string>& processingLayerIds();

/** Shape of a spec that must never change after release: "id|kind|min|max|taper|choices". */
std::string goldenLine (const ParamSpec& spec);

} // namespace strat3::params
