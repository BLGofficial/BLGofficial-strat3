// Proves the test target links strat3_dsp + JUCE dsp before real modules land.
#include <catch2/catch_test_macros.hpp>
#include <juce_dsp/juce_dsp.h>

namespace strat3::dsp { int libraryAnchor() noexcept; }

TEST_CASE("build smoke: strat3_dsp links and JUCE dsp is usable", "[smoke]")
{
    REQUIRE(strat3::dsp::libraryAnchor() == 0);
    juce::AudioBuffer<float> buffer(2, 64);
    buffer.clear();
    REQUIRE(buffer.getMagnitude(0, 64) == 0.0f);
}
