#pragma once
// Two-slot handover for FIR stages whose kernels are designed off the audio thread
// (band-domain crossover, FIR EQ). One slot is audible; the other is the standby.
//
//   background thread  swapDesign():  Idle -> Designing -> design(standby) -> Ready
//   audio thread       swapBegin():   Ready -> Consumed, start warm-up
//                      swapProcess(): warm-up (standby runs silently until its history is
//                                     complete) -> linear crossfade -> swap -> Idle
//
// The atomic handshake gives the happens-before edges: the audio thread touches the standby
// slot only after acquiring Ready, the background thread only after acquiring Idle, and the
// final Idle store (release) publishes the new `active` index to the next designer.
// The old kernel memory is freed by the next design on the background thread, never on the
// audio thread.
#include <algorithm>
#include <array>
#include <atomic>

namespace strat3::dsp {

namespace swapdetail {
enum Handshake : int { Idle, Designing, Ready, Consumed };
enum class Phase { Normal, Warming, Fading };
}

/** Not copyable or movable (atomic): construct in place, then swapPrepare(). */
template <typename Slot>
struct FirSwap
{
    std::array<Slot, 2> slots {};
    int active = 0;

    std::atomic<int> handshake { swapdetail::Idle };
    swapdetail::Phase phase = swapdetail::Phase::Normal;   // audio thread only
    int warmRemaining = 0;
    int fadeLength = 1;
    int fadePos = 0;
};

/** prepareToPlay only: no designer may run concurrently. Cancels any swap in flight.
 *  `prepareSlot(Slot&)` is called for both slots (allocate here, not on the audio thread). */
template <typename Slot, typename Fn>
void swapPrepare (FirSwap<Slot>& sw, int fadeSamples, Fn&& prepareSlot)
{
    for (auto& s : sw.slots)
        prepareSlot (s);
    sw.active = 0;
    sw.fadeLength = std::max (1, fadeSamples);
    sw.fadePos = 0;
    sw.warmRemaining = 0;
    sw.phase = swapdetail::Phase::Normal;
    sw.handshake.store (swapdetail::Idle, std::memory_order_release);
}

template <typename Slot>
Slot& activeSlot (FirSwap<Slot>& sw) noexcept { return sw.slots[static_cast<size_t> (sw.active)]; }

template <typename Slot>
const Slot& activeSlot (const FirSwap<Slot>& sw) noexcept { return sw.slots[static_cast<size_t> (sw.active)]; }

/** Background thread only. Runs `design(Slot& standby)` and marks it Ready. Returns false,
 *  without calling `design`, while a design is pending or a swap is running: the caller keeps
 *  its latest request and retries, so intermediate requests are coalesced. */
template <typename Slot, typename Fn>
bool swapDesign (FirSwap<Slot>& sw, Fn&& design)
{
    int expected = swapdetail::Idle;
    if (! sw.handshake.compare_exchange_strong (expected, swapdetail::Designing, std::memory_order_acq_rel))
        return false;

    // Acquiring Idle also acquired the last swap's `active` store: this is the silent slot.
    design (sw.slots[static_cast<size_t> (1 - sw.active)]);
    sw.handshake.store (swapdetail::Ready, std::memory_order_release);
    return true;
}

/** Audio thread, once per block: starts a swap if a design is Ready. `warmSamples` is how
 *  long the standby needs to run before its output matches a long-running filter. */
template <typename Slot>
bool swapBegin (FirSwap<Slot>& sw, int warmSamples) noexcept
{
    if (sw.phase != swapdetail::Phase::Normal || sw.handshake.load (std::memory_order_acquire) != swapdetail::Ready)
        return false;

    sw.handshake.store (swapdetail::Consumed, std::memory_order_relaxed);
    sw.warmRemaining = warmSamples;
    sw.fadePos = 0;
    sw.phase = warmSamples > 0 ? swapdetail::Phase::Warming : swapdetail::Phase::Fading;
    return true;
}

template <typename Slot>
bool isSwapping (const FirSwap<Slot>& sw) noexcept { return sw.phase != swapdetail::Phase::Normal; }

/** Audio thread, once per sample. `run(Slot&) -> T` processes one sample through a slot;
 *  `mix(T a, T b, float t) -> T` blends (t = 0 is a, 1 is b). The standby slot only runs
 *  while a swap is in progress, so an idle swap costs one slot. */
template <typename Slot, typename Run, typename Mix>
auto swapProcess (FirSwap<Slot>& sw, Run&& run, Mix&& mix) noexcept
{
    auto out = run (sw.slots[static_cast<size_t> (sw.active)]);
    if (sw.phase == swapdetail::Phase::Normal)
        return out;

    auto next = run (sw.slots[static_cast<size_t> (1 - sw.active)]);
    if (sw.phase == swapdetail::Phase::Warming)
    {
        if (--sw.warmRemaining <= 0)
            sw.phase = swapdetail::Phase::Fading;
        return out;
    }

    // Linear (equal-gain) crossfade: both slots see the same input and are highly correlated,
    // so equal-power would bump the level by up to 3 dB mid-fade.
    const float t = static_cast<float> (++sw.fadePos) / static_cast<float> (sw.fadeLength);
    out = mix (out, next, t);

    if (sw.fadePos >= sw.fadeLength)
    {
        sw.active = 1 - sw.active;
        sw.phase = swapdetail::Phase::Normal;
        sw.handshake.store (swapdetail::Idle, std::memory_order_release);   // publishes `active`
    }
    return out;
}

} // namespace strat3::dsp
