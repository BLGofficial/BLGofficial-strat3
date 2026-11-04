#pragma once
// Shared crossover points for the band domain, plus the sanitiser that keeps them usable.
// Host ranges overlap on purpose (see params); ordering and spacing are enforced here.
#include <algorithm>
#include <array>
#include <cmath>

namespace strat3::dsp {

struct CrossoverPoints
{
    float f1 = 120.0f, f2 = 1000.0f, f3 = 5000.0f;
};

inline constexpr CrossoverPoints kDefaultCrossovers { 120.0f, 1000.0f, 5000.0f };
inline constexpr float kMinCrossoverHz    = 20.0f;
// About 1/3 octave. Closer LR4 points make the middle band collapse into a narrow, lossy
// bump; this also keeps FIR band differences (LP2 - LP1) well conditioned.
inline constexpr float kMinCrossoverRatio = 1.25f;

/** Orders, spaces and range-limits crossover points for `sampleRate`.
 *  Valid input is returned bit-identical (no needless coefficient redesign).
 *  Non-finite input or sampleRate <= 0 yields kDefaultCrossovers. */
inline CrossoverPoints sanitiseCrossovers (CrossoverPoints p, double sampleRate) noexcept
{
    if (sampleRate <= 0.0 || ! std::isfinite (p.f1) || ! std::isfinite (p.f2) || ! std::isfinite (p.f3))
        return kDefaultCrossovers;

    const float hi = static_cast<float> (0.45 * sampleRate);
    std::array<float, 3> f { std::clamp (p.f1, kMinCrossoverHz, hi),
                             std::clamp (p.f2, kMinCrossoverHz, hi),
                             std::clamp (p.f3, kMinCrossoverHz, hi) };
    std::sort (f.begin(), f.end());

    // Push up for spacing, then pull down from the top if that overshot Nyquist.
    f[1] = std::max (f[1], f[0] * kMinCrossoverRatio);
    f[2] = std::max (f[2], f[1] * kMinCrossoverRatio);
    f[2] = std::min (f[2], hi);
    f[1] = std::min (f[1], f[2] / kMinCrossoverRatio);
    f[0] = std::max (std::min (f[0], f[1] / kMinCrossoverRatio), kMinCrossoverHz);

    return { f[0], f[1], f[2] };
}

} // namespace strat3::dsp
