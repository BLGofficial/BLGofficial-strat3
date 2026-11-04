#pragma once
// Band domain: one stereo 4-band split shared by consecutive band layers (MULTIBAND, WIDTH,
// SATURATE), then one merge. Splitting once instead of per layer is the CPU saving from the
// concept ledger, and it keeps every band layer phase-aligned with the others.
//
// IIR mode (TRACK/MIX): LR4, zero latency, crossover changes glide in place.
// FIR mode (MASTER): linear phase. Crossover changes need new kernels: designed on a
// background thread and handed over through FirSwap (warm-up, then a 50 ms crossfade).
#include <array>
#include <cmath>

#include "dsp/core/FirSwap.h"
#include "dsp/crossover/CrossoverPoints.h"
#include "dsp/crossover/FirCrossover.h"
#include "dsp/crossover/Lr4Crossover.h"

namespace strat3::dsp {

enum class SplitMode { Iir, Fir };

struct StereoBands
{
    std::array<float, 4> l {}, r {};
};

inline constexpr float kFirSwapFadeMs = 50.0f;

/** One FIR design: a split per channel plus the points it was designed for. */
struct FirSplitSlot
{
    std::array<FirSplit4, 2> ch {};
    CrossoverPoints points = kDefaultCrossovers;
};

/** Not copyable or movable (atomic in FirSwap): construct in place, then prepare(). */
struct BandDomain
{
    SplitMode mode = SplitMode::Iir;
    double sampleRate = 48000.0;
    int partition = 256;

    std::array<Lr4Split4, 2> iir {};                      // [channel]
    CrossoverPoints iirPoints = kDefaultCrossovers;

    FirSwap<FirSplitSlot> fir {};
};

namespace banddetail {
inline void designSlot (FirSplitSlot& slot, CrossoverPoints p, double sampleRate, int partition)
{
    for (auto& s : slot.ch)
        prepare (s, p, sampleRate, partition);
    slot.points = p;
}
} // namespace banddetail

/** Allocates both FIR slots in FIR mode. prepareToPlay only (no concurrent designStandby). */
inline void prepare (BandDomain& d, SplitMode mode, CrossoverPoints points, double sampleRate, int partition = 256)
{
    d.mode = mode;
    d.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    d.partition = partition;
    const auto p = sanitiseCrossovers (points, d.sampleRate);

    d.iirPoints = p;
    for (auto& s : d.iir)
        s = makeLr4Split4 (p, d.sampleRate);

    const int fade = static_cast<int> (std::lround (static_cast<double> (kFirSwapFadeMs) * 0.001 * d.sampleRate));
    if (mode == SplitMode::Fir)
        swapPrepare (d.fir, fade, [&] (FirSplitSlot& slot) { banddetail::designSlot (slot, p, d.sampleRate, partition); });
    else
        swapPrepare (d.fir, fade, [] (FirSplitSlot&) {});   // IIR: no kernels, but a clean handshake
}

inline int latencySamples (const BandDomain& d) noexcept
{
    return d.mode == SplitMode::Fir ? latencySamples (activeSlot (d.fir).ch[0]) : 0;
}

/** Background thread only (allocates, designs FIRs). Returns false if the domain is in IIR
 *  mode or a swap is already pending or running; the caller retries with the latest points. */
inline bool designStandby (BandDomain& d, CrossoverPoints points)
{
    if (d.mode != SplitMode::Fir)
        return false;

    const auto p = sanitiseCrossovers (points, d.sampleRate);
    return swapDesign (d.fir, [&] (FirSplitSlot& slot) { banddetail::designSlot (slot, p, d.sampleRate, d.partition); });
}

/** Audio thread, once per block: picks up a finished standby design. */
inline void beginBlock (BandDomain& d) noexcept
{
    if (d.mode != SplitMode::Fir)
        return;
    // The standby slot starts from silence; its output equals a long-running split only once
    // every tap and the convolver partition have seen real input.
    swapBegin (d.fir, static_cast<int> (firCrossoverTaps (d.sampleRate)) - 1 + d.partition);
}

inline bool isSwapping (const BandDomain& d) noexcept { return isSwapping (d.fir); }

/** Audio thread (IIR mode): glide to new points, no redesign needed. */
inline void setIirCrossovers (BandDomain& d, CrossoverPoints points) noexcept
{
    if (d.mode != SplitMode::Iir)
        return;
    d.iirPoints = sanitiseCrossovers (points, d.sampleRate);
    for (auto& s : d.iir)
        setCrossovers (s, d.iirPoints);
}

/** Points the audible split is using (audio thread / after the swap completes). */
inline CrossoverPoints activeCrossovers (const BandDomain& d) noexcept
{
    return d.mode == SplitMode::Fir ? activeSlot (d.fir).points : d.iirPoints;
}

inline StereoBands split (BandDomain& d, float l, float r) noexcept
{
    StereoBands out;
    if (d.mode == SplitMode::Iir)
    {
        out.l = process (d.iir[0], l);
        out.r = process (d.iir[1], r);
        return out;
    }

    auto run = [&] (FirSplitSlot& slot)
    {
        StereoBands b;
        b.l = process (slot.ch[0], l);
        b.r = process (slot.ch[1], r);
        return b;
    };
    auto mix = [] (StereoBands a, const StereoBands& b, float t)
    {
        for (size_t i = 0; i < 4; ++i)
        {
            a.l[i] += t * (b.l[i] - a.l[i]);
            a.r[i] += t * (b.r[i] - a.r[i]);
        }
        return a;
    };
    return swapProcess (d.fir, run, mix);
}

inline void merge (const StereoBands& bands, float& l, float& r) noexcept
{
    l = (bands.l[0] + bands.l[1]) + (bands.l[2] + bands.l[3]);
    r = (bands.r[0] + bands.r[1]) + (bands.r[2] + bands.r[3]);
}

} // namespace strat3::dsp
