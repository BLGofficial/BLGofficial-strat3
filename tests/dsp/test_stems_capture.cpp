// STEMS capture (module 22b): the ring the audio thread fills with the STEMS input during an
// analysis pass, and the analysis side drains. Contracts tested here:
//   - armed, it starts on the first block the host plays with a position, and keeps that
//     block's timeline position as the start
//   - it ends itself when the host stops, loses its position or jumps, and when the ring is
//     full (the reader fell behind); what came before stays readable, in order
//   - the reader can end it (budget reached, cancelled); a new pass needs the ring drained
//   - the writer never allocates; one writer and one reader on two threads lose nothing
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "dsp/stems/Capture.h"
#include "support/AllocCounter.h"

using namespace strat3::dsp;

namespace {

/** Distinct, checkable samples: left n, right -n (scaled). */
void ramp (std::vector<float>& l, std::vector<float>& r, std::int64_t from, int n)
{
    l.resize (static_cast<size_t> (n));
    r.resize (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
    {
        l[static_cast<size_t> (i)] = static_cast<float> (from + i) * 1e-4f;
        r[static_cast<size_t> (i)] = -static_cast<float> (from + i) * 1e-4f;
    }
}

Transport playing (std::int64_t at) { return { true, true, at }; }

} // namespace

TEST_CASE ("stems capture: armed, it starts on the first played block and keeps its position", "[dsp][stems][capture]")
{
    StemsCapture c;
    prepare (c, 4096);
    CHECK (state (c) == CaptureState::Idle);
    std::vector<float> l, r;
    ramp (l, r, 0, 256);
    write (c, l.data(), r.data(), 256, playing (0));   // not armed: ignored
    CHECK (available (c) == 0);

    REQUIRE (arm (c));
    CHECK (state (c) == CaptureState::Armed);
    CHECK_FALSE (arm (c));   // already armed
    write (c, l.data(), r.data(), 256, Transport { false, true, 0 });   // stopped: still waiting
    write (c, l.data(), r.data(), 256, Transport { true, false, 0 });   // no position: still waiting
    CHECK (state (c) == CaptureState::Armed);
    CHECK (available (c) == 0);

    ramp (l, r, 48000, 256);
    write (c, l.data(), r.data(), 256, playing (48000));
    CHECK (state (c) == CaptureState::Running);
    CHECK (startSample (c) == 48000);
    ramp (l, r, 48256, 100);
    write (c, l.data(), r.data(), 100, playing (48256));
    REQUIRE (available (c) == 356);

    std::vector<float> ol (356), or_ (356);
    CHECK (read (c, ol.data(), or_.data(), 1000) == 356);
    for (int i = 0; i < 356; ++i)
    {
        CHECK (ol[static_cast<size_t> (i)] == static_cast<float> (48000 + i) * 1e-4f);
        CHECK (or_[static_cast<size_t> (i)] == -static_cast<float> (48000 + i) * 1e-4f);
    }
    CHECK (available (c) == 0);
}

TEST_CASE ("stems capture: it ends itself when the host stops, loses its position or jumps", "[dsp][stems][capture]")
{
    struct Case { Transport t; CaptureEnd why; };
    for (const auto& k : { Case { { false, true, 512 }, CaptureEnd::Stopped },
                           Case { { true, false, 512 }, CaptureEnd::Stopped },
                           Case { { true, true, 9000 }, CaptureEnd::Jumped },
                           Case { { true, true, 0 }, CaptureEnd::Jumped } })
    {
        StemsCapture c;
        prepare (c, 4096);
        REQUIRE (arm (c));
        std::vector<float> l, r;
        ramp (l, r, 0, 512);
        write (c, l.data(), r.data(), 512, playing (0));
        ramp (l, r, 512, 512);
        write (c, l.data(), r.data(), 512, k.t);
        CHECK (state (c) == CaptureState::Ended);
        CHECK (endReason (c) == k.why);
        CHECK (available (c) == 512);   // the first block, not the one that ended it
        write (c, l.data(), r.data(), 512, playing (1024));   // ended: ignored
        CHECK (available (c) == 512);
    }
}

TEST_CASE ("stems capture: a full ring ends it, keeping what fit", "[dsp][stems][capture]")
{
    StemsCapture c;
    prepare (c, 1000);
    REQUIRE (arm (c));
    std::vector<float> l, r;
    ramp (l, r, 0, 600);
    write (c, l.data(), r.data(), 600, playing (0));
    ramp (l, r, 600, 600);
    write (c, l.data(), r.data(), 600, playing (600));   // 1200 > 1000: the reader fell behind
    CHECK (state (c) == CaptureState::Ended);
    CHECK (endReason (c) == CaptureEnd::Overflow);
    std::vector<float> ol (1000), or_ (1000);
    CHECK (read (c, ol.data(), or_.data(), 1000) == 600);
    CHECK (ol[599] == 599 * 1e-4f);
}

TEST_CASE ("stems capture: the ring wraps around without losing order", "[dsp][stems][capture]")
{
    StemsCapture c;
    prepare (c, 1000);
    REQUIRE (arm (c));
    std::vector<float> l, r, ol (700), or_ (700);
    std::int64_t pos = 0, got = 0;
    for (int round = 0; round < 10; ++round)
    {
        ramp (l, r, pos, 700);
        write (c, l.data(), r.data(), 700, playing (pos));
        pos += 700;
        REQUIRE (state (c) == CaptureState::Running);
        const int n = read (c, ol.data(), or_.data(), 700);
        REQUIRE (n == 700);
        for (int i = 0; i < n; ++i)
            REQUIRE (ol[static_cast<size_t> (i)] == static_cast<float> (got + i) * 1e-4f);
        got += n;
    }
}

TEST_CASE ("stems capture: the reader can end it, and a new pass needs the ring drained", "[dsp][stems][capture]")
{
    StemsCapture c;
    prepare (c, 4096);
    std::vector<float> l, r, ol (4096), or_ (4096);

    SECTION ("ended while waiting for the host")
    {
        REQUIRE (arm (c));
        end (c, CaptureEnd::Cancelled);
        CHECK (state (c) == CaptureState::Ended);
        CHECK (endReason (c) == CaptureEnd::Cancelled);
        ramp (l, r, 0, 256);
        write (c, l.data(), r.data(), 256, playing (0));
        CHECK (available (c) == 0);
    }
    SECTION ("ended while running (budget reached)")
    {
        REQUIRE (arm (c));
        ramp (l, r, 0, 256);
        write (c, l.data(), r.data(), 256, playing (0));
        end (c, CaptureEnd::Full);
        CHECK (endReason (c) == CaptureEnd::Full);
        write (c, l.data(), r.data(), 256, playing (256));
        CHECK (available (c) == 256);
        CHECK_FALSE (reset (c));   // not drained yet
        CHECK_FALSE (arm (c));
        read (c, ol.data(), or_.data(), 4096);
    }
    end (c, CaptureEnd::Stopped);   // already ended: the first reason stays
    CHECK (endReason (c) != CaptureEnd::Stopped);
    REQUIRE (reset (c));
    CHECK (state (c) == CaptureState::Idle);
    REQUIRE (arm (c));
    ramp (l, r, 777, 128);
    write (c, l.data(), r.data(), 128, playing (777));
    CHECK (startSample (c) == 777);
    CHECK (available (c) == 128);
}

TEST_CASE ("stems capture: writing never allocates", "[dsp][stems][capture]")
{
    StemsCapture c;
    prepare (c, 48000);
    REQUIRE (arm (c));
    std::vector<float> l (480, 0.1f), r (480, 0.2f);
    std::size_t allocations = 0;
    {
        const strat3::test::ScopedAllocCounter counter;
        for (int b = 0; b < 50; ++b)
            write (c, l.data(), r.data(), 480, playing (b * 480));
        allocations = counter.count();
    }
    CHECK (allocations == 0);
    CHECK (available (c) == 50 * 480);
}

TEST_CASE ("stems capture: one writer and one reader on two threads lose nothing", "[dsp][stems][capture][threads]")
{
    StemsCapture c;
    prepare (c, 8192);
    REQUIRE (arm (c));
    constexpr int kBlock = 256, kBlocks = 4000;
    std::atomic<bool> done { false };
    std::thread writer ([&]
    {
        std::vector<float> l, r;
        for (int b = 0; b < kBlocks; ++b)
        {
            ramp (l, r, static_cast<std::int64_t> (b) * kBlock, kBlock);
            // A full ring would end the pass: wait for room, as a reader keeping up would give.
            while (available (c) > 8192 - kBlock)
                std::this_thread::yield();
            write (c, l.data(), r.data(), kBlock, playing (static_cast<std::int64_t> (b) * kBlock));
        }
        done.store (true);
    });
    std::vector<float> ol (1000), or_ (1000);
    std::int64_t got = 0;
    bool inOrder = true;
    while (! done.load() || available (c) > 0)
    {
        const int n = read (c, ol.data(), or_.data(), 1000);
        for (int i = 0; i < n; ++i)
            inOrder = inOrder && ol[static_cast<size_t> (i)] == static_cast<float> (got + i) * 1e-4f
                      && or_[static_cast<size_t> (i)] == -static_cast<float> (got + i) * 1e-4f;
        got += n;
    }
    writer.join();
    CHECK (inOrder);
    CHECK (got == static_cast<std::int64_t> (kBlock) * kBlocks);
    CHECK (state (c) == CaptureState::Running);
}
