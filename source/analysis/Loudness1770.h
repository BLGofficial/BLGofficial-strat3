#pragma once
// Loudness metering, ITU-R BS.1770-4 (EBU R128 windows).
//
// Audio thread (LoudnessTap): K-weighting per channel (high shelf, then RLB high-pass), squared
// and summed over both channels (G = 1 each) into 100 ms blocks of round(0.1 fs) samples. Each
// block's mean square goes into a lock-free SPSC ring. A full ring drops the block and counts
// it: the audio thread never waits on the meter.
// Analysis thread (LoudnessMeter): momentary = last 4 blocks, short-term = last 30 (missing
// blocks count as silence, as for a meter started on digital silence); integrated = 400 ms
// gating blocks at 75% overlap (one per 100 ms block once 4 exist), absolute gate -70 LUFS,
// relative gate -10 LU below the absolute-gated mean.
// Integrated keeps a histogram (0.01 LU bins from -70 LUFS up, each bin holding a count and the
// summed energy) instead of every block: fixed memory for any programme length. Energies are
// summed exactly; only the bin that straddles the relative gate is included or excluded as a
// whole (by its mean), which moves the result by far less than 0.01 LU.
//
// K-weighting is designed per sample rate from the analog prototype of the 48 kHz tables
// (shelf f0 1681.97 Hz, +4 dB, Q 0.7072; high-pass f0 38.14 Hz, Q 0.5003) [INFERRED: prototype
// values fitted to the BS.1770 tables, checked against them in the tests].
//
// prepare() and reset() of a tap must not run while a consumer is draining it.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
#include <numbers>
#include <vector>

namespace strat3::analysis {

inline constexpr double kLoudnessOffsetDb = -0.691;
inline constexpr double kAbsoluteGateLufs = -70.0;
inline constexpr double kRelativeGateLu = -10.0;
inline constexpr int kMomentaryBlocks = 4;
inline constexpr int kShortTermBlocks = 30;
inline constexpr int kLoudnessRingSize = 64;   // 6.4 s of blocks: the analysis thread drains at 30 Hz
inline constexpr double kHistogramStepLu = 0.01;
inline constexpr int kHistogramBins = 8000;    // -70 .. +10 LUFS; louder blocks share the top bin

// ------------------------------------------------------------------- K-weighting

struct Biquad
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
};

struct KWeighting
{
    Biquad shelf {}, highpass {};
};

/** Non-RT. */
inline KWeighting designKWeighting (double sampleRate)
{
    const double fs = sampleRate > 0.0 ? sampleRate : 48000.0;
    KWeighting k;
    {
        constexpr double f0 = 1681.974450955533, gainDb = 3.999843853973347, q = 0.7071752369554196;
        const double kk = std::tan (std::numbers::pi * f0 / fs);
        const double vh = std::pow (10.0, gainDb / 20.0);
        const double vb = std::pow (vh, 0.4996667741545416);
        const double a0 = 1.0 + kk / q + kk * kk;
        k.shelf.b0 = (vh + vb * kk / q + kk * kk) / a0;
        k.shelf.b1 = 2.0 * (kk * kk - vh) / a0;
        k.shelf.b2 = (vh - vb * kk / q + kk * kk) / a0;
        k.shelf.a1 = 2.0 * (kk * kk - 1.0) / a0;
        k.shelf.a2 = (1.0 - kk / q + kk * kk) / a0;
    }
    {
        constexpr double f0 = 38.13547087602444, q = 0.5003270373238773;
        auto a0Of = [&] (double rate) { const double kk = std::tan (std::numbers::pi * f0 / rate); return 1.0 + kk / q + kk * kk; };
        const double kk = std::tan (std::numbers::pi * f0 / fs);
        const double a0 = a0Of (fs);
        // The BS.1770 table leaves the numerator at 1, -2, 1: the normalised high-pass times
        // a0(48 kHz), a +0.043 dB passband gain the -0.691 offset was calibrated with. Keep that
        // same gain at every rate, or the reading would shift with the sample rate.
        const double g = a0Of (48000.0) / a0;
        k.highpass = { g, -2.0 * g, g, 2.0 * (kk * kk - 1.0) / a0, (1.0 - kk / q + kk * kk) / a0 };
    }
    return k;
}

/** Non-RT: magnitude of the cascade in dB. */
inline double kWeightingGainDb (const KWeighting& k, double hz, double sampleRate)
{
    const std::complex<double> z1 = std::polar (1.0, -2.0 * std::numbers::pi * hz / sampleRate);
    auto h = [&] (const Biquad& c) { return (c.b0 + c.b1 * z1 + c.b2 * z1 * z1) / (1.0 + c.a1 * z1 + c.a2 * z1 * z1); };
    return 20.0 * std::log10 (std::abs (h (k.shelf) * h (k.highpass)));
}

// ------------------------------------------------------------------- SPSC ring

struct LoudnessRing
{
    static_assert ((kLoudnessRingSize & (kLoudnessRingSize - 1)) == 0, "power of two");
    std::array<double, kLoudnessRingSize> buf {};
    std::atomic<std::uint32_t> head { 0 }, tail { 0 };   // free-running; unsigned wrap is fine
};

/** Producer only. False if full. */
inline bool push (LoudnessRing& r, double v) noexcept
{
    const auto h = r.head.load (std::memory_order_relaxed);
    if (h - r.tail.load (std::memory_order_acquire) >= static_cast<std::uint32_t> (kLoudnessRingSize))
        return false;
    r.buf[h & (kLoudnessRingSize - 1)] = v;
    r.head.store (h + 1, std::memory_order_release);
    return true;
}

/** Consumer only. False if empty. */
inline bool pop (LoudnessRing& r, double& v) noexcept
{
    const auto t = r.tail.load (std::memory_order_relaxed);
    if (t == r.head.load (std::memory_order_acquire))
        return false;
    v = r.buf[t & (kLoudnessRingSize - 1)];
    r.tail.store (t + 1, std::memory_order_release);
    return true;
}

// ------------------------------------------------------------------- audio-thread tap

struct LoudnessTap
{
    KWeighting k {};
    std::array<std::array<double, 4>, 2> z {};   // per channel: shelf z1 z2, high-pass z1 z2
    double acc = 0.0;
    int count = 0;
    int blockLen = 4800;
    LoudnessRing ring {};
    std::atomic<std::uint32_t> dropped { 0 };
};

inline int blockLength (const LoudnessTap& t) noexcept { return t.blockLen; }
inline std::uint32_t droppedBlocks (const LoudnessTap& t) noexcept { return t.dropped.load (std::memory_order_relaxed); }

/** Consumer side of the tap's ring. */
inline bool pop (LoudnessTap& t, double& meanSquare) noexcept { return pop (t.ring, meanSquare); }

namespace loudetail {

inline double biquad (const Biquad& c, double x, double& z1, double& z2) noexcept
{
    const double y = c.b0 * x + z1;
    z1 = c.b1 * x - c.a1 * y + z2;
    z2 = c.b2 * x - c.a2 * y;
    return y;
}

inline void resetFilters (LoudnessTap& t) noexcept
{
    for (auto& ch : t.z)
        ch = {};
    t.acc = 0.0;
    t.count = 0;
}

} // namespace loudetail

/** prepareToPlay. Clears the ring: the consumer must not be draining. */
inline void prepare (LoudnessTap& t, double sampleRate)
{
    const double fs = sampleRate > 0.0 ? sampleRate : 48000.0;
    t.k = designKWeighting (fs);
    t.blockLen = std::max (1, static_cast<int> (std::lround (0.1 * fs)));
    loudetail::resetFilters (t);
    t.ring.head.store (0);
    t.ring.tail.store (0);
    t.dropped.store (0);
}

/** Audio thread, per sample. */
inline void push (LoudnessTap& t, float l, float r) noexcept
{
    const std::array<double, 2> in { static_cast<double> (l), static_cast<double> (r) };
    for (std::size_t c = 0; c < 2; ++c)
    {
        auto& z = t.z[c];
        const double y = loudetail::biquad (t.k.highpass, loudetail::biquad (t.k.shelf, in[c], z[0], z[1]), z[2], z[3]);
        t.acc += y * y;
    }
    if (++t.count == t.blockLen)
    {
        if (! push (t.ring, t.acc / static_cast<double> (t.blockLen)))
            t.dropped.fetch_add (1, std::memory_order_relaxed);
        t.acc = 0.0;
        t.count = 0;
    }
}

template <typename Fn>
void forEachState (LoudnessTap& t, Fn&& fn)
{
    for (auto& ch : t.z)
        for (auto& v : ch)
            fn (v);
    fn (t.acc);
}

/** Returns true if the filters had to be reset (raise the UI flag; never log here). */
inline bool resetIfNotFinite (LoudnessTap& t) noexcept
{
    bool bad = false;
    forEachState (t, [&] (double v) { bad |= ! std::isfinite (v); });
    if (bad)
        loudetail::resetFilters (t);
    return bad;
}

inline void flushDenormals (LoudnessTap& t) noexcept
{
    for (auto& ch : t.z)
        for (auto& v : ch)
            if (std::abs (v) < 1e-30)   // ~ -600 dB: minutes of silence must not reach subnormals
                v = 0.0;
}

// ------------------------------------------------------------------- analysis-thread meter

struct LoudnessMeter
{
    std::array<double, kShortTermBlocks> recent {};   // block energies, ring
    int pos = 0;
    std::int64_t received = 0;
    std::vector<std::uint32_t> count {};
    std::vector<double> energy {};
};

inline float energyToLufs (double meanSquare) noexcept
{
    if (! (meanSquare > 0.0))
        return -std::numeric_limits<float>::infinity();
    return static_cast<float> (kLoudnessOffsetDb + 10.0 * std::log10 (meanSquare));
}

inline void reset (LoudnessMeter& m) noexcept
{
    m.recent = {};
    m.pos = 0;
    m.received = 0;
    std::fill (m.count.begin(), m.count.end(), 0u);
    std::fill (m.energy.begin(), m.energy.end(), 0.0);
}

/** Analysis thread setup (allocates the histogram). */
inline void prepare (LoudnessMeter& m)
{
    m.count.assign (kHistogramBins, 0u);
    m.energy.assign (kHistogramBins, 0.0);
    reset (m);
}

namespace loudetail {

inline double recentMean (const LoudnessMeter& m, int blocks) noexcept
{
    double s = 0.0;
    for (int i = 1; i <= blocks; ++i)
        s += m.recent[static_cast<std::size_t> ((m.pos - i + kShortTermBlocks) % kShortTermBlocks)];
    return s / blocks;
}

inline int binOf (double lufs) noexcept
{
    return std::clamp (static_cast<int> (std::floor ((lufs - kAbsoluteGateLufs) / kHistogramStepLu)), 0, kHistogramBins - 1);
}

} // namespace loudetail

/** One 100 ms block's channel-summed mean square. Non-finite or negative blocks are ignored. */
inline void addBlock (LoudnessMeter& m, double meanSquare) noexcept
{
    if (! std::isfinite (meanSquare) || meanSquare < 0.0)
        return;
    m.recent[static_cast<std::size_t> (m.pos)] = meanSquare;
    m.pos = (m.pos + 1) % kShortTermBlocks;
    ++m.received;
    if (m.received < kMomentaryBlocks || m.count.empty())
        return;
    const double g = loudetail::recentMean (m, kMomentaryBlocks);
    if (! (g > 0.0))
        return;
    const double lufs = kLoudnessOffsetDb + 10.0 * std::log10 (g);
    if (lufs <= kAbsoluteGateLufs)
        return;
    const auto b = static_cast<std::size_t> (loudetail::binOf (lufs));
    ++m.count[b];
    m.energy[b] += g;
}

/** Drains the tap's ring into the meter. Returns the number of blocks taken. */
inline int drain (LoudnessMeter& m, LoudnessTap& t) noexcept
{
    int n = 0;
    double e = 0.0;
    while (pop (t, e))
    {
        addBlock (m, e);
        ++n;
    }
    return n;
}

inline float momentaryLufs (const LoudnessMeter& m) noexcept { return energyToLufs (loudetail::recentMean (m, kMomentaryBlocks)); }
inline float shortTermLufs (const LoudnessMeter& m) noexcept { return energyToLufs (loudetail::recentMean (m, kShortTermBlocks)); }

/** O(bins): call at UI rate, not per block. */
inline float integratedLufs (const LoudnessMeter& m) noexcept
{
    double e = 0.0;
    std::uint64_t n = 0;
    for (std::size_t b = 0; b < m.count.size(); ++b)
    {
        e += m.energy[b];
        n += m.count[b];
    }
    if (n == 0)
        return -std::numeric_limits<float>::infinity();
    const double gate = kLoudnessOffsetDb + 10.0 * std::log10 (e / static_cast<double> (n)) + kRelativeGateLu;
    const int first = gate < kAbsoluteGateLufs ? 0 : loudetail::binOf (gate);
    e = 0.0;
    n = 0;
    for (auto b = static_cast<std::size_t> (first); b < m.count.size(); ++b)
    {
        if (m.count[b] == 0)
            continue;
        // The straddling bin goes in or out as a whole, decided by its own mean loudness.
        if (static_cast<int> (b) == first
            && kLoudnessOffsetDb + 10.0 * std::log10 (m.energy[b] / m.count[b]) <= gate)
            continue;
        e += m.energy[b];
        n += m.count[b];
    }
    return n == 0 ? -std::numeric_limits<float>::infinity() : energyToLufs (e / static_cast<double> (n));
}

// ------------------------------------------------------------------- delivery maths

/** True peak (dBTP) minus integrated loudness. NaN while integrated is empty. */
inline float peakToLoudnessRatio (float truePeakDb, float integrated) noexcept
{
    if (! std::isfinite (integrated) || ! std::isfinite (truePeakDb))
        return std::numeric_limits<float>::quiet_NaN();
    return truePeakDb - integrated;
}

enum class Platform { Spotify, AppleMusic, YouTube };

/** Normalisation reference levels, LUFS-I [INFERRED: publicly stated platform figures; they
 *  change, so they live in this one table]. */
inline constexpr std::array<float, 3> kPlatformReferenceLufs { -14.0f, -16.0f, -14.0f };

/** dB the platform turns the track down (<= 0). Turn-up is not shown: platforms limit it by
 *  peak headroom, which this figure cannot know. */
inline float platformPenaltyDb (Platform p, float integrated) noexcept
{
    if (! std::isfinite (integrated))
        return 0.0f;
    return std::min (0.0f, kPlatformReferenceLufs[static_cast<std::size_t> (p)] - integrated);
}

} // namespace strat3::analysis
