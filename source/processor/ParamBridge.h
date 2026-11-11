#pragma once
// APVTS -> EngineParams. The pointers are looked up once (message thread, string keys); the
// read is atomic loads only, so the audio thread and the analysis thread can both call it.
#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>

#include "engine/Engine.h"

namespace strat3::processor {

using Raw = const std::atomic<float>*;

struct EqBandPointers { Raw on, type, freq, gain, q, ms, dyn, thresh, range, atk, rel; };
struct MbBandPointers { Raw thresh, ratio, dir, knee, atk, rel, det, mix; };
struct SatBandPointers { Raw model, drive, mix; };
struct LayerPointers { Raw on, depth, delta; };

struct ParamBridge
{
    Raw latencyMode {}, inTrim {}, outTrim {}, gainMatch {}, globalDelta {}, target {};
    std::array<LayerPointers, engine::kDepthLayers> layer {};
    LayerPointers stems {};
    std::array<Raw, dsp::kStems> stemGain {}, stemTilt {};   // dsp::Stem order: voc, bass, drm, oth
    Raw ceilingOn {}, ceilingDb {}, dither {}, bits {};
    std::array<EqBandPointers, 8> eq {};
    Raw eqPhase {};
    Raw xover1 {}, xover2 {}, xover3 {};
    Raw lowXover {}, lowPunch {}, lowSustain {}, lowMonoOn {}, lowMonoFreq {}, lowTight {};
    std::array<MbBandPointers, 4> mb {};
    Raw scSource {}, scHpOn {}, scHpFreq {};
    std::array<Raw, 4> width {};
    Raw stereoize {}, monoGuard {};
    std::array<SatBandPointers, 4> sat {};
    Raw satOs {};
    Raw limitStyle {}, limitThresh {}, limitRelease {}, limitLink {}, limitTransient {};
};

/** Message thread. Every pointer is valid afterwards (asserts on a missing ID). */
ParamBridge makeParamBridge (juce::AudioProcessorValueTreeState& apvts);

/** Any thread. `mono` forces stereoize off: with one channel there is no side to add to. */
engine::EngineParams readParams (const ParamBridge& b, bool mono = false) noexcept;

engine::LatencyMode readLatencyMode (const ParamBridge& b) noexcept;

/** sat.os choice -> requested factor 1, 2 or 4 (the engine caps it by sample rate). */
int readSatOs (const ParamBridge& b) noexcept;

} // namespace strat3::processor
