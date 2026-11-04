#pragma once
// Assistant v1: a rule engine on the analysis thread (no model). Rules and constants are own
// design and tuning [INFERRED], each kept in one place so they can be retuned.
//
// Input: a Snapshot of a 30/60 s capture, per 1/6-octave band (the impact-map bands):
//   levelDb      mean band energy, mid + side
//   spreadDb     dynamics spread: P95 / P50 of ~100 ms block energies (0 dB = steady)
//   sideToMidDb  mean side energy / mean mid energy
// plus LUFS-I, true peak and correlation. A TargetCurve is captured from the user's own
// reference tracks (no built-in genre data).
//
// Rules, in chain order:
//   TONAL     level-independent shape against the target (both normalised by their median
//             over 100 Hz - 10 kHz, smoothed over 1/2 octave). Each contiguous region beyond
//             +-2 dB is one EQ move on a free band: bell at the region's weighted centre, Q
//             from its width, 70% of the deviation, capped at +-6 dB. A cut on a band whose
//             spread is >= 6 dB becomes a dynamic dip: the excess comes and goes, so a static
//             cut would also thin the quiet parts. (The Phase 5 example tied dynamic EQ to a
//             crest UNDER 6 dB; that reads inverted, so this follows the reasoning instead.)
//   LOW MONO  side/mid below 120 Hz above -10 dB: mono bass below 120 Hz.
//   LOUDNESS  LUFS-I more than 0.5 LU from the platform target: move the limiter threshold by
//             the difference (threshold = makeup, so this is an open-loop estimate).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "analysis/ImpactAnalyser.h"   // band layout
#include "assistant/Move.h"

namespace strat3::assistant {

inline constexpr int kBands = analysis::kImpactBands;
inline constexpr int kEqBands = 8;

inline constexpr float kTonalThresholdDb = 2.0f;
inline constexpr float kTonalCorrection = 0.7f;   // fraction of the deviation corrected
inline constexpr float kMaxTonalGainDb = 6.0f;
inline constexpr int kMaxTonalMoves = 3;
inline constexpr double kTonalLowHz = 40.0, kTonalHighHz = 16000.0;
inline constexpr double kNormLowHz = 100.0, kNormHighHz = 10000.0;
inline constexpr float kDynamicSpreadDb = 6.0f;

inline constexpr float kLowMonoHz = 120.0f;
inline constexpr float kLowSideMaxDb = -10.0f;

inline constexpr float kLoudnessToleranceLu = 0.5f;
inline constexpr std::array<float, 4> kTargetLufs { -14.0f, -11.0f, -9.0f, -8.0f };   // global.target 1..4

inline constexpr double kSnapshotFrameHz = 30.0;
inline constexpr int kFramesPerBlock = 3;   // ~100 ms blocks for the spread

using BandEnergies = std::array<double, kBands>;
using ParamValues = std::map<std::string, float>;

struct Snapshot
{
    std::array<float, kBands> levelDb {}, spreadDb {}, sideToMidDb {};
    float integratedLufs = 0.0f, truePeakDb = 0.0f, correlation = 1.0f;
    double seconds = 0.0;
};

struct TargetCurve
{
    std::array<float, kBands> levelDb {};
};

// ------------------------------------------------------------------- snapshot builder

struct SnapshotBuilder
{
    std::vector<BandEnergies> mid {}, side {};
    std::size_t maxFrames = 0;
};

/** Message/analysis thread: allocates for `seconds` of frames at 30 Hz. */
inline void begin (SnapshotBuilder& b, double seconds)
{
    b.maxFrames = static_cast<std::size_t> (std::max (0.0, std::round (seconds * kSnapshotFrameHz)));
    b.mid.clear();
    b.side.clear();
    b.mid.reserve (b.maxFrames);
    b.side.reserve (b.maxFrames);
}

/** One analysis frame of band energies. Frames past the capture length or with any
 *  non-finite or negative value are ignored. */
inline void addFrame (SnapshotBuilder& b, const BandEnergies& mid, const BandEnergies& side)
{
    if (b.mid.size() >= b.maxFrames)
        return;
    for (std::size_t i = 0; i < mid.size(); ++i)
        if (! std::isfinite (mid[i]) || ! std::isfinite (side[i]) || mid[i] < 0.0 || side[i] < 0.0)
            return;
    b.mid.push_back (mid);
    b.side.push_back (side);
}

namespace ruledetail {

inline float toDb (double e) noexcept { return static_cast<float> (10.0 * std::log10 (std::max (e, 1e-20))); }

/** Median over the normalisation range, finite bands only. NaN if there are none. */
inline float normMedian (const std::array<float, kBands>& v)
{
    std::vector<float> x;
    for (int b = 0; b < kBands; ++b)
    {
        const double hz = analysis::impactBandCentreHz (b);
        if (hz >= kNormLowHz && hz <= kNormHighHz && std::isfinite (v[static_cast<std::size_t> (b)]))
            x.push_back (v[static_cast<std::size_t> (b)]);
    }
    if (x.empty())
        return std::numeric_limits<float>::quiet_NaN();
    std::sort (x.begin(), x.end());
    const auto n = x.size();
    return n % 2 == 1 ? x[n / 2] : 0.5f * (x[n / 2 - 1] + x[n / 2]);
}

inline float get (const ParamValues& p, const std::string& id, float fallback)
{
    const auto it = p.find (id);
    return it != p.end() && std::isfinite (it->second) ? it->second : fallback;
}

inline std::string format (const char* fmt, double a, double b = 0.0, double c = 0.0, double d = 0.0)
{
    char buf[160];
    std::snprintf (buf, sizeof (buf), fmt, a, b, c, d);
    return buf;
}

/** Adds an unscaled change only when it changes something; scaled changes always. */
inline void change (Move& m, const ParamValues& p, const std::string& id, float fallback, float to, bool scaled)
{
    const float from = get (p, id, fallback);
    if (! scaled && from == to)
        return;
    m.changes.push_back ({ id, from, to, scaled });
}

} // namespace ruledetail

/** Message/analysis thread: summarises the capture. */
inline Snapshot finish (const SnapshotBuilder& b, float integratedLufs, float truePeakDb, float correlation)
{
    Snapshot s;
    s.integratedLufs = integratedLufs;
    s.truePeakDb = truePeakDb;
    s.correlation = correlation;
    const auto frames = b.mid.size();
    s.seconds = static_cast<double> (frames) / kSnapshotFrameHz;
    const std::size_t blocks = frames / kFramesPerBlock;
    std::vector<double> blockE (blocks);
    for (std::size_t band = 0; band < static_cast<std::size_t> (kBands); ++band)
    {
        double m = 0.0, sd = 0.0;
        for (std::size_t f = 0; f < frames; ++f)
        {
            m += b.mid[f][band];
            sd += b.side[f][band];
        }
        const double n = std::max<double> (1.0, static_cast<double> (frames));
        s.levelDb[band] = ruledetail::toDb ((m + sd) / n);
        s.sideToMidDb[band] = ruledetail::toDb (sd / n) - ruledetail::toDb (m / n);

        for (std::size_t k = 0; k < blocks; ++k)
        {
            double e = 0.0;
            for (int j = 0; j < kFramesPerBlock; ++j)
            {
                const auto f = k * kFramesPerBlock + static_cast<std::size_t> (j);
                e += b.mid[f][band] + b.side[f][band];
            }
            blockE[k] = e / kFramesPerBlock;
        }
        if (blocks == 0)
        {
            s.spreadDb[band] = 0.0f;
            continue;
        }
        std::sort (blockE.begin(), blockE.end());
        auto rank = [&] (double q) { return blockE[static_cast<std::size_t> (std::max (0.0, std::ceil (q * static_cast<double> (blocks)) - 1.0))]; };
        s.spreadDb[band] = ruledetail::toDb (rank (0.95)) - ruledetail::toDb (rank (0.5));
    }
    return s;
}

/** Average shape of the user's references, each normalised first (levels do not matter). */
inline TargetCurve captureTarget (std::span<const Snapshot> refs)
{
    TargetCurve t;
    std::array<int, kBands> count {};
    for (const auto& r : refs)
    {
        const float med = ruledetail::normMedian (r.levelDb);
        if (! std::isfinite (med))
            continue;
        for (std::size_t b = 0; b < static_cast<std::size_t> (kBands); ++b)
            if (std::isfinite (r.levelDb[b]))
            {
                t.levelDb[b] += r.levelDb[b] - med;
                ++count[b];
            }
    }
    for (std::size_t b = 0; b < static_cast<std::size_t> (kBands); ++b)
        t.levelDb[b] = count[b] > 0 ? t.levelDb[b] / static_cast<float> (count[b]) : 0.0f;
    return t;
}

// ------------------------------------------------------------------- rules

namespace ruledetail {

inline void tonal (const Snapshot& s, const TargetCurve& target, const ParamValues& p, std::vector<Move>& out, int& nextId)
{
    const float mixMed = normMedian (s.levelDb), tgtMed = normMedian (target.levelDb);
    if (! std::isfinite (mixMed) || ! std::isfinite (tgtMed))
        return;
    std::array<float, kBands> d {}, sd {};
    for (std::size_t b = 0; b < static_cast<std::size_t> (kBands); ++b)
    {
        const float v = (s.levelDb[b] - mixMed) - (target.levelDb[b] - tgtMed);
        d[b] = std::isfinite (v) ? v : 0.0f;
    }
    for (int b = 0; b < kBands; ++b)   // +-1 band: a 1/2-octave moving average
    {
        float sum = 0.0f;
        int n = 0;
        for (int k = std::max (0, b - 1); k <= std::min (kBands - 1, b + 1); ++k, ++n)
            sum += d[static_cast<std::size_t> (k)];
        sd[static_cast<std::size_t> (b)] = sum / static_cast<float> (n);
    }

    struct Region { int lo, hi, peakBand; float peak; double centre; };
    std::vector<Region> regions;
    for (int b = 0; b < kBands;)
    {
        const double hz = analysis::impactBandCentreHz (b);
        const float v = sd[static_cast<std::size_t> (b)];
        if (hz < kTonalLowHz || hz > kTonalHighHz || std::abs (v) < kTonalThresholdDb)
        {
            ++b;
            continue;
        }
        Region r { b, b, b, v, 0.0 };
        double wsum = 0.0, ksum = 0.0;
        int k = b;
        for (; k < kBands && analysis::impactBandCentreHz (k) <= kTonalHighHz; ++k)
        {
            const float x = sd[static_cast<std::size_t> (k)];
            if (std::abs (x) < kTonalThresholdDb || (x > 0.0f) != (v > 0.0f))
                break;
            r.hi = k;
            if (std::abs (x) > std::abs (r.peak))
            {
                r.peak = x;
                r.peakBand = k;
            }
            wsum += static_cast<double> (std::abs (x));
            ksum += static_cast<double> (std::abs (x)) * k;
        }
        r.centre = ksum / wsum;
        regions.push_back (r);
        b = k;
    }
    std::stable_sort (regions.begin(), regions.end(), [] (const Region& a, const Region& b) { return std::abs (a.peak) > std::abs (b.peak); });

    int used = 0, eqBand = 1;
    for (const auto& r : regions)
    {
        if (used == kMaxTonalMoves)
            break;
        while (eqBand <= kEqBands && get (p, "eq.b" + std::to_string (eqBand) + ".on", 0.0f) >= 0.5f)
            ++eqBand;
        if (eqBand > kEqBands)
            break;
        const std::string pre = "eq.b" + std::to_string (eqBand) + ".";
        const auto hz = static_cast<float> (1000.0 * std::pow (2.0, (r.centre - 34.0) / 6.0));
        const double octaves = static_cast<double> (r.hi - r.lo + 1) / 6.0;
        const auto q = static_cast<float> (std::clamp (std::sqrt (std::pow (2.0, octaves)) / (std::pow (2.0, octaves) - 1.0), 0.5, 4.0));
        const float gain = std::clamp (-kTonalCorrection * r.peak, -kMaxTonalGainDb, kMaxTonalGainDb);
        const float spread = s.spreadDb[static_cast<std::size_t> (r.peakBand)];
        const bool dynamic = r.peak > 0.0f && std::isfinite (spread) && spread >= kDynamicSpreadDb;

        Move m;
        m.id = nextId++;
        m.layer = "eq";
        change (m, p, pre + "on", 0.0f, 1.0f, false);
        change (m, p, pre + "type", 0.0f, 0.0f, false);   // Bell
        change (m, p, pre + "ms", 0.0f, 0.0f, false);     // Stereo
        change (m, p, pre + "freq", 1000.0f, hz, false);
        change (m, p, pre + "q", 0.707f, q, false);
        if (dynamic)
        {
            const float thresh = std::clamp (s.levelDb[static_cast<std::size_t> (r.peakBand)], -60.0f, 0.0f);
            change (m, p, pre + "dyn", 0.0f, 2.0f, false);   // Down
            change (m, p, pre + "thresh", -20.0f, thresh, false);
            change (m, p, pre + "range", -3.0f, gain, true);
            m.reason = format ("dynamic dip %.1f dB @ %.0f Hz: %.1f dB above target, spread %.1f dB", gain, hz, r.peak, spread);
        }
        else
        {
            change (m, p, pre + "dyn", 0.0f, 0.0f, false);
            change (m, p, pre + "gain", 0.0f, gain, true);
            m.reason = format ("%+.1f dB @ %.0f Hz: ", gain, hz) + format ("%.1f dB ", std::abs (r.peak)) + (r.peak > 0.0f ? "above target" : "below target");
        }
        m.metric = "deviation from target (dB)";
        m.metricBefore = r.peak;
        m.metricTarget = 0.0f;
        out.push_back (std::move (m));
        ++used;
        ++eqBand;
    }
}

inline void lowMono (const Snapshot& s, const ParamValues& p, std::vector<Move>& out, int& nextId)
{
    if (get (p, "low.monoOn", 0.0f) >= 0.5f)
        return;
    double sideE = 0.0, midE = 0.0;
    for (int b = 0; b < kBands && analysis::impactBandCentreHz (b) < static_cast<double> (kLowMonoHz); ++b)
    {
        const float level = s.levelDb[static_cast<std::size_t> (b)], ratio = s.sideToMidDb[static_cast<std::size_t> (b)];
        if (! std::isfinite (level) || ! std::isfinite (ratio))
            continue;
        const double total = std::pow (10.0, static_cast<double> (level) / 10.0), r = std::pow (10.0, static_cast<double> (ratio) / 10.0);
        midE += total / (1.0 + r);
        sideE += total * r / (1.0 + r);
    }
    if (midE <= 0.0)
        return;
    const auto ratioDb = static_cast<float> (10.0 * std::log10 (sideE / midE));
    if (! (ratioDb > kLowSideMaxDb))
        return;
    Move m;
    m.id = nextId++;
    m.layer = "low";
    change (m, p, "low.on", 0.0f, 1.0f, false);
    change (m, p, "low.monoOn", 0.0f, 1.0f, false);
    change (m, p, "low.monoFreq", 80.0f, kLowMonoHz, false);
    m.reason = format ("mono bass below %.0f Hz: side %.1f dB vs %.0f dB max", kLowMonoHz, ratioDb, kLowSideMaxDb);
    m.metric = "side/mid below 120 Hz (dB)";
    m.metricBefore = ratioDb;
    m.metricTarget = kLowSideMaxDb;
    out.push_back (std::move (m));
}

inline void loudness (const Snapshot& s, const ParamValues& p, std::vector<Move>& out, int& nextId)
{
    const int idx = static_cast<int> (std::lround (get (p, "global.target", 0.0f)));
    if (idx < 1 || idx > static_cast<int> (kTargetLufs.size()) || ! std::isfinite (s.integratedLufs))
        return;
    const float target = kTargetLufs[static_cast<std::size_t> (idx - 1)];
    const float delta = target - s.integratedLufs;
    if (std::abs (delta) <= kLoudnessToleranceLu)
        return;
    const float from = get (p, "limit.thresh", 0.0f);
    const float to = std::clamp (from - delta, -24.0f, 0.0f);
    if (to == from)
        return;
    Move m;
    m.id = nextId++;
    m.layer = "limit";
    change (m, p, "limit.on", 0.0f, 1.0f, false);
    change (m, p, "limit.thresh", 0.0f, to, true);
    m.reason = format ("limiter threshold %.1f dB: %.1f LUFS-I vs %.0f target (open-loop estimate)", to, s.integratedLufs, target);
    m.metric = "integrated LUFS";
    m.metricBefore = s.integratedLufs;
    m.metricTarget = target;
    out.push_back (std::move (m));
}

} // namespace ruledetail

/** All proposals for one capture, in chain order (EQ, LOW, LIMIT). `target` may be null:
 *  no target, no tonal moves. Deterministic: the same inputs give the same moves. */
inline std::vector<Move> propose (const Snapshot& s, const TargetCurve* target, const ParamValues& p)
{
    std::vector<Move> moves;
    int id = 1;
    if (target != nullptr)
        ruledetail::tonal (s, *target, p, moves, id);
    ruledetail::lowMono (s, p, moves, id);
    ruledetail::loudness (s, p, moves, id);
    return moves;
}

} // namespace strat3::assistant
