#pragma once
// Linear-phase 4-band split for MASTER mode.
// Bands are built as differences of three linear-phase low-passes with identical length:
//   b1 = LP1,  b2 = LP2 - LP1,  b3 = LP3 - LP2,  b4 = delay(x) - LP3
// so b1 + b2 + b3 + b4 telescopes to the delayed input exactly (to float rounding).
// Runtime crossover changes (kernel redesign + crossfade) are handled one level up, in the
// band domain, because they need a background thread and a second convolver set.
#include <array>

#include "dsp/core/DelayLine.h"
#include "dsp/crossover/CrossoverPoints.h"
#include "dsp/crossover/FirDesign.h"
#include "dsp/crossover/UniformConvolver.h"

namespace strat3::dsp {

using FirBands4 = std::array<float, 4>;

struct FirSplit4
{
    UniformConvolver conv {};
    DelayLine dry {};
    int latency = 0;
    bool prepared = false;
};

/** Designs kernels and allocates. prepareToPlay / background thread only. */
inline void prepare (FirSplit4& s, CrossoverPoints points, double sampleRate, int partition = 256)
{
    const auto p = sanitiseCrossovers (points, sampleRate);
    const auto taps = firCrossoverTaps (sampleRate);
    prepare (s.conv, partition, { designLinearPhaseLowpass (p.f1, sampleRate, taps),
                                  designLinearPhaseLowpass (p.f2, sampleRate, taps),
                                  designLinearPhaseLowpass (p.f3, sampleRate, taps) });

    s.latency = static_cast<int> (taps / 2) + latencySamples (s.conv);
    prepare (s.dry, static_cast<size_t> (s.latency));
    setDelay (s.dry, static_cast<size_t> (s.latency));
    s.prepared = true;
}

/** Group delay of the FIRs plus the convolver's partition. Report this to the host. */
inline int latencySamples (const FirSplit4& s) noexcept { return s.prepared ? s.latency : 0; }

inline FirBands4 process (FirSplit4& s, float x) noexcept
{
    if (! s.prepared)
        return { 0.0f, 0.0f, 0.0f, x };   // before prepareToPlay: pass audio through on the sum

    float lp[3];
    process (s.conv, x, lp);
    const float delayed = process (s.dry, x);
    return { lp[0], lp[1] - lp[0], lp[2] - lp[1], delayed - lp[2] };
}

inline void reset (FirSplit4& s) noexcept
{
    reset (s.conv);
    reset (s.dry);
}

} // namespace strat3::dsp
