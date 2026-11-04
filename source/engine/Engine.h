#pragma once
// The fixed-order chain that wires every layer (headless: no AudioProcessor types).
//
//   IN TRIM -> EQ -> LOW -> [MB -> WIDTH -> SAT, one shared band split] -> LIMIT -> OUT TRIM
//   -> monitor (normal | delta) -> CEILING -> gain match (monitor only)
//
// Depth: each layer has its own DepthBlend. LOW, SAT and LIMIT return a reference (their
// all-pass / oversampler / look-ahead path, unprocessed) and blend against it; EQ blends
// against its input delayed by its own latency (FIR in MASTER); MB and WIDTH have none.
// Latency is fixed per mode: a layer that is off keeps its latency, so the host's delay
// compensation never moves when the user toggles something.
//
// Delta [INFERRED, own design]: each layer's addition (blend out - blend dry) is delayed to
// the monitor point. The topmost layer with delta on is heard alone (the layers below it
// keep running but are not heard). Global delta is the sum of every layer's addition, so a
// neutral chain nulls even though its IIR crossovers rotate phase. Trims are not layers.
// Both delta signals still pass the ceiling (ear protection).
//
// Gain match [INFERRED]: the analysis thread measures input vs output loudness and sets an
// offset; it is applied after the ceiling, so it is monitor-only and never printed. It is
// muted (0 dB) while any delta is heard, or a quiet delta would be boosted by up to +24 dB.
//
// Threads: prepare() on the message thread with the audio stopped; setParams()/process()
// on the audio thread; requestDesigns() on one background thread (MASTER FIR redesigns);
// setGainMatchDb() and takeNonFiniteFlag() from any thread.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>

#include "analysis/ImpactAnalyser.h"
#include "analysis/Loudness1770.h"
#include "dsp/core/DelayLine.h"
#include "dsp/core/Smoother.h"
#include "dsp/crossover/BandDomain.h"
#include "dsp/crossover/CrossoverPoints.h"
#include "dsp/eq/EqFir.h"
#include "dsp/eq/EqLayer.h"
#include "dsp/layers/CeilingLayer.h"
#include "dsp/layers/DepthBlend.h"
#include "dsp/layers/LimitLayer.h"
#include "dsp/layers/LowLayer.h"
#include "dsp/layers/MultibandLayer.h"
#include "dsp/layers/SaturateLayer.h"
#include "dsp/layers/WidthLayer.h"

namespace strat3::engine {

enum class LatencyMode { Track, Mix, Master };   // global.latencyMode order: append only

/** Layers with a depth handle, in chain order. */
enum DepthLayer { kEq, kLow, kMb, kWidth, kSat, kLimit, kDepthLayers };

/** Impact-map rows (one per stage, trims included); tap r is a row's input, r + 1 its output. */
enum Row { kRowInTrim, kRowEq, kRowLow, kRowMb, kRowWidth, kRowSat, kRowLimit, kRowOutTrim, kRowCeiling, kRows };

inline constexpr float kTrimRampMs = 20.0f;          // Phase 5: trims smoothed over 20 ms
inline constexpr float kMonitorRampMs = 10.0f;       // delta switching
inline constexpr float kCeilingSwitchMs = 10.0f;     // ceiling.on
inline constexpr float kGainMatchRampMs = 50.0f;     // [INFERRED] slow enough to never pump
inline constexpr float kGainMatchMaxDb = 24.0f;
inline constexpr double kMaxOversampledRate = 192000.0 * 1.0001;   // [INFERRED] CPU cap for sat.os

struct LayerSwitch
{
    bool on = true;
    float depth = 100.0f;   // 0..150 %
    bool delta = false;
    bool operator== (const LayerSwitch&) const = default;
};

/** Everything the audio thread needs, already converted from parameter values. Plain data:
 *  copying it never allocates. */
struct EngineParams
{
    float inTrimDb = 0.0f, outTrimDb = 0.0f;
    bool globalDelta = false;
    bool gainMatch = false;
    std::array<LayerSwitch, kDepthLayers> layer {};
    bool ceilingOn = true;

    dsp::EqSettings eq {};
    float eqPhase = 100.0f;   // MASTER only
    dsp::LowSettings low {};
    dsp::CrossoverPoints xover = dsp::kDefaultCrossovers;
    dsp::MbSettings mb {};
    dsp::WidthSettings width {};
    dsp::SatSettings sat {};
    dsp::LimitSettings limit {};       // ceilingDb is ignored: it follows ceiling.ceilingDb
    dsp::CeilingSettings ceiling {};
    bool operator== (const EngineParams&) const = default;
};

struct EngineConfig
{
    double sampleRate = 48000.0;
    int maxBlock = 512;
    LatencyMode mode = LatencyMode::Mix;
    int satOs = 2;   // requested factor (1, 2, 4); capped by effectiveSatOs
};

/** Not copyable or movable (atomics in the FIR swaps and taps): construct in place. */
struct Engine
{
    EngineConfig config {};
    EngineParams params {};

    // ---- layers
    dsp::EqLayer eqIir {};
    dsp::FirEq eqFir {};
    dsp::LowLayer low {};
    dsp::BandDomain domain {};
    dsp::SidechainSplit sidechain {};
    dsp::CrossoverPoints sidechainPoints = dsp::kDefaultCrossovers;
    dsp::MultibandLayer mb {};
    dsp::WidthLayer width {};
    dsp::SaturateLayer sat {};
    dsp::LimitLayer limit {};
    dsp::CeilingLayer ceiling {};

    // ---- depth and alignment
    dsp::DepthBlend<2> eqBlend {}, lowBlend {}, limitBlend {};
    dsp::DepthBlend<8> mbBlend {}, widthBlend {}, satBlend {};
    std::array<dsp::DelayLine, 2> eqDry {};
    std::array<std::array<dsp::DelayLine, 2>, kDepthLayers> deltaDelay {};
    std::array<dsp::DelayLine, 2> ceilingBypass {};

    // ---- gains and monitor
    dsp::LinearRamp inTrim {}, outTrim {}, ceilingMix {}, gainMatchDb {};
    float inGain = 1.0f, outGain = 1.0f, matchGain = 1.0f;
    dsp::LinearRamp normalGain {}, globalGain {};
    std::array<dsp::LinearRamp, kDepthLayers> deltaGain {};
    std::atomic<float> gainMatchTargetDb { 0.0f };
    std::atomic<bool> nonFinite { false };

    // ---- latency
    std::array<int, kRows> rows {};
    int satOs = 1;

    // ---- analysis taps (consumers: the analysis thread)
    analysis::ImpactCapture capture {};
    analysis::LoudnessTap loudIn {}, loudOut {};

    // ---- background designer's view (owned by the requestDesigns thread)
    dsp::EqSettings designedEq {};
    float designedPhase = 100.0f;
    dsp::CrossoverPoints designedXover = dsp::kDefaultCrossovers;
};

namespace enginedetail {

inline float dbToGain (float db) noexcept { return std::exp (db * 0.11512925f); }   // ln 10 / 20
inline float finiteOr0 (float v) noexcept { return std::isfinite (v) ? v : 0.0f; }

/** Static bands are the FIR's job in MASTER: the IIR keeps only the dynamic nodes. */
inline dsp::EqSettings iirBands (const dsp::EqSettings& s, LatencyMode mode) noexcept
{
    if (mode != LatencyMode::Master)
        return s;
    auto out = s;
    for (auto& b : out)
        if (! dsp::eqdetail::isDynamic (b))
            b.on = false;
    return out;
}

/** Largest of 1, 2, 4 that is <= the request and keeps fs * factor <= 192 kHz. */
inline int capOversampling (int requested, double sampleRate) noexcept
{
    int f = requested >= 4 ? 4 : requested >= 2 ? 2 : 1;
    while (f > 1 && sampleRate * f > kMaxOversampledRate)
        f /= 2;
    return f;
}

inline void pack (const dsp::StereoBands& b, float* out) noexcept
{
    for (std::size_t i = 0; i < 4; ++i)
    {
        out[i] = b.l[i];
        out[4 + i] = b.r[i];
    }
}

inline dsp::StereoBands unpack (const float* in) noexcept
{
    dsp::StereoBands b;
    for (std::size_t i = 0; i < 4; ++i)
    {
        b.l[i] = in[i];
        b.r[i] = in[4 + i];
    }
    return b;
}

inline void sum (const float* bands8, float& l, float& r) noexcept
{
    l = bands8[0] + bands8[1] + bands8[2] + bands8[3];
    r = bands8[4] + bands8[5] + bands8[6] + bands8[7];
}

inline void snapRamp (dsp::LinearRamp& r) noexcept
{
    r.current = r.target;
    r.remaining = 0;
}

/** Delta / gain-match targets from the current params. */
inline void setMonitorTargets (Engine& e) noexcept
{
    const auto& p = e.params;
    int top = -1;
    for (int i = 0; i < kDepthLayers; ++i)
        if (p.layer[static_cast<std::size_t> (i)].delta)
        {
            top = i;
            break;
        }
    const bool global = top < 0 && p.globalDelta;   // a layer's own delta is more specific
    for (int i = 0; i < kDepthLayers; ++i)
        setTarget (e.deltaGain[static_cast<std::size_t> (i)], i == top ? 1.0f : 0.0f);
    setTarget (e.globalGain, global ? 1.0f : 0.0f);
    setTarget (e.normalGain, top < 0 && ! global ? 1.0f : 0.0f);
    const bool match = p.gainMatch && top < 0 && ! global;
    setTarget (e.gainMatchDb, match ? e.gainMatchTargetDb.load (std::memory_order_relaxed) : 0.0f);
}

/** Reset every running state that holds a non-finite value (audio thread). */
inline void resetNonFinite (Engine& e) noexcept
{
    dsp::resetIfNotFinite (e.eqIir);
    dsp::resetIfNotFinite (e.low);
    for (auto& s : e.domain.iir)
        dsp::resetIfNotFinite (s);
    dsp::resetIfNotFinite (e.sidechain);
    dsp::resetIfNotFinite (e.mb);
    dsp::resetIfNotFinite (e.width);
    dsp::resetIfNotFinite (e.sat);
    dsp::resetIfNotFinite (e.limit);
    dsp::resetIfNotFinite (e.ceiling);
    analysis::resetIfNotFinite (e.loudIn);
    analysis::resetIfNotFinite (e.loudOut);

    // FIR state has no feedback but a whole kernel of memory: clear it rather than wait out
    // ~100 ms of NaN. Only the slots the audio thread owns: the standby slot belongs to the
    // designer unless a swap is running.
    auto& eqActive = dsp::activeSlot (e.eqFir.swap);
    for (auto& c : eqActive.conv)
        dsp::reset (c);
    for (auto& c : dsp::activeSlot (e.domain.fir).ch)
        dsp::reset (c);
    if (dsp::isSwapping (e.eqFir.swap))
        for (auto& c : e.eqFir.swap.slots[static_cast<std::size_t> (1 - e.eqFir.swap.active)].conv)
            dsp::reset (c);
    if (dsp::isSwapping (e.domain.fir))
        for (auto& c : e.domain.fir.slots[static_cast<std::size_t> (1 - e.domain.fir.active)].ch)
            dsp::reset (c);

    auto sanitiseAll = [] (auto& lines) { for (auto& d : lines) dsp::sanitise (d); };
    sanitiseAll (e.eqDry);
    sanitiseAll (e.ceilingBypass);
    for (auto& pair : e.deltaDelay)
        sanitiseAll (pair);
    for (auto* b : { &e.eqBlend, &e.lowBlend, &e.limitBlend })
        sanitiseAll (b->dry);
    for (auto* b : { &e.mbBlend, &e.widthBlend, &e.satBlend })
        sanitiseAll (b->dry);
}

inline void flushDenormals (Engine& e) noexcept
{
    dsp::flushDenormals (e.eqIir);
    dsp::flushDenormals (e.low);
    for (auto& s : e.domain.iir)
        dsp::flushDenormals (s);
    dsp::flushDenormals (e.sidechain);
    dsp::flushDenormals (e.mb);
    dsp::flushDenormals (e.width);
    dsp::flushDenormals (e.sat);
    dsp::flushDenormals (e.limit);
    dsp::flushDenormals (e.ceiling);
    analysis::flushDenormals (e.loudIn);
    analysis::flushDenormals (e.loudOut);
}

} // namespace enginedetail

/** Total latency in samples: what the plugin reports to the host. */
inline int latencySamples (const Engine& e) noexcept
{
    int sum = 0;
    for (int r : e.rows)
        sum += r;
    return sum;
}

/** Per impact-map row (ImpactAnalyser::setLatencies). Sums to latencySamples. */
inline std::array<int, kRows> rowLatencies (const Engine& e) noexcept { return e.rows; }

inline int effectiveSatOs (const Engine& e) noexcept { return e.satOs; }

/** Audio thread, once per block before process(). Never allocates. */
inline void setParams (Engine& e, const EngineParams& in) noexcept
{
    using namespace enginedetail;
    e.params = in;
    const auto& p = e.params;

    dsp::setBands (e.eqIir, iirBands (p.eq, e.config.mode));
    dsp::setSettings (e.low, p.low);
    dsp::setIirCrossovers (e.domain, p.xover);   // FIR mode: no-op, the designer handles it
    const auto points = dsp::activeCrossovers (e.domain);
    if (points.f1 != e.sidechainPoints.f1 || points.f2 != e.sidechainPoints.f2 || points.f3 != e.sidechainPoints.f3)
    {
        e.sidechainPoints = points;
        dsp::setCrossovers (e.sidechain, points);
    }
    dsp::setSettings (e.mb, p.mb);
    dsp::setSettings (e.width, p.width);
    dsp::setSettings (e.sat, p.sat);
    auto lim = p.limit;
    lim.ceilingDb = p.ceiling.ceilingDb;   // one ceiling control for both stages
    dsp::setSettings (e.limit, lim);
    dsp::setSettings (e.ceiling, p.ceiling);

    auto depth = [&] (auto& blend, DepthLayer l)
    {
        const auto& s = p.layer[static_cast<std::size_t> (l)];
        dsp::setDepth (blend, s.depth, s.on);
    };
    depth (e.eqBlend, kEq);
    depth (e.lowBlend, kLow);
    depth (e.mbBlend, kMb);
    depth (e.widthBlend, kWidth);
    depth (e.satBlend, kSat);
    depth (e.limitBlend, kLimit);

    setTarget (e.inTrim, std::isfinite (p.inTrimDb) ? std::clamp (p.inTrimDb, -24.0f, 24.0f) : e.inTrim.target);
    setTarget (e.outTrim, std::isfinite (p.outTrimDb) ? std::clamp (p.outTrimDb, -24.0f, 24.0f) : e.outTrim.target);
    setTarget (e.ceilingMix, p.ceilingOn ? 1.0f : 0.0f);
    setMonitorTargets (e);
}

/** Message thread, audio stopped. Allocates and designs; resets all state. Safe to call
 *  again for a new rate, block size, mode or oversampling factor. */
inline void prepare (Engine& e, const EngineConfig& cfg, const EngineParams& p)
{
    using namespace enginedetail;
    e.config = cfg;
    e.config.sampleRate = cfg.sampleRate > 0.0 ? cfg.sampleRate : 48000.0;
    e.config.maxBlock = std::max (1, cfg.maxBlock);
    const double fs = e.config.sampleRate;
    const bool master = e.config.mode == LatencyMode::Master;
    const auto modeIndex = static_cast<std::size_t> (e.config.mode);
    e.satOs = capOversampling (cfg.satOs, fs);

    dsp::prepare (e.eqIir, fs);
    if (master)
        dsp::prepare (e.eqFir, p.eq, p.eqPhase, fs);
    else
        e.eqFir.prepared = false;   // process() then passes and latencySamples() reports 0
    e.designedEq = p.eq;
    e.designedPhase = p.eqPhase;
    e.designedXover = p.xover;

    dsp::prepare (e.low, fs);
    dsp::prepare (e.domain, master ? dsp::SplitMode::Fir : dsp::SplitMode::Iir, p.xover, fs);
    const int domainLatency = dsp::latencySamples (e.domain);
    // The sidechain must meet the main bands at MB: the main path is late by the FIR EQ and
    // the FIR split (MASTER), the sidechain only by this delay.
    const int sidechainDelay = dsp::latencySamples (e.eqFir) + domainLatency;
    e.sidechainPoints = dsp::activeCrossovers (e.domain);
    dsp::prepare (e.sidechain, e.sidechainPoints, fs, sidechainDelay);
    dsp::setDelay (e.sidechain, sidechainDelay);
    dsp::prepare (e.mb, fs);
    dsp::prepare (e.width, fs);
    dsp::prepare (e.sat, fs, e.satOs, master ? dsp::OsKind::Fir : dsp::OsKind::Iir);
    dsp::prepare (e.limit, fs, dsp::kLimitLookaheadMs[modeIndex]);
    dsp::prepare (e.ceiling, fs, e.config.mode != LatencyMode::Track);

    e.rows = {};
    e.rows[kRowEq] = dsp::latencySamples (e.eqFir);
    e.rows[kRowMb] = domainLatency;   // the split sits at the head of the band domain
    e.rows[kRowSat] = dsp::latencySamples (e.sat);
    e.rows[kRowLimit] = dsp::latencySamples (e.limit);
    e.rows[kRowCeiling] = dsp::latencySamples (e.ceiling);
    const int total = latencySamples (e);

    for (auto* b : { &e.eqBlend, &e.lowBlend, &e.limitBlend })
        dsp::prepare (*b, fs, 0);   // dry arrives aligned (delayed input or a reference)
    for (auto* b : { &e.mbBlend, &e.widthBlend, &e.satBlend })
        dsp::prepare (*b, fs, 0);
    for (auto& d : e.eqDry)
    {
        dsp::prepare (d, static_cast<std::size_t> (e.rows[kRowEq]));
        dsp::setDelay (d, static_cast<std::size_t> (e.rows[kRowEq]));
    }
    for (auto& d : e.ceilingBypass)
    {
        dsp::prepare (d, static_cast<std::size_t> (e.rows[kRowCeiling]));
        dsp::setDelay (d, static_cast<std::size_t> (e.rows[kRowCeiling]));
    }
    // From each layer's output to the monitor point (after LIMIT).
    const int afterSat = e.rows[kRowLimit];
    const int afterDomainHead = e.rows[kRowSat] + afterSat;
    const std::array<int, kDepthLayers> toMonitor {
        domainLatency + afterDomainHead,   // EQ
        domainLatency + afterDomainHead,   // LOW
        afterDomainHead,                   // MB
        afterDomainHead,                   // WIDTH
        afterSat,                          // SAT
        0                                  // LIMIT
    };
    for (std::size_t i = 0; i < kDepthLayers; ++i)
        for (auto& d : e.deltaDelay[i])
        {
            dsp::prepare (d, static_cast<std::size_t> (std::max (1, total)));
            dsp::setDelay (d, static_cast<std::size_t> (toMonitor[i]));
        }

    analysis::prepare (e.capture, kRows + 1);
    analysis::prepare (e.loudIn, fs);
    analysis::prepare (e.loudOut, fs);

    e.inTrim = dsp::makeLinearRamp (kTrimRampMs, fs, 0.0f);
    e.outTrim = dsp::makeLinearRamp (kTrimRampMs, fs, 0.0f);
    e.ceilingMix = dsp::makeLinearRamp (kCeilingSwitchMs, fs, 1.0f);
    e.gainMatchDb = dsp::makeLinearRamp (kGainMatchRampMs, fs, 0.0f);
    e.normalGain = dsp::makeLinearRamp (kMonitorRampMs, fs, 1.0f);
    e.globalGain = dsp::makeLinearRamp (kMonitorRampMs, fs, 0.0f);
    for (auto& g : e.deltaGain)
        g = dsp::makeLinearRamp (kMonitorRampMs, fs, 0.0f);
    e.nonFinite.store (false);

    setParams (e, p);
    dsp::snapToTargets (e.eqIir);
    dsp::snapToTargets (e.low);
    dsp::snapToTargets (e.mb);
    dsp::snapToTargets (e.width);
    dsp::snapToTargets (e.sat);
    dsp::snapToTargets (e.limit);
    dsp::snapToTargets (e.ceiling);
    for (auto* b : { &e.eqBlend, &e.lowBlend, &e.limitBlend })
        dsp::snapToTarget (*b);
    for (auto* b : { &e.mbBlend, &e.widthBlend, &e.satBlend })
        dsp::snapToTarget (*b);
    for (auto* r : { &e.inTrim, &e.outTrim, &e.ceilingMix, &e.gainMatchDb, &e.normalGain, &e.globalGain })
        snapRamp (*r);
    for (auto& g : e.deltaGain)
        snapRamp (g);
    e.inGain = dbToGain (e.inTrim.current);
    e.outGain = dbToGain (e.outTrim.current);
    e.matchGain = dbToGain (e.gainMatchDb.current);
}

/** Background thread (one only). MASTER: starts FIR redesigns for changed static EQ bands,
 *  eq.phase or crossover points. Returns true if a design was made. A design refused because
 *  a swap is still running is retried on the next call (the designed copy is unchanged). */
inline bool requestDesigns (Engine& e, const EngineParams& p)
{
    if (e.config.mode != LatencyMode::Master)
        return false;
    bool made = false;
    if (p.eq != e.designedEq || p.eqPhase != e.designedPhase)
        if (dsp::designStandby (e.eqFir, p.eq, p.eqPhase))
        {
            e.designedEq = p.eq;
            e.designedPhase = p.eqPhase;
            made = true;
        }
    const auto& x = p.xover;
    if (x.f1 != e.designedXover.f1 || x.f2 != e.designedXover.f2 || x.f3 != e.designedXover.f3)
        if (dsp::designStandby (e.domain, x))
        {
            e.designedXover = x;
            made = true;
        }
    return made;
}

/** Any thread: the analysis thread's measured offset (output vs input loudness). Ignores
 *  non-finite values; clamped to +-24 dB. Takes effect at the next process block. */
inline void setGainMatchDb (Engine& e, float db) noexcept
{
    if (std::isfinite (db))
        e.gainMatchTargetDb.store (std::clamp (db, -kGainMatchMaxDb, kGainMatchMaxDb), std::memory_order_relaxed);
}

/** Any thread: true once after a block was muted for a non-finite sample. */
inline bool takeNonFiniteFlag (Engine& e) noexcept { return e.nonFinite.exchange (false, std::memory_order_acq_rel); }

/** Audio thread. In place on l and r (stereo; mono hosts duplicate L). sidechainL/R may be
 *  null (no sidechain bus), in which case MULTIBAND uses its internal detector. */
inline void process (Engine& e, float* l, float* r, int numSamples,
                     const float* sidechainL = nullptr, const float* sidechainR = nullptr) noexcept
{
    using namespace enginedetail;
    if (numSamples <= 0 || l == nullptr || r == nullptr)
        return;

    // A non-finite input never enters the state: replace it, process (timing stays intact),
    // then mute the block and raise the flag.
    bool bad = false;
    for (int i = 0; i < numSamples; ++i)
        if (! std::isfinite (l[i]) || ! std::isfinite (r[i]))
        {
            bad = true;
            l[i] = finiteOr0 (l[i]);
            r[i] = finiteOr0 (r[i]);
        }

    setMonitorTargets (e);   // picks up a new gain-match offset without a setParams
    dsp::beginBlock (e.eqFir);
    dsp::beginBlock (e.domain);
    analysis::beginBlock (e.capture, numSamples);
    const bool external = e.params.mb.sidechain == dsp::MbSidechain::External
                       && sidechainL != nullptr && sidechainR != nullptr;

    for (int i = 0; i < numSamples; ++i)
    {
        auto tap = [&] (int t, float a, float b) { analysis::write (e.capture, t, i, a, b); };
        float x[2] { l[i], r[i] };
        tap (0, x[0], x[1]);
        analysis::push (e.loudIn, x[0], x[1]);

        // ---- IN TRIM
        if (isSmoothing (e.inTrim))
            e.inGain = dbToGain (step (e.inTrim));
        x[0] *= e.inGain;
        x[1] *= e.inGain;
        tap (1, x[0], x[1]);

        std::array<std::array<float, 2>, kDepthLayers> diff {};
        auto diff2 = [&] (DepthLayer k, const float* out, const float* dry)
        {
            diff[static_cast<std::size_t> (k)] = { out[0] - dry[0], out[1] - dry[1] };
        };

        // ---- EQ (MASTER: static bands in the FIR, dynamic nodes in the IIR after it)
        const float eqDry[2] { dsp::process (e.eqDry[0], x[0]), dsp::process (e.eqDry[1], x[1]) };
        float eqWet[2] { x[0], x[1] };
        dsp::process (e.eqFir, eqWet[0], eqWet[1]);
        dsp::process (e.eqIir, eqWet[0], eqWet[1]);
        float y[2];
        dsp::process (e.eqBlend, eqDry, eqWet, y);
        diff2 (kEq, y, eqDry);
        tap (2, y[0], y[1]);

        // ---- LOW
        const auto lf = dsp::process (e.low, y[0], y[1]);
        const float lowDry[2] { lf.refL, lf.refR }, lowWet[2] { lf.l, lf.r };
        dsp::process (e.lowBlend, lowDry, lowWet, y);
        diff2 (kLow, y, lowDry);
        tap (3, y[0], y[1]);

        // ---- band domain: MB -> WIDTH -> SAT
        float dry8[8], wet8[8], out8[8], dl, dr, ol, orr;
        auto bands = dsp::split (e.domain, y[0], y[1]);
        pack (bands, dry8);
        dsp::StereoBands scBands;
        const dsp::StereoBands* sc = nullptr;
        if (external)
        {
            scBands = dsp::process (e.sidechain, finiteOr0 (sidechainL[i]), finiteOr0 (sidechainR[i]));
            sc = &scBands;
        }
        dsp::process (e.mb, bands, sc);
        pack (bands, wet8);
        dsp::process (e.mbBlend, dry8, wet8, out8);
        sum (out8, ol, orr);
        sum (dry8, dl, dr);
        diff[kMb] = { ol - dl, orr - dr };
        tap (4, ol, orr);

        std::copy (out8, out8 + 8, dry8);
        bands = unpack (dry8);
        dsp::process (e.width, bands);
        pack (bands, wet8);
        dsp::process (e.widthBlend, dry8, wet8, out8);
        sum (out8, ol, orr);
        sum (dry8, dl, dr);
        diff[kWidth] = { ol - dl, orr - dr };
        tap (5, ol, orr);

        bands = unpack (out8);
        dsp::StereoBands ref;
        dsp::process (e.sat, bands, ref);
        pack (ref, dry8);
        pack (bands, wet8);
        dsp::process (e.satBlend, dry8, wet8, out8);
        sum (out8, ol, orr);
        sum (dry8, dl, dr);
        diff[kSat] = { ol - dl, orr - dr };
        tap (6, ol, orr);

        // ---- LIMIT
        const auto lim = dsp::process (e.limit, ol, orr);
        const float limDry[2] { lim.refL, lim.refR }, limWet[2] { lim.l, lim.r };
        dsp::process (e.limitBlend, limDry, limWet, y);
        diff2 (kLimit, y, limDry);
        tap (7, y[0], y[1]);

        // ---- monitor: normal, one layer's delta, or the sum of all of them
        const float gN = step (e.normalGain), gG = step (e.globalGain);
        float m[2] { gN * y[0], gN * y[1] };
        for (std::size_t k = 0; k < kDepthLayers; ++k)
        {
            const float gk = step (e.deltaGain[k]);
            for (std::size_t c = 0; c < 2; ++c)
                m[c] += (gG + gk) * dsp::process (e.deltaDelay[k][c], diff[k][c]);
        }

        // ---- OUT TRIM
        if (isSmoothing (e.outTrim))
            e.outGain = dbToGain (step (e.outTrim));
        m[0] *= e.outGain;
        m[1] *= e.outGain;
        tap (8, m[0], m[1]);

        // ---- CEILING (always runs, so switching it on never starts from stale state)
        float c[2] { m[0], m[1] };
        dsp::process (e.ceiling, c[0], c[1]);
        const float k = step (e.ceilingMix);
        for (std::size_t ch = 0; ch < 2; ++ch)
        {
            const float bypass = dsp::process (e.ceilingBypass[ch], m[ch]);
            c[ch] = k == 1.0f ? c[ch] : bypass + k * (c[ch] - bypass);
        }
        tap (9, c[0], c[1]);
        analysis::push (e.loudOut, c[0], c[1]);

        // ---- gain match (monitor only: after everything that is printed)
        if (isSmoothing (e.gainMatchDb))
            e.matchGain = dbToGain (step (e.gainMatchDb));
        l[i] = c[0] * e.matchGain;
        r[i] = c[1] * e.matchGain;
    }

    analysis::endBlock (e.capture);
    flushDenormals (e);

    for (int i = 0; i < numSamples && ! bad; ++i)
        bad = ! std::isfinite (l[i]) || ! std::isfinite (r[i]);
    if (bad)
    {
        std::fill (l, l + numSamples, 0.0f);
        std::fill (r, r + numSamples, 0.0f);
        resetNonFinite (e);
        e.nonFinite.store (true, std::memory_order_release);
    }
}

} // namespace strat3::engine
