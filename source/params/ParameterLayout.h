#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "params/ParameterSpecs.h"

namespace strat3::params {

/** Host range for a spec. Log tapers map 0..1 exponentially so the geometric mean sits at
 *  mid-travel (a skew factor only approximates this and drifts at the range ends). */
juce::NormalisableRange<float> makeRange (const ParamSpec& spec);

/** APVTS layout built from allSpecs(), in spec order. Message thread only. */
juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

} // namespace strat3::params
