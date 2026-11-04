#pragma once
// Integer-sample delay line, used to delay each layer's dry path by exactly that layer's
// latency so depth blending (dry + k * (wet - dry)) does not comb-filter.
// Changing the delay at runtime jumps; callers crossfade if they change it during playback.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace strat3::dsp {

struct DelayLine
{
    std::vector<float> buffer {};
    std::size_t mask     = 0;
    std::size_t write    = 0;
    std::size_t delay    = 0;
    std::size_t capacity = 0;   // max delay the buffer was prepared for
};

/** Allocates. Call from prepareToPlay only, never from the audio thread. */
inline void prepare (DelayLine& d, std::size_t maxDelaySamples)
{
    std::size_t size = 1;
    while (size < maxDelaySamples + 1)   // power of two: wrap with a mask instead of a modulo
        size <<= 1;
    d.buffer.assign (size, 0.0f);
    d.mask     = size - 1;
    d.write    = 0;
    d.capacity = maxDelaySamples;
    d.delay    = std::min (d.delay, d.capacity);
}

/** Clamped to the prepared capacity; an unprepared line clamps to 0 (passthrough). */
inline void setDelay (DelayLine& d, std::size_t samples) noexcept { d.delay = std::min (samples, d.capacity); }

inline std::size_t getDelay (const DelayLine& d) noexcept { return d.delay; }

inline float process (DelayLine& d, float x) noexcept
{
    if (d.buffer.empty())
        return x;   // processBlock before prepareToPlay: pass audio rather than crash

    d.buffer[d.write] = x;
    const float y = d.buffer[(d.write - d.delay) & d.mask];   // unsigned wrap is well-defined
    d.write = (d.write + 1) & d.mask;
    return y;
}

/** In place. Same result as calling process() per sample. */
inline void processBlock (DelayLine& d, float* samples, int numSamples) noexcept
{
    for (int i = 0; i < numSamples; ++i)
        samples[i] = process (d, samples[i]);
}

inline void reset (DelayLine& d) noexcept
{
    std::fill (d.buffer.begin(), d.buffer.end(), 0.0f);
}

/** Replaces non-finite samples with 0, leaving valid history intact.
 *  Returns true if anything was replaced. O(capacity): call once per block at most, and
 *  only after an upstream NaN flag, not unconditionally. */
inline bool sanitise (DelayLine& d) noexcept
{
    bool replaced = false;
    for (auto& v : d.buffer)
        if (! std::isfinite (v))
        {
            v = 0.0f;
            replaced = true;
        }
    return replaced;
}

} // namespace strat3::dsp
