#pragma once
// Plain-data EQ settings. Enum order matches the parameter choice lists (params/ParameterSpecs)
// so a choice index converts with a static_cast.
#include <array>

namespace strat3::dsp {

enum class EqType    { Bell, LowShelf, HighShelf, HighPass, LowPass, PassiveShelf };
enum class EqChannel { Stereo, Mid, Side };
enum class EqDynMode { Off, Up, Down };

struct EqBand
{
    bool      on        = false;
    EqType    type      = EqType::Bell;
    float     freq      = 1000.0f;
    float     gainDb    = 0.0f;
    float     q         = 0.707f;
    EqChannel channel   = EqChannel::Stereo;
    EqDynMode dyn       = EqDynMode::Off;
    float     threshDb  = -20.0f;
    float     rangeDb   = -3.0f;
    float     attackMs  = 10.0f;
    float     releaseMs = 100.0f;

    bool operator== (const EqBand&) const = default;
};

inline constexpr int kEqBands = 8;
using EqSettings = std::array<EqBand, kEqBands>;

// Dynamic travel: the dynamic offset reaches its full range this many dB past threshold.
// A fixed travel instead of a ratio control keeps the node to two intuitive knobs
// (threshold, range); it is a STRAT3 design choice, not a copy of any source behaviour.
inline constexpr float kDynamicTravelDb = 12.0f;

// Passive-style shelf: a low shelf plus an opposite-sign bell above the corner, so a boost
// comes with the gentle dip characteristic of passive LC shelving. Curve shape is our own.
inline constexpr float kPassiveDipRatio  = 1.6f;    // bell centre relative to the corner
inline constexpr float kPassiveDipAmount = 0.35f;   // bell gain relative to the shelf gain
inline constexpr float kPassiveDipQ      = 1.0f;

inline bool supportsGain (EqType t) noexcept { return t != EqType::HighPass && t != EqType::LowPass; }

} // namespace strat3::dsp
