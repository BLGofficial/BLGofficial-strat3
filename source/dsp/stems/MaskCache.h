#pragma once
// STEMS mask cache: what the separator found, per stem, bin and frame, for one stretch of the
// host timeline. The layer looks masks up by timeline position, never by sample count, so a
// seek or a loop finds the right frame.
//   frame f covers [startSample + f * 512, startSample + f * 512 + 1024)
//   mask bytes: [frame][stem][bin], the 4 stems of a bin and frame sum to exactly 255, so with
//   every stem at 0 dB the masks pass the mix unchanged
//   prints: one fingerprint per 512 samples from startSample, to tell whether the audio
//   playing now is still the audio that was analysed
// 8-bit masks keep a 4-minute song near 46 MB at 48 kHz; 1/255 steps are -48 dB, far below
// what a gain of +-12 dB can make audible [INFERRED].
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "dsp/stems/Fingerprint.h"

namespace strat3::dsp {

// The order of the stem parameters (stems.voc, .bass, .drm, .oth).
enum Stem { kStemVoc, kStemBass, kStemDrm, kStemOth, kStems };

inline constexpr int kMaskFft = 1024;
inline constexpr int kMaskHop = 512;
inline constexpr int kMaskBins = kMaskFft / 2 + 1;
inline constexpr int kMaskFull = 255;

struct MaskCache
{
    double sampleRate = 0.0;
    std::int64_t startSample = 0;
    int frames = 0;
    std::vector<std::uint8_t> masks {};
    std::vector<BlockPrint> prints {};
};

/** Whole frames that fit in `samples` samples. */
inline int maskFramesFor (int samples) noexcept
{
    return samples < kMaskFft ? 0 : (samples - kMaskFft) / kMaskHop + 1;
}

inline std::size_t maskIndex (int frame, int stem, int bin) noexcept
{
    return (static_cast<std::size_t> (frame) * kStems + static_cast<std::size_t> (stem)) * kMaskBins + static_cast<std::size_t> (bin);
}

/** A mask as a gain in [0, 1]. No range checks: the caller holds a valid frame. */
inline float maskAt (const MaskCache& c, int frame, int stem, int bin) noexcept
{
    return static_cast<float> (c.masks[maskIndex (frame, stem, bin)]) * (1.0f / static_cast<float> (kMaskFull));
}

/** The frame whose start is nearest to `frameStart` (a timeline position), or -1 if that
 *  frame is not in the cache. */
inline int cacheFrameAt (const MaskCache& c, std::int64_t frameStart) noexcept
{
    const std::int64_t offset = frameStart - c.startSample + kMaskHop / 2;
    if (offset < 0)
        return -1;
    const std::int64_t f = offset / kMaskHop;
    return f < c.frames ? static_cast<int> (f) : -1;
}

/** Timeline positions [startSample, end) the cache has masks for. */
inline std::int64_t cacheEnd (const MaskCache& c) noexcept
{
    return c.frames <= 0 ? c.startSample : c.startSample + static_cast<std::int64_t> (c.frames - 1) * kMaskHop + kMaskFft;
}

} // namespace strat3::dsp
