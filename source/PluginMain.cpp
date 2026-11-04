// Host entry point: every format wrapper (VST3, AU, CLAP, Standalone) calls this.
#include "processor/Strat3Processor.h"

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new strat3::processor::Strat3Processor();
}
