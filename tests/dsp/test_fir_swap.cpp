// FirSwap: the background-design / warm-up / crossfade handover shared by every FIR stage
// (band-domain crossover, FIR EQ). Tested with trivial slots so the state machine is checked
// on its own, independent of any filter maths.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>
#include <vector>

#include "dsp/core/FirSwap.h"

using namespace strat3::dsp;

namespace {

struct CountingSlot
{
    float value = 0.0f;
    int calls = 0;
};

float runCounting (CountingSlot& s) { ++s.calls; return s.value; }
float lerp (float a, float b, float t) { return a + t * (b - a); }

/** Swap with slot values 1 (active) and 2 (standby) and the given fade length. */
void prepareCounting (FirSwap<CountingSlot>& sw, int fadeSamples)
{
    swapPrepare (sw, fadeSamples, [] (CountingSlot& s) { s = {}; });
    sw.slots[0].value = 1.0f;
    sw.slots[1].value = 2.0f;
}

float tick (FirSwap<CountingSlot>& sw) { return swapProcess (sw, runCounting, lerp); }

} // namespace

TEST_CASE ("idle swap runs only the active slot", "[dsp][swap]")
{
    FirSwap<CountingSlot> sw;
    prepareCounting (sw, 4);
    for (int i = 0; i < 10; ++i)
        CHECK (tick (sw) == 1.0f);
    CHECK (sw.slots[0].calls == 10);
    CHECK (sw.slots[1].calls == 0);   // standby costs nothing while idle
    CHECK_FALSE (isSwapping (sw));
}

TEST_CASE ("swap sequence: design -> warm-up (standby silent) -> linear fade -> swap", "[dsp][swap]")
{
    FirSwap<CountingSlot> sw;
    prepareCounting (sw, 4);

    REQUIRE (swapDesign (sw, [] (CountingSlot& s) { s.value = 5.0f; }));
    CHECK (sw.slots[1].value == 5.0f);    // design went to the standby slot
    CHECK (tick (sw) == 1.0f);            // a finished design is not audible until swapBegin
    CHECK_FALSE (isSwapping (sw));

    REQUIRE (swapBegin (sw, 3));
    CHECK (isSwapping (sw));
    for (int i = 0; i < 3; ++i)
        CHECK (tick (sw) == 1.0f);        // warm-up: standby runs but is not heard
    CHECK (sw.slots[1].calls == 3);

    // Fade 1 -> 5 over 4 samples, ending exactly on the new slot.
    for (const float expected : { 2.0f, 3.0f, 4.0f, 5.0f })
        CHECK (tick (sw) == expected);

    CHECK_FALSE (isSwapping (sw));
    CHECK (activeSlot (sw).value == 5.0f);
    const int oldCalls = sw.slots[0].calls;
    CHECK (tick (sw) == 5.0f);
    CHECK (sw.slots[0].calls == oldCalls);   // the old slot is idle again
}

TEST_CASE ("swapDesign refuses while a design is pending or a swap is running", "[dsp][swap]")
{
    FirSwap<CountingSlot> sw;
    prepareCounting (sw, 2);
    int designs = 0;
    auto design = [&] (CountingSlot& s) { ++designs; s.value = 7.0f; };

    REQUIRE (swapDesign (sw, design));
    CHECK_FALSE (swapDesign (sw, design));   // Ready, not yet picked up
    REQUIRE (swapBegin (sw, 1));
    CHECK_FALSE (swapDesign (sw, design));   // warming
    tick (sw);
    tick (sw);
    CHECK_FALSE (swapDesign (sw, design));   // fading
    tick (sw);
    CHECK_FALSE (isSwapping (sw));
    CHECK (swapDesign (sw, design));         // idle again
    CHECK (designs == 2);                    // refused calls never touched a slot
}

TEST_CASE ("swapBegin without a finished design does nothing", "[dsp][swap]")
{
    FirSwap<CountingSlot> sw;
    prepareCounting (sw, 4);
    CHECK_FALSE (swapBegin (sw, 10));
    CHECK_FALSE (isSwapping (sw));
    CHECK (tick (sw) == 1.0f);
    CHECK (sw.slots[1].calls == 0);
}

TEST_CASE ("swapBegin during a running swap does not restart it", "[dsp][swap]")
{
    FirSwap<CountingSlot> sw;
    prepareCounting (sw, 2);
    REQUIRE (swapDesign (sw, [] (CountingSlot& s) { s.value = 3.0f; }));
    REQUIRE (swapBegin (sw, 2));
    tick (sw);
    CHECK_FALSE (swapBegin (sw, 2));   // called every block: must not reset the warm-up
    tick (sw);
    CHECK (tick (sw) == 2.0f);         // fade step 1 of 2: halfway between 1 and 3
    CHECK (tick (sw) == 3.0f);
}

TEST_CASE ("zero warm-up goes straight to the fade", "[dsp][swap]")
{
    FirSwap<CountingSlot> sw;
    prepareCounting (sw, 2);
    REQUIRE (swapDesign (sw, [] (CountingSlot& s) { s.value = 3.0f; }));
    REQUIRE (swapBegin (sw, 0));
    CHECK (tick (sw) == 2.0f);
    CHECK (tick (sw) == 3.0f);
    CHECK_FALSE (isSwapping (sw));
}

TEST_CASE ("swapPrepare cancels an in-flight swap and re-arms the handshake", "[dsp][swap]")
{
    FirSwap<CountingSlot> sw;
    prepareCounting (sw, 100);
    REQUIRE (swapDesign (sw, [] (CountingSlot& s) { s.value = 9.0f; }));
    REQUIRE (swapBegin (sw, 0));
    tick (sw);
    REQUIRE (isSwapping (sw));

    prepareCounting (sw, 100);   // prepareToPlay mid-fade (sample-rate change)
    CHECK_FALSE (isSwapping (sw));
    CHECK (sw.active == 0);
    CHECK (tick (sw) == 1.0f);
    CHECK (swapDesign (sw, [] (CountingSlot&) {}));
}

TEST_CASE ("fade length below one sample is clamped to one", "[dsp][swap]")
{
    FirSwap<CountingSlot> sw;
    prepareCounting (sw, 0);
    REQUIRE (swapDesign (sw, [] (CountingSlot& s) { s.value = 4.0f; }));
    REQUIRE (swapBegin (sw, 0));
    CHECK (tick (sw) == 4.0f);   // no division by zero, lands on the new slot
    CHECK_FALSE (isSwapping (sw));
}

namespace {

// Allocating slot, like a real FIR slot: a torn read shows up as a non-uniform vector.
struct VectorSlot
{
    std::vector<float> data;
};

} // namespace

TEST_CASE ("concurrent designs: the audio thread only ever reads complete designs", "[dsp][swap][concurrency]")
{
    FirSwap<VectorSlot> sw;
    swapPrepare (sw, 32, [] (VectorSlot& s) { s.data.assign (64, 1.0f); });

    std::atomic<bool> stop { false };
    std::atomic<int> designs { 0 };
    std::thread designer ([&]
    {
        float v = 2.0f;
        while (! stop.load (std::memory_order_relaxed))
        {
            const auto size = static_cast<size_t> (64 + (designs.load (std::memory_order_relaxed) % 7) * 16);
            if (swapDesign (sw, [&] (VectorSlot& s) { s.data.assign (size, v); }))
            {
                designs.fetch_add (1, std::memory_order_relaxed);
                v += 1.0f;
            }
            std::this_thread::yield();
        }
    });

    bool torn = false;
    auto run = [&] (VectorSlot& s)
    {
        const float first = s.data.front();
        for (const float x : s.data)
            torn |= x != first;
        return first;
    };

    float previous = 1.0f;
    bool monotonic = true;
    for (int i = 0; i < 400000; ++i)
    {
        swapBegin (sw, 16);
        const float out = swapProcess (sw, run, lerp);
        monotonic &= out >= previous - 1e-4f;   // designs only ever increase in value
        previous = out;
    }
    stop = true;
    designer.join();

    INFO ("designs = " << designs.load());
    CHECK (designs.load() >= 2);
    CHECK_FALSE (torn);
    CHECK (monotonic);
}
