#pragma once
// STEMS block fingerprint: the spectral shape of every 512 samples of the mono sum, so the
// layer can tell whether the audio playing now is the audio the masks were made from.
//   4 bands from 3 low-passes (200 Hz, 2 kHz, 8 kHz): below 200, 200-2k, 2k-8k, above 8k
//   shape: each band's energy against the sum of the bands, in whole dB, -40..0
//   level: the block's mean square in whole dB; under -70 dBFS the block counts as silent
// The match ignores level on purpose: a trim or fader change before the plugin is the same
// mix. It compares the shape only, within 6 dB per band, which a changed mix (an edit, another
// take, a muted part) breaks within a block or two [INFERRED tuning].
// The offline pass and the layer run this same code, so a print of the same audio is the same.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "dsp/core/SvfTpt.h"

namespace strat3::dsp {

inline constexpr int kPrintBands = 4;
inline constexpr int kPrintBlock = 512;
inline constexpr std::array<float, kPrintBands - 1> kPrintSplitHz { 200.0f, 2000.0f, 8000.0f };
inline constexpr int kPrintFloorDb = -40;
inline constexpr int kPrintSilentDb = -70;
inline constexpr int kPrintToleranceDb = 6;

struct BlockPrint
{
    std::array<std::int8_t, kPrintBands> shapeDb {};
    std::int8_t levelDb = -128;
    bool operator== (const BlockPrint&) const = default;
};

struct PrintMeter
{
    double sampleRate = 48000.0;
    std::array<SvfCoeffs, kPrintBands - 1> lp {};
    std::array<SvfState, kPrintBands - 1> state {};
    std::array<double, kPrintBands> bandEnergy {};
    double energy = 0.0;
    int count = 0;
    BlockPrint last {};
};

namespace printdetail {

inline std::int8_t wholeDb (double ratio, int lo, int hi) noexcept
{
    if (! (ratio > 0.0))   // also catches NaN
        return static_cast<std::int8_t> (lo);
    return static_cast<std::int8_t> (std::clamp (static_cast<int> (std::lround (10.0 * std::log10 (ratio))), lo, hi));
}

inline BlockPrint makePrint (const PrintMeter& m) noexcept
{
    BlockPrint p;
    p.levelDb = wholeDb (m.energy / kPrintBlock, -128, 127);
    if (p.levelDb < kPrintSilentDb)
        return p;   // silent: the shape of noise floor or dither says nothing
    double total = 0.0;
    for (double e : m.bandEnergy)
        total += e;
    for (std::size_t b = 0; b < kPrintBands; ++b)
        p.shapeDb[b] = wholeDb (m.bandEnergy[b] / total, kPrintFloorDb, 0);
    return p;
}

} // namespace printdetail

/** Clears the filters and the block in progress. */
inline void reset (PrintMeter& m) noexcept
{
    m.state = {};
    m.bandEnergy = {};
    m.energy = 0.0;
    m.count = 0;
    m.last = {};
}

/** Starts a new block here, keeping the filters: the layer lines blocks up with the cache's. */
inline void restartBlock (PrintMeter& m) noexcept
{
    m.bandEnergy = {};
    m.energy = 0.0;
    m.count = 0;
}

inline void prepare (PrintMeter& m, double sampleRate) noexcept
{
    m.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    for (std::size_t i = 0; i < m.lp.size(); ++i)
        m.lp[i] = makeSvfCoeffs (SvfType::LowPass, kPrintSplitHz[i], 0.70710678f, 0.0f, m.sampleRate);
    reset (m);
}

/** One sample of the mono sum. Returns true when it completes a block (see lastPrint). */
inline bool push (PrintMeter& m, float mono) noexcept
{
    // A non-finite sample would poison the filters for good: it counts as silence.
    const float x = std::isfinite (mono) ? mono : 0.0f;
    float below = 0.0f;
    for (std::size_t i = 0; i < m.lp.size(); ++i)
    {
        const float lowPassed = processSample (m.state[i], m.lp[i], x);
        const float band = lowPassed - below;
        m.bandEnergy[i] += static_cast<double> (band * band);
        below = lowPassed;
    }
    const float top = x - below;
    m.bandEnergy[kPrintBands - 1] += static_cast<double> (top * top);
    m.energy += static_cast<double> (x * x);

    if (++m.count < kPrintBlock)
        return false;
    m.last = printdetail::makePrint (m);
    restartBlock (m);
    for (auto& s : m.state)
        flushDenormals (s);
    return true;
}

inline BlockPrint lastPrint (const PrintMeter& m) noexcept { return m.last; }

inline bool isSilent (const BlockPrint& p) noexcept { return p.levelDb < kPrintSilentDb; }

/** Same audio, at any level? Silence matches silence only. */
inline bool matches (const BlockPrint& a, const BlockPrint& b) noexcept
{
    if (isSilent (a) || isSilent (b))
        return isSilent (a) && isSilent (b);
    for (std::size_t i = 0; i < kPrintBands; ++i)
        if (std::abs (a.shapeDb[i] - b.shapeDb[i]) > kPrintToleranceDb)
            return false;
    return true;
}

/** The prints of a whole stretch, one per complete block of 512 samples. Allocates. */
inline std::vector<BlockPrint> fingerprints (const float* l, const float* r, int n, double sampleRate)
{
    PrintMeter m;
    prepare (m, sampleRate);
    std::vector<BlockPrint> out;
    out.reserve (static_cast<std::size_t> (std::max (0, n / kPrintBlock)));
    for (int i = 0; i < n; ++i)
        if (push (m, 0.5f * (l[i] + r[i])))
            out.push_back (lastPrint (m));
    return out;
}

} // namespace strat3::dsp
