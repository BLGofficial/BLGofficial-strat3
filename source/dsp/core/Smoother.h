#pragma once
// Parameter smoothers. Plain structs + free functions: trivially copyable, no allocation,
// safe on the audio thread. Construct with make*() in prepareToPlay.
#include <cmath>

namespace strat3::dsp {

// Below this distance to the target a one-pole snaps. Far above FLT_MIN (1.2e-38), so the
// state can never decay into subnormal range where some CPUs run 10-100x slower.
inline constexpr float kSnapEpsilon = 1e-15f;

/** Exponential (RC-style) smoother. After `timeMs` it has covered 1 - 1/e of a step.
 *  Use for filter coefficients, where an exponential approach sounds smoother than a ramp. */
struct OnePoleSmoother
{
    float coeff = 0.0f;   // 0 = jump immediately
    float state = 0.0f;
};

/** @param timeMs time constant; <= 0, or an unprepared sample rate (<= 0), means no smoothing. */
inline OnePoleSmoother makeOnePole (float timeMs, double sampleRate, float initial) noexcept
{
    const double samples = static_cast<double> (timeMs) * 0.001 * sampleRate;
    const float coeff = samples > 0.0 ? static_cast<float> (std::exp (-1.0 / samples)) : 0.0f;
    return { coeff, std::isfinite (initial) ? initial : 0.0f };
}

/** Advances one sample toward `target` and returns the new value.
 *  A non-finite target is ignored: one NaN from a host or UI must not poison the state. */
inline float step (OnePoleSmoother& s, float target) noexcept
{
    if (! std::isfinite (target))
        return s.state;

    const float next = target + s.coeff * (s.state - target);
    // Snap when (a) close enough to rule out subnormals, or (b) float rounding stalls the
    // approach: with coeff near 1, target + coeff*d rounds back to the old value while d is
    // still ~100s of ulps, and the smoother would sit just off target forever.
    s.state = (std::abs (next - target) <= kSnapEpsilon || next == s.state) ? target : next;
    return s.state;
}

/** Linear ramp that lands exactly on the target after a fixed number of samples.
 *  Use for gains and depth, where a predictable end time matters more than the curve. */
struct LinearRamp
{
    float current   = 0.0f;
    float start     = 0.0f;   // value when the current ramp began
    float target    = 0.0f;
    float increment = 0.0f;
    int   remaining = 0;
    int   length    = 0;   // ramp length in samples; 0 = jump immediately
};

/** @param timeMs ramp duration; <= 0, or sampleRate <= 0, means no smoothing. */
inline LinearRamp makeLinearRamp (float timeMs, double sampleRate, float initial) noexcept
{
    const double samples = static_cast<double> (timeMs) * 0.001 * sampleRate;
    const int length = samples > 0.0 ? static_cast<int> (std::lround (samples)) : 0;
    const float v = std::isfinite (initial) ? initial : 0.0f;
    return { v, v, v, 0.0f, 0, length };
}

/** Starts a new ramp from the current value. Non-finite targets are ignored, and so is an
 *  unchanged target: callers re-send every parameter each block, and restarting the ramp
 *  then would make it approach geometrically and never land. */
inline void setTarget (LinearRamp& r, float target) noexcept
{
    if (! std::isfinite (target) || target == r.target)
        return;

    r.target = target;
    if (r.length <= 0)
    {
        r.current = target;
        r.remaining = 0;
        return;
    }
    r.start = r.current;
    r.increment = (target - r.current) / static_cast<float> (r.length);
    r.remaining = r.length;
}

inline float step (LinearRamp& r) noexcept
{
    if (r.remaining > 0)
    {
        --r.remaining;
        // start + i * increment instead of accumulating: summing the increment 1000s of times
        // drifts, which made the final snap-to-target a visible step. The last step assigns
        // the exact target so gains land bit-exactly (null tests, bit-exact bypass).
        r.current = r.remaining == 0
                        ? r.target
                        : r.start + r.increment * static_cast<float> (r.length - r.remaining);
    }
    return r.current;
}

inline bool isSmoothing (const LinearRamp& r) noexcept { return r.remaining > 0; }

/** Writes `numSamples` successive ramp values into `out`. Identical to calling step() per sample. */
inline void stepBlock (LinearRamp& r, float* out, int numSamples) noexcept
{
    for (int i = 0; i < numSamples; ++i)
        out[i] = step (r);
}

} // namespace strat3::dsp
