#pragma once
// STEMS capture (module 22b): the ring the audio thread fills with the STEMS input during an
// analysis pass, drained by the thread that separates it. One writer (the audio thread), one
// reader (the analysis worker).
//
// The pass follows the host's timeline: armed, it starts on the first block that is playing
// with a position, and it ends itself when the host stops, loses its position or jumps (a
// seek or loop would splice two stretches into one analysis). A full ring also ends it: the
// reader fell behind, and dropping samples in the middle would misalign every later mask. The
// reader can end it too (mask budget reached, cancelled).
//
// The written count and the state share one atomic word, so a block the writer was copying
// when the reader ended or reset the pass can never be committed: its compare-and-swap fails
// and the samples sit past the count, where nobody reads them.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <vector>

#include "dsp/stems/Transport.h"

namespace strat3::dsp {

enum class CaptureState { Idle, Armed, Running, Ended };
enum class CaptureEnd { None, Stopped, Jumped, Overflow, Full, Cancelled };

struct StemsCapture
{
    std::vector<float> l {}, r {};
    int capacity = 0;
    // bits 0..7: 0 idle, 1 armed, 2 running, 3 + CaptureEnd ended; bits 8..63: samples written.
    std::atomic<std::uint64_t> word { 0 };
    std::atomic<std::int64_t> readCount { 0 };   // reader-owned
    std::atomic<std::int64_t> start { 0 };       // published by the armed -> running swap
    std::int64_t next = 0;                       // writer-owned: where the next block must start
};

static_assert (std::atomic<std::uint64_t>::is_always_lock_free);
static_assert (std::atomic<std::int64_t>::is_always_lock_free);

namespace capdetail {

inline constexpr std::uint64_t kIdle = 0, kArmed = 1, kRunning = 2, kEnded = 3;

inline constexpr std::uint64_t pack (std::int64_t written, std::uint64_t code) noexcept
{
    return (static_cast<std::uint64_t> (written) << 8) | code;
}
inline constexpr std::uint64_t code (std::uint64_t w) noexcept { return w & 0xFFu; }
inline constexpr std::int64_t written (std::uint64_t w) noexcept { return static_cast<std::int64_t> (w >> 8); }
inline constexpr std::uint64_t endCode (CaptureEnd why) noexcept { return kEnded + static_cast<std::uint64_t> (why); }

/** Writer: ends the pass it saw. No retry: if the word moved, the reader ended or reset the
    pass meanwhile, and a retry could end the next pass the reader has already armed. */
inline void endSeen (StemsCapture& c, std::uint64_t seen, CaptureEnd why) noexcept
{
    c.word.compare_exchange_strong (seen, pack (written (seen), endCode (why)), std::memory_order_acq_rel);
}

} // namespace capdetail

/** Message thread, audio stopped: sizes the ring (samples per channel) and goes idle. */
inline void prepare (StemsCapture& c, int capacity)
{
    c.capacity = std::max (1, capacity);
    c.l.assign (static_cast<size_t> (c.capacity), 0.0f);
    c.r.assign (static_cast<size_t> (c.capacity), 0.0f);
    c.word.store (0);
    c.readCount.store (0);
    c.start.store (0);
    c.next = 0;
}

/** Reader: idle -> armed. False when a pass is armed, running, or ended and not yet reset. */
inline bool arm (StemsCapture& c) noexcept
{
    auto seen = capdetail::pack (0, capdetail::kIdle);
    return c.word.compare_exchange_strong (seen, capdetail::pack (0, capdetail::kArmed), std::memory_order_acq_rel);
}

/** Audio thread: offers one block of the STEMS input with the host's transport for it. */
inline void write (StemsCapture& c, const float* l, const float* r, int n, const Transport& t) noexcept
{
    using namespace capdetail;
    auto seen = c.word.load (std::memory_order_acquire);
    const bool rolling = t.playing && t.hasPosition;
    if (code (seen) == kArmed)
    {
        if (! rolling)
            return;   // still waiting for the host to play
        c.start.store (t.sample, std::memory_order_relaxed);
        c.next = t.sample;
    }
    else if (code (seen) == kRunning)
    {
        if (! rolling)
            return endSeen (c, seen, CaptureEnd::Stopped);
        if (t.sample != c.next)
            return endSeen (c, seen, CaptureEnd::Jumped);
    }
    else
    {
        return;
    }
    if (n <= 0)
        return;

    const auto w = written (seen);
    if (w - c.readCount.load (std::memory_order_acquire) + n > c.capacity)
        return endSeen (c, seen, CaptureEnd::Overflow);

    const auto at = static_cast<int> (w % c.capacity);
    const int first = std::min (n, c.capacity - at);
    std::copy (l, l + first, c.l.begin() + at);
    std::copy (r, r + first, c.r.begin() + at);
    std::copy (l + first, l + n, c.l.begin());
    std::copy (r + first, r + n, c.r.begin());
    // Fails only when the reader ended (or ended and reset) the pass meanwhile: the block is
    // then dropped, which is what ending means.
    if (c.word.compare_exchange_strong (seen, pack (w + n, kRunning), std::memory_order_acq_rel))
        c.next = t.sample + n;
}

/** Reader: takes up to `maxN` samples per channel, oldest first. Returns how many. */
inline int read (StemsCapture& c, float* l, float* r, int maxN) noexcept
{
    const auto w = capdetail::written (c.word.load (std::memory_order_acquire));
    const auto rd = c.readCount.load (std::memory_order_relaxed);
    const int n = static_cast<int> (std::min<std::int64_t> (std::max (0, maxN), w - rd));
    if (n <= 0)
        return 0;
    const auto at = static_cast<int> (rd % c.capacity);
    const int first = std::min (n, c.capacity - at);
    std::copy (c.l.begin() + at, c.l.begin() + at + first, l);
    std::copy (c.r.begin() + at, c.r.begin() + at + first, r);
    std::copy (c.l.begin(), c.l.begin() + (n - first), l + first);
    std::copy (c.r.begin(), c.r.begin() + (n - first), r + first);
    c.readCount.store (rd + n, std::memory_order_release);
    return n;
}

/** Reader: ends an armed or running pass. One already ended keeps its first reason. */
inline void end (StemsCapture& c, CaptureEnd why) noexcept
{
    using namespace capdetail;
    auto seen = c.word.load (std::memory_order_acquire);
    // Retries while the writer commits blocks (the count moves under us).
    while (code (seen) == kArmed || code (seen) == kRunning)
        if (c.word.compare_exchange_weak (seen, pack (written (seen), endCode (why)), std::memory_order_acq_rel))
            return;
}

/** Reader: ended and drained -> idle. False otherwise (what is left must be read first). */
inline bool reset (StemsCapture& c) noexcept
{
    using namespace capdetail;
    auto seen = c.word.load (std::memory_order_acquire);
    if (code (seen) < kEnded || written (seen) != c.readCount.load (std::memory_order_relaxed))
        return false;
    // Only the reader leaves the ended state, so plain stores suffice. A writer still finishing
    // a block of the old pass may read the zeroed count; its commit then fails on the word.
    c.readCount.store (0, std::memory_order_relaxed);
    c.word.store (pack (0, kIdle), std::memory_order_release);
    return true;
}

inline CaptureState state (const StemsCapture& c) noexcept
{
    const auto k = capdetail::code (c.word.load (std::memory_order_acquire));
    return k >= capdetail::kEnded ? CaptureState::Ended : static_cast<CaptureState> (k);
}

inline CaptureEnd endReason (const StemsCapture& c) noexcept
{
    const auto k = capdetail::code (c.word.load (std::memory_order_acquire));
    return k >= capdetail::kEnded ? static_cast<CaptureEnd> (k - capdetail::kEnded) : CaptureEnd::None;
}

/** Timeline position of the pass's first sample (valid once it has run). */
inline std::int64_t startSample (const StemsCapture& c) noexcept
{
    return c.start.load (std::memory_order_relaxed);
}

/** Samples written and not yet read. */
inline std::int64_t available (const StemsCapture& c) noexcept
{
    const auto w = capdetail::written (c.word.load (std::memory_order_acquire));
    return w - c.readCount.load (std::memory_order_acquire);
}

} // namespace strat3::dsp
