#pragma once
// Depth blend for one layer: out = (1 - k) * delayedDry + k * wet, k = depth/100 (0..1.5).
// The dry path is delayed by exactly the layer's latency; without that, any k < 1 sums two
// time-shifted copies and comb-filters the master. Endpoint form (not dry + k*(wet-dry)) so
// k = 0 and k = 1 are bit-exact.
// A switched-off layer keeps delaying its dry path: reported plugin latency must not change
// when a user toggles a layer, or hosts re-align (or fail to) mid-session.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

#include "dsp/core/DelayLine.h"
#include "dsp/core/Smoother.h"

namespace strat3::dsp {

inline constexpr float kDepthRampMs = 30.0f;
inline constexpr float kDeltaRampMs = 10.0f;
inline constexpr float kMaxDepthPercent = 150.0f;

template <std::size_t Channels>
struct DepthBlend
{
    LinearRamp gain {};
    LinearRamp delta {};
    std::array<DelayLine, Channels> dry {};
    int latency = 0;
};

/** Allocates the dry delays. prepareToPlay only. Starts at gain 0 (silent wet). */
template <std::size_t N>
void prepare (DepthBlend<N>& b, double sampleRate, int maxLatencySamples)
{
    b.gain  = makeLinearRamp (kDepthRampMs, sampleRate, 0.0f);
    b.delta = makeLinearRamp (kDeltaRampMs, sampleRate, 0.0f);
    for (auto& line : b.dry)
        prepare (line, static_cast<std::size_t> (std::max (0, maxLatencySamples)));
    b.latency = std::min (b.latency, static_cast<int> (b.dry[0].capacity));
    for (auto& line : b.dry)
        setDelay (line, static_cast<std::size_t> (b.latency));
}

/** Clamped to the prepared capacity. Jumps: change latency only with the transport stopped
 *  or the layer silent, never mid-signal. */
template <std::size_t N>
void setLatency (DepthBlend<N>& b, int samples) noexcept
{
    for (auto& line : b.dry)
        setDelay (line, static_cast<std::size_t> (std::max (0, samples)));
    b.latency = static_cast<int> (getDelay (b.dry[0]));
}

template <std::size_t N>
int latencySamples (const DepthBlend<N>& b) noexcept { return b.latency; }

/** @param percent 0..150; non-finite values are ignored (the on flag still applies). */
template <std::size_t N>
void setDepth (DepthBlend<N>& b, float percent, bool on) noexcept
{
    const float k = std::isfinite (percent)
                        ? std::clamp (percent, 0.0f, kMaxDepthPercent) / 100.0f
                        : (b.gain.target > 0.0f ? b.gain.target : 1.0f);
    setTarget (b.gain, on ? k : 0.0f);
}

template <std::size_t N>
void setDelta (DepthBlend<N>& b, bool deltaOn) noexcept { setTarget (b.delta, deltaOn ? 1.0f : 0.0f); }

/** Jump to targets: use after prepare / state load, never during playback. */
template <std::size_t N>
void snapToTarget (DepthBlend<N>& b) noexcept
{
    for (auto* r : { &b.gain, &b.delta })
    {
        r->current = r->target;
        r->remaining = 0;
    }
}

template <std::size_t N>
float currentGain (const DepthBlend<N>& b) noexcept { return b.gain.current; }

/** True once the wet path contributes nothing and no fade is running: the caller may skip
 *  the layer's processing (CPU) but must still call process() to keep the dry delay fed.
 *  Reset the layer's state before using it again. */
template <std::size_t N>
bool canSkipWet (const DepthBlend<N>& b) noexcept { return b.gain.current == 0.0f && ! isSmoothing (b.gain); }

/** One frame: dry[c] is the layer's input, wet[c] the layer's output (already delayed by the
 *  layer itself). Writes out[c]. With delta on, out is what the layer adds: k * (wet - dry). */
template <std::size_t N>
void process (DepthBlend<N>& b, const float* dry, const float* wet, float* out) noexcept
{
    const float k = step (b.gain);
    const float d = step (b.delta);

    for (std::size_t c = 0; c < N; ++c)
    {
        const float dd = process (b.dry[c], dry[c]);
        if (d == 0.0f)
            out[c] = (1.0f - k) * dd + k * wet[c];          // branches keep the endpoints bit-exact
        else if (d == 1.0f)
            out[c] = k * (wet[c] - dd);
        else
            out[c] = (1.0f - d) * ((1.0f - k) * dd + k * wet[c]) + d * (k * (wet[c] - dd));
    }
}

} // namespace strat3::dsp
