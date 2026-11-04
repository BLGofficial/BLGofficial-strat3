#pragma once
// Minimal AudioProcessor that owns an APVTS built from the real layout, so parameter
// tests exercise exactly what the plugin will register with the host.
#include <juce_audio_processors/juce_audio_processors.h>
#include "params/ParameterLayout.h"

namespace strat3::test {

struct TestProcessor final : juce::AudioProcessor
{
    TestProcessor()
        : AudioProcessor (BusesProperties()
                              .withInput  ("In",  juce::AudioChannelSet::stereo(), true)
                              .withOutput ("Out", juce::AudioChannelSet::stereo(), true))
    {}

    // Constructed after the base so the processor is fully formed when params attach.
    juce::AudioProcessorValueTreeState apvts { *this, nullptr, "STRAT3",
                                               strat3::params::createParameterLayout() };

    const juce::String getName() const override { return "STRAT3Test"; }
    void prepareToPlay (double, int) override {}
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}
};

} // namespace strat3::test
