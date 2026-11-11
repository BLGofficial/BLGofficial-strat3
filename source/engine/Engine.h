#pragma once
// The fixed-order chain that wires every layer (headless: no AudioProcessor types).
//
//   IN TRIM -> STEMS -> six depth layers in EngineConfig::order -> OUT TRIM
//   -> monitor (normal | delta) -> CEILING -> gain match (monitor only)
//   default order: EQ -> LOW -> [MB -> WIDTH -> SAT, one shared band split] -> LIMIT
//
// STEMS (module 22b) is always first and not part of the order. It runs in MIX and MASTER
// only: its 1024-sample STFT is too much latency for TRACK. Until the impact map gets a STEMS
// row (22c), its latency and effect are counted in the IN TRIM row. It is an STFT, so the
// chain runs in three passes per block: IN TRIM per sample, STEMS on the block, the rest per
// sample. The analysis pass captures the STEMS input (after IN TRIM).
//
// Order (Phase 5 rule): neighbouring band layers (MB, WIDTH, SAT) share one split, a band
// domain; a broadband layer between them starts another. Each domain adds its split latency
// (MASTER: one FIR, ~52 ms), so the order is part of the prepared config, not a parameter.
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
// offset. The input is measured delayed by the chain's latency, so both meters hear the same
// moment: when the music stops, both windows empty together (measured ahead, the input's
// emptied first and the offset ran to the -24 dB limit in MASTER). The offset is applied
// after the ceiling, so it is monitor-only and never printed. It is muted (0 dB) while any
// delta is heard, or a quiet delta would be boosted by up to +24 dB.
//
// Threads: prepare() and setStemsCapture() on the message thread with the audio stopped;
// setParams(), process() and setStemsCache() on the audio thread; requestDesigns() on one
// background thread (MASTER FIR redesigns); setGainMatchDb() and takeNonFiniteFlag() from
// any thread.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>

#include "analysis/Correlation.h"
#include "analysis/ImpactAnalyser.h"
#include "analysis/Loudness1770.h"
#include "analysis/TruePeak.h"
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
#include "dsp/layers/StemsLayer.h"
#include "dsp/layers/WidthLayer.h"
#include "dsp/stems/Capture.h"
#include "dsp/stems/Transport.h"

namespace strat3::engine {

enum class LatencyMode { Track, Mix, Master };   // global.latencyMode order: append only

/** Layers with a depth handle, in chain order. */
enum DepthLayer { kEq, kLow, kMb, kWidth, kSat, kLimit, kDepthLayers };

/** Impact-map rows (one per stage, trims included); tap r is a row's input, r + 1 its output.
 *  Rows are chain positions: the layer names below are the default order. Use rowOf(). */
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
    LayerSwitch stems { false, 100.0f, false };   // stems.on defaults to off
    dsp::StemsSettings stemsSettings {};
    bool operator== (const EngineParams&) const = default;
};

/** Chain order of the depth layers (IN TRIM before, OUT TRIM and CEILING after, always). */
using LayerOrder = std::array<DepthLayer, kDepthLayers>;
inline constexpr LayerOrder kDefaultOrder { kEq, kLow, kMb, kWidth, kSat, kLimit };
/** Neighbouring band layers share one split (a band domain); at most three runs fit in six. */
inline constexpr int kMaxBandDomains = 3;

inline constexpr bool isBandLayer (DepthLayer k) noexcept { return k == kMb || k == kWidth || k == kSat; }

/** True for an exact permutation of the six depth layers. */
inline bool isValidOrder (const LayerOrder& o) noexcept
{
    std::array<bool, kDepthLayers> seen {};
    for (auto k : o)
    {
        const auto i = static_cast<std::size_t> (k);
        if (k < 0 || k >= kDepthLayers || seen[i])
            return false;
        seen[i] = true;
    }
    return true;
}

struct EngineConfig
{
    double sampleRate = 48000.0;
    int maxBlock = 512;
    LatencyMode mode = LatencyMode::Mix;
    int satOs = 2;   // requested factor (1, 2, 4); capped by effectiveSatOs
    LayerOrder order = kDefaultOrder;   // not a permutation: prepare() uses kDefaultOrder
};

/** Not copyable or movable (atomics in the FIR swaps and taps): construct in place. */
/** Output metering of the last process() call (audio thread; the processor publishes it).
 *  Linear peaks; grDb <= 0 is the most reduction of CEILING + LIMIT (detector gain, before the
 *  depth blend [INFERRED]: at depth < 100 % the audible reduction is smaller). */
struct BlockMeters
{
    std::array<float, 2> inPeak {}, outPeak {};   // sample peak, IN TRIM input / CEILING output
    float outTruePeak = 0.0f;                     // 4x true peak of the CEILING output
    float grDb = 0.0f;
    float correlation = 1.0f;                     // output, at the end of the block
};

struct Engine
{
    EngineConfig config {};
    EngineParams params {};

    // ---- layers
    dsp::EqLayer eqIir {};
    dsp::FirEq eqFir {};
    dsp::LowLayer low {};
    std::array<dsp::BandDomain, kMaxBandDomains> domains {};   // [0, domainCount) in use
    dsp::SidechainSplit sidechain {};
    dsp::CrossoverPoints sidechainPoints = dsp::kDefaultCrossovers;
    dsp::MultibandLayer mb {};
    dsp::WidthLayer width {};
    dsp::SaturateLayer sat {};
    dsp::LimitLayer limit {};
    dsp::CeilingLayer ceiling {};
    dsp::StemsLayer stems {};
    std::array<std::vector<float>, 2> stemsWet {};   // maxBlock: STEMS runs on the block
    dsp::StemsCapture* stemsCapture = nullptr;       // not owned; analysis pass only

    // ---- depth and alignment
    dsp::DepthBlend<2> eqBlend {}, lowBlend {}, limitBlend {};
    dsp::DepthBlend<8> mbBlend {}, widthBlend {}, satBlend {};
    std::array<dsp::DelayLine, 2> eqDry {};
    dsp::DepthBlend<2> stemsBlend {};
    std::array<dsp::DelayLine, 2> stemsDry {}, stemsDeltaDelay {};
    std::array<std::array<dsp::DelayLine, 2>, kDepthLayers> deltaDelay {};
    std::array<dsp::DelayLine, 2> ceilingBypass {};

    // ---- gains and monitor
    dsp::LinearRamp inTrim {}, outTrim {}, ceilingMix {}, gainMatchDb {};
    float inGain = 1.0f, outGain = 1.0f, matchGain = 1.0f;
    dsp::LinearRamp normalGain {}, globalGain {};
    std::array<dsp::LinearRamp, kDepthLayers> deltaGain {};
    dsp::LinearRamp stemsDeltaGain {};
    std::atomic<float> gainMatchTargetDb { 0.0f };
    std::atomic<bool> nonFinite { false };

    // ---- order and latency
    LayerOrder order = kDefaultOrder;
    std::array<int, kDepthLayers> domainOf {};    // per chain slot: band domain, -1 broadband
    std::array<int, kDepthLayers> slotOf {};      // per layer: chain slot
    int domainCount = 1, mbDomain = 0;
    std::array<int, kRows> rows {};               // per chain position: 0 IN TRIM (+ STEMS), 1 + slot, 7, 8
    int satOs = 1;

    // ---- analysis taps (consumers: the analysis thread)
    analysis::ImpactCapture capture {};
    analysis::LoudnessTap loudIn {}, loudOut {};
    std::array<dsp::DelayLine, 2> loudInDelay {};   // input loudness at the output's moment
    BlockMeters blockMeters {};
    std::array<analysis::TruePeakDetector, 2> outTp {};   // delivery meter (the CEILING has its own)
    analysis::CorrelationMeter outCorr {};

    // ---- background designer's view (owned by the requestDesigns thread)
    dsp::EqSettings designedEq {};
    float designedPhase = 100.0f;
    std::array<dsp::CrossoverPoints, kMaxBandDomains> designedXover { dsp::kDefaultCrossovers, dsp::kDefaultCrossovers, dsp::kDefaultCrossovers };
};

/** The one place that decides where STEMS runs (and adds its latency): MIX and MASTER.
 *  [INFERRED; pending the owner's pick between this, MASTER only, and only while on] */
inline constexpr bool stemsInChain (LatencyMode m) noexcept { return m != LatencyMode::Track; }

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
    const bool stemsTop = stemsInChain (e.config.mode) && p.stems.delta;   // STEMS heads the chain
    int top = -1;   // "topmost" = first in the chain
    for (auto k : e.order)
        if (! stemsTop && p.layer[static_cast<std::size_t> (k)].delta)
        {
            top = k;
            break;
        }
    const bool layerDelta = stemsTop || top >= 0;
    const bool global = ! layerDelta && p.globalDelta;   // a layer's own delta is more specific
    for (int i = 0; i < kDepthLayers; ++i)
        setTarget (e.deltaGain[static_cast<std::size_t> (i)], i == top ? 1.0f : 0.0f);
    setTarget (e.stemsDeltaGain, stemsTop ? 1.0f : 0.0f);
    setTarget (e.globalGain, global ? 1.0f : 0.0f);
    setTarget (e.normalGain, ! layerDelta && ! global ? 1.0f : 0.0f);
    const bool match = p.gainMatch && ! layerDelta && ! global;
    setTarget (e.gainMatchDb, match ? e.gainMatchTargetDb.load (std::memory_order_relaxed) : 0.0f);
}

/** Reset every running state that holds a non-finite value (audio thread). */
inline void resetNonFinite (Engine& e) noexcept
{
    dsp::resetIfNotFinite (e.eqIir);
    dsp::resetIfNotFinite (e.low);
    for (int d = 0; d < e.domainCount; ++d)
        for (auto& s : e.domains[static_cast<std::size_t> (d)].iir)
            dsp::resetIfNotFinite (s);
    dsp::resetIfNotFinite (e.sidechain);
    dsp::resetIfNotFinite (e.mb);
    dsp::resetIfNotFinite (e.width);
    dsp::resetIfNotFinite (e.sat);
    dsp::resetIfNotFinite (e.limit);
    dsp::resetIfNotFinite (e.ceiling);
    dsp::resetIfNotFinite (e.stems);
    analysis::resetIfNotFinite (e.loudIn);
    analysis::resetIfNotFinite (e.loudOut);
    if (! analysis::isFinite (e.outCorr))
        analysis::reset (e.outCorr);
    for (auto& d : e.outTp)
        if (! std::all_of (d.ring.begin(), d.ring.end(), [] (float v) { return std::isfinite (v); }))
            analysis::reset (d);

    // FIR state has no feedback but a whole kernel of memory: clear it rather than wait out
    // ~100 ms of NaN. Only the slots the audio thread owns: the standby slot belongs to the
    // designer unless a swap is running.
    auto& eqActive = dsp::activeSlot (e.eqFir.swap);
    for (auto& c : eqActive.conv)
        dsp::reset (c);
    for (int d = 0; d < e.domainCount; ++d)
    {
        auto& dom = e.domains[static_cast<std::size_t> (d)];
        for (auto& c : dsp::activeSlot (dom.fir).ch)
            dsp::reset (c);
        if (dsp::isSwapping (dom.fir))
            for (auto& c : dom.fir.slots[static_cast<std::size_t> (1 - dom.fir.active)].ch)
                dsp::reset (c);
    }
    if (dsp::isSwapping (e.eqFir.swap))
        for (auto& c : e.eqFir.swap.slots[static_cast<std::size_t> (1 - e.eqFir.swap.active)].conv)
            dsp::reset (c);

    auto sanitiseAll = [] (auto& lines) { for (auto& d : lines) dsp::sanitise (d); };
    sanitiseAll (e.eqDry);
    sanitiseAll (e.ceilingBypass);
    sanitiseAll (e.stemsDry);
    sanitiseAll (e.stemsDeltaDelay);
    sanitiseAll (e.stemsBlend.dry);
    sanitiseAll (e.loudInDelay);
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
    for (int d = 0; d < e.domainCount; ++d)
        for (auto& s : e.domains[static_cast<std::size_t> (d)].iir)
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

/** Output metering of the last process() call. Audio thread (read it right after process()). */
inline const BlockMeters& blockMeters (const Engine& e) noexcept { return e.blockMeters; }

/** The prepared chain order (kDefaultOrder if the config's order was not a permutation). */
inline LayerOrder layerOrder (const Engine& e) noexcept { return e.order; }
/** Number of band splits the order needs (1 to 3). */
inline int bandDomainCount (const Engine& e) noexcept { return e.domainCount; }
/** Impact-map row of a layer: 1 + its chain slot. */
inline int rowOf (const Engine& e, DepthLayer k) noexcept { return 1 + e.slotOf[static_cast<std::size_t> (k)]; }
inline const dsp::BandDomain& bandDomain (const Engine& e, int d) noexcept
{
    return e.domains[static_cast<std::size_t> (std::clamp (d, 0, e.domainCount - 1))];
}

/** Audio thread, once per block before process(). Never allocates. */
inline void setParams (Engine& e, const EngineParams& in) noexcept
{
    using namespace enginedetail;
    e.params = in;
    const auto& p = e.params;

    dsp::setBands (e.eqIir, iirBands (p.eq, e.config.mode));
    dsp::setSettings (e.low, p.low);
    for (int d = 0; d < e.domainCount; ++d)   // FIR mode: no-op, the designer handles it
        dsp::setIirCrossovers (e.domains[static_cast<std::size_t> (d)], p.xover);
    const auto points = dsp::activeCrossovers (e.domains[static_cast<std::size_t> (e.mbDomain)]);
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
    dsp::setSettings (e.stems, p.stemsSettings);

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
    dsp::setDepth (e.stemsBlend, p.stems.depth, p.stems.on);   // TRACK never runs it

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
    // ---- order -> band domains
    e.order = isValidOrder (cfg.order) ? cfg.order : kDefaultOrder;
    e.config.order = e.order;
    e.domainCount = 0;
    for (std::size_t s = 0; s < kDepthLayers; ++s)
    {
        const auto k = e.order[s];
        e.slotOf[static_cast<std::size_t> (k)] = static_cast<int> (s);
        if (! isBandLayer (k))
        {
            e.domainOf[s] = -1;
            continue;
        }
        if (s == 0 || ! isBandLayer (e.order[s - 1]))
            ++e.domainCount;
        e.domainOf[s] = e.domainCount - 1;
        if (k == kMb)
            e.mbDomain = e.domainOf[s];
    }
    for (int d = 0; d < e.domainCount; ++d)
    {
        const auto i = static_cast<std::size_t> (d);
        dsp::prepare (e.domains[i], master ? dsp::SplitMode::Fir : dsp::SplitMode::Iir, p.xover, fs);
        e.designedXover[i] = p.xover;
    }
    const int splitLatency = dsp::latencySamples (e.domains[0]);   // every domain is the same split

    dsp::prepare (e.low, fs);
    dsp::prepare (e.mb, fs);
    dsp::prepare (e.width, fs);
    dsp::prepare (e.sat, fs, e.satOs, master ? dsp::OsKind::Fir : dsp::OsKind::Iir);
    dsp::prepare (e.limit, fs, dsp::kLimitLookaheadMs[modeIndex]);
    dsp::prepare (e.ceiling, fs, e.config.mode != LatencyMode::Track);
    dsp::prepare (e.stems, fs);   // keeps the cache pointer and the settings
    for (auto& w : e.stemsWet)
        w.assign (static_cast<size_t> (e.config.maxBlock), 0.0f);

    // ---- rows: what each chain position adds; a split counts at the head of its run
    e.rows = {};
    const int stemsLatency = stemsInChain (e.config.mode) ? dsp::latencySamples (e.stems) : 0;
    e.rows[kRowInTrim] = stemsLatency;
    for (std::size_t s = 0; s < kDepthLayers; ++s)
    {
        int lat = 0;
        switch (e.order[s])
        {
            case kEq:    lat = dsp::latencySamples (e.eqFir); break;
            case kSat:   lat = dsp::latencySamples (e.sat); break;
            case kLimit: lat = dsp::latencySamples (e.limit); break;
            case kLow: case kMb: case kWidth: case kDepthLayers: break;
        }
        const int d = e.domainOf[s];
        if (d >= 0 && (s == 0 || e.domainOf[s - 1] != d))
            lat += splitLatency;
        e.rows[1 + s] = lat;
    }
    e.rows[kRowCeiling] = dsp::latencySamples (e.ceiling);
    const int total = latencySamples (e);

    // The sidechain must meet the main bands at MB's input: everything up to and including
    // MB's row (MB itself adds nothing; its row holds the split when MB heads the run), and
    // STEMS ahead of all of it.
    int sidechainDelay = e.rows[kRowInTrim];
    for (int s = 0; s <= e.slotOf[kMb]; ++s)
        sidechainDelay += e.rows[static_cast<std::size_t> (1 + s)];
    e.sidechainPoints = dsp::activeCrossovers (e.domains[static_cast<std::size_t> (e.mbDomain)]);
    dsp::prepare (e.sidechain, e.sidechainPoints, fs, sidechainDelay);
    dsp::setDelay (e.sidechain, sidechainDelay);

    for (auto* b : { &e.eqBlend, &e.lowBlend, &e.limitBlend })
        dsp::prepare (*b, fs, 0);   // dry arrives aligned (delayed input or a reference)
    for (auto* b : { &e.mbBlend, &e.widthBlend, &e.satBlend })
        dsp::prepare (*b, fs, 0);
    for (auto& d : e.eqDry)
    {
        dsp::prepare (d, static_cast<std::size_t> (dsp::latencySamples (e.eqFir)));
        dsp::setDelay (d, static_cast<std::size_t> (dsp::latencySamples (e.eqFir)));
    }
    for (auto& d : e.ceilingBypass)
    {
        dsp::prepare (d, static_cast<std::size_t> (e.rows[kRowCeiling]));
        dsp::setDelay (d, static_cast<std::size_t> (e.rows[kRowCeiling]));
    }
    // STEMS: dry delayed by the layer (as for EQ), and its addition carried past all six layers.
    dsp::prepare (e.stemsBlend, fs, 0);
    int sixLayers = 0;
    for (std::size_t s = 0; s < kDepthLayers; ++s)
        sixLayers += e.rows[1 + s];
    for (std::size_t ch = 0; ch < 2; ++ch)
    {
        dsp::prepare (e.stemsDry[ch], static_cast<std::size_t> (stemsLatency));
        dsp::setDelay (e.stemsDry[ch], static_cast<std::size_t> (stemsLatency));
        dsp::prepare (e.stemsDeltaDelay[ch], static_cast<std::size_t> (std::max (1, sixLayers)));
        dsp::setDelay (e.stemsDeltaDelay[ch], static_cast<std::size_t> (sixLayers));
    }
    // From each layer's output to the monitor point (after the last layer).
    for (std::size_t k = 0; k < kDepthLayers; ++k)
    {
        int toMonitor = 0;
        for (int s = e.slotOf[k] + 1; s < kDepthLayers; ++s)
            toMonitor += e.rows[static_cast<std::size_t> (1 + s)];
        for (auto& d : e.deltaDelay[k])
        {
            dsp::prepare (d, static_cast<std::size_t> (std::max (1, total)));
            dsp::setDelay (d, static_cast<std::size_t> (toMonitor));
        }
    }

    analysis::prepare (e.capture, kRows + 1);
    analysis::prepare (e.loudIn, fs);
    for (auto& d : e.loudInDelay)
    {
        dsp::prepare (d, static_cast<std::size_t> (std::max (1, total)));
        dsp::setDelay (d, static_cast<std::size_t> (total));
    }
    analysis::prepare (e.loudOut, fs);
    analysis::prepare (e.outCorr, fs);
    for (auto& d : e.outTp)
        analysis::reset (d);
    e.blockMeters = {};

    e.inTrim = dsp::makeLinearRamp (kTrimRampMs, fs, 0.0f);
    e.outTrim = dsp::makeLinearRamp (kTrimRampMs, fs, 0.0f);
    e.ceilingMix = dsp::makeLinearRamp (kCeilingSwitchMs, fs, 1.0f);
    e.gainMatchDb = dsp::makeLinearRamp (kGainMatchRampMs, fs, 0.0f);
    e.normalGain = dsp::makeLinearRamp (kMonitorRampMs, fs, 1.0f);
    e.globalGain = dsp::makeLinearRamp (kMonitorRampMs, fs, 0.0f);
    for (auto& g : e.deltaGain)
        g = dsp::makeLinearRamp (kMonitorRampMs, fs, 0.0f);
    e.stemsDeltaGain = dsp::makeLinearRamp (kMonitorRampMs, fs, 0.0f);
    e.nonFinite.store (false);

    setParams (e, p);
    dsp::snapToTargets (e.eqIir);
    dsp::snapToTargets (e.low);
    dsp::snapToTargets (e.mb);
    dsp::snapToTargets (e.width);
    dsp::snapToTargets (e.sat);
    dsp::snapToTargets (e.limit);
    dsp::snapToTargets (e.ceiling);
    for (auto* b : { &e.eqBlend, &e.lowBlend, &e.limitBlend, &e.stemsBlend })
        dsp::snapToTarget (*b);
    for (auto* b : { &e.mbBlend, &e.widthBlend, &e.satBlend })
        dsp::snapToTarget (*b);
    for (auto* r : { &e.inTrim, &e.outTrim, &e.ceilingMix, &e.gainMatchDb, &e.normalGain, &e.globalGain, &e.stemsDeltaGain })
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
    // Every split gets the same points; each swaps on its own, so each keeps its own record.
    for (int d = 0; d < e.domainCount; ++d)
    {
        const auto i = static_cast<std::size_t> (d);
        if (p.xover != e.designedXover[i] && dsp::designStandby (e.domains[i], p.xover))
        {
            e.designedXover[i] = p.xover;
            made = true;
        }
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

/** Audio thread: the mask cache STEMS reads, null for none. Not owned: the caller keeps it
 *  alive until the audio thread has been handed another. Same pointer again: no change. */
inline void setStemsCache (Engine& e, const dsp::MaskCache* cache) noexcept { dsp::setCache (e.stems, cache); }

/** Message thread, audio stopped: where process() offers the STEMS input during an analysis
 *  pass (MIX and MASTER only). Not owned; null for none. */
inline void setStemsCapture (Engine& e, dsp::StemsCapture* capture) noexcept { e.stemsCapture = capture; }

/** Audio thread: what STEMS is doing with its cache (read it right after process()). */
inline dsp::StemsStatus stemsStatus (const Engine& e) noexcept { return dsp::status (e.stems); }

/** Any thread: true once after a block was muted for a non-finite sample. */
inline bool takeNonFiniteFlag (Engine& e) noexcept { return e.nonFinite.exchange (false, std::memory_order_acq_rel); }

namespace enginedetail {

/** At most maxBlock samples, all finite. Meters accumulate into blockMeters. */
inline void processPiece (Engine& e, float* l, float* r, int numSamples,
                          const float* sidechainL, const float* sidechainR, const dsp::Transport& t) noexcept
{
    setMonitorTargets (e);   // picks up a new gain-match offset without a setParams
    dsp::beginBlock (e.eqFir);
    for (int d = 0; d < e.domainCount; ++d)
        dsp::beginBlock (e.domains[static_cast<std::size_t> (d)]);
    analysis::beginBlock (e.capture, numSamples);
    auto& bm = e.blockMeters;
    const bool meterLimit = e.params.layer[kLimit].on, meterCeiling = e.params.ceilingOn;
    const bool external = e.params.mb.sidechain == dsp::MbSidechain::External
                       && sidechainL != nullptr && sidechainR != nullptr;

    // ---- pass 1, per sample: input meters and IN TRIM, in place
    for (int i = 0; i < numSamples; ++i)
    {
        analysis::write (e.capture, 0, i, l[i], r[i]);
        bm.inPeak[0] = std::max (bm.inPeak[0], std::abs (l[i]));
        bm.inPeak[1] = std::max (bm.inPeak[1], std::abs (r[i]));
        analysis::push (e.loudIn, dsp::process (e.loudInDelay[0], l[i]), dsp::process (e.loudInDelay[1], r[i]));
        if (isSmoothing (e.inTrim))
            e.inGain = dbToGain (step (e.inTrim));
        l[i] *= e.inGain;
        r[i] *= e.inGain;
    }

    // ---- pass 2, the block: STEMS is an STFT. l / r keep its input (the dry path).
    const bool stems = stemsInChain (e.config.mode) && static_cast<int> (e.stemsWet[0].size()) >= numSamples;
    float* wetL = e.stemsWet[0].data();
    float* wetR = e.stemsWet[1].data();
    if (stems)
    {
        if (e.stemsCapture != nullptr)
            dsp::write (*e.stemsCapture, l, r, numSamples, t);
        std::copy (l, l + numSamples, wetL);
        std::copy (r, r + numSamples, wetR);
        dsp::process (e.stems, wetL, wetR, numSamples, t);
    }

    // ---- pass 3, per sample: STEMS depth, the six layers, monitor, CEILING
    for (int i = 0; i < numSamples; ++i)
    {
        auto tap = [&] (int tp, float a, float b) { analysis::write (e.capture, tp, i, a, b); };
        float x[2] { l[i], r[i] };
        float stemsDiff[2] { 0.0f, 0.0f };
        if (stems)
        {
            const float dry[2] { dsp::process (e.stemsDry[0], x[0]), dsp::process (e.stemsDry[1], x[1]) };
            const float wet[2] { wetL[i], wetR[i] };
            dsp::process (e.stemsBlend, dry, wet, x);
            stemsDiff[0] = x[0] - dry[0];
            stemsDiff[1] = x[1] - dry[1];
        }
        tap (1, x[0], x[1]);   // IN TRIM row: IN TRIM and STEMS until STEMS gets its own row

        std::array<std::array<float, 2>, kDepthLayers> diff {};
        auto diff2 = [&] (DepthLayer k, const float* out, const float* dry)
        {
            diff[static_cast<std::size_t> (k)] = { out[0] - dry[0], out[1] - dry[1] };
        };

        // ---- the six depth layers, in the prepared order. A band run is split at its first
        // layer; each band layer's output is summed for its tap, delta and (at the end of
        // the run) the next broadband layer.
        float y[2] { x[0], x[1] };
        dsp::StereoBands bands {};
        for (std::size_t s = 0; s < kDepthLayers; ++s)
        {
            const auto k = e.order[s];
            const int d = e.domainOf[s];
            float dry8[8], wet8[8], out8[8];
            if (d >= 0 && (s == 0 || e.domainOf[s - 1] != d))
                bands = dsp::split (e.domains[static_cast<std::size_t> (d)], y[0], y[1]);

            switch (k)
            {
                case kEq:
                {
                    // MASTER: static bands in the FIR, dynamic nodes in the IIR after it
                    const float eqDry[2] { dsp::process (e.eqDry[0], y[0]), dsp::process (e.eqDry[1], y[1]) };
                    float eqWet[2] { y[0], y[1] };
                    dsp::process (e.eqFir, eqWet[0], eqWet[1]);
                    dsp::process (e.eqIir, eqWet[0], eqWet[1]);
                    dsp::process (e.eqBlend, eqDry, eqWet, y);
                    diff2 (kEq, y, eqDry);
                    break;
                }
                case kLow:
                {
                    const auto lf = dsp::process (e.low, y[0], y[1]);
                    const float lowDry[2] { lf.refL, lf.refR }, lowWet[2] { lf.l, lf.r };
                    dsp::process (e.lowBlend, lowDry, lowWet, y);
                    diff2 (kLow, y, lowDry);
                    break;
                }
                case kLimit:
                {
                    const auto lim = dsp::process (e.limit, y[0], y[1]);
                    const float limDry[2] { lim.refL, lim.refR }, limWet[2] { lim.l, lim.r };
                    dsp::process (e.limitBlend, limDry, limWet, y);
                    diff2 (kLimit, y, limDry);
                    break;
                }
                case kMb:
                {
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
                    break;
                }
                case kWidth:
                {
                    pack (bands, dry8);
                    dsp::process (e.width, bands);
                    pack (bands, wet8);
                    dsp::process (e.widthBlend, dry8, wet8, out8);
                    break;
                }
                case kSat:
                {
                    dsp::StereoBands ref;
                    dsp::process (e.sat, bands, ref);
                    pack (ref, dry8);
                    pack (bands, wet8);
                    dsp::process (e.satBlend, dry8, wet8, out8);
                    break;
                }
                case kDepthLayers:
                    break;
            }

            if (d >= 0)
            {
                float dl, dr;
                sum (out8, y[0], y[1]);
                sum (dry8, dl, dr);
                diff[static_cast<std::size_t> (k)] = { y[0] - dl, y[1] - dr };
                bands = unpack (out8);
            }
            tap (static_cast<int> (s) + 2, y[0], y[1]);
        }

        // ---- monitor: normal, one layer's delta, or the sum of all of them
        const float gN = step (e.normalGain), gG = step (e.globalGain), gS = step (e.stemsDeltaGain);
        float m[2] { gN * y[0], gN * y[1] };
        for (std::size_t k = 0; k < kDepthLayers; ++k)
        {
            const float gk = step (e.deltaGain[k]);
            for (std::size_t c = 0; c < 2; ++c)
                m[c] += (gG + gk) * dsp::process (e.deltaDelay[k][c], diff[k][c]);
        }
        if (stems)
            for (std::size_t c = 0; c < 2; ++c)
                m[c] += (gG + gS) * dsp::process (e.stemsDeltaDelay[c], stemsDiff[c]);

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

        // ---- delivery meters (what is printed: after CEILING, before the monitor-only gain match)
        for (std::size_t ch = 0; ch < 2; ++ch)
        {
            bm.outPeak[ch] = std::max (bm.outPeak[ch], std::abs (c[ch]));
            bm.outTruePeak = std::max (bm.outTruePeak, analysis::push (e.outTp[ch], c[ch]));   // NaN is ignored
        }
        analysis::push (e.outCorr, c[0], c[1]);
        {
            float gr = meterCeiling ? dsp::ceilingGainDb (e.ceiling, 0) : 0.0f;
            if (meterLimit)
                gr += std::min (dsp::limitGainDb (e.limit, 0), dsp::limitGainDb (e.limit, 1));
            bm.grDb = std::min (bm.grDb, gr);
        }

        // ---- gain match (monitor only: after everything that is printed)
        if (isSmoothing (e.gainMatchDb))
            e.matchGain = dbToGain (step (e.gainMatchDb));
        l[i] = c[0] * e.matchGain;
        r[i] = c[1] * e.matchGain;
    }

    analysis::endBlock (e.capture);
    flushDenormals (e);
}

} // namespace enginedetail

/** Audio thread. In place on l and r (stereo; mono hosts duplicate L). sidechainL/R may be
 *  null (no sidechain bus), in which case MULTIBAND uses its internal detector. `transport`
 *  is the host's timeline at the block's first sample (STEMS and its analysis pass need it;
 *  the default, not playing, bypasses STEMS). A block longer than maxBlock is processed in
 *  pieces, each at its own position. */
inline void process (Engine& e, float* l, float* r, int numSamples,
                     const float* sidechainL = nullptr, const float* sidechainR = nullptr,
                     const dsp::Transport& transport = {}) noexcept
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

    auto& bm = e.blockMeters;
    bm.inPeak = { 0.0f, 0.0f };
    bm.outPeak = { 0.0f, 0.0f };
    bm.outTruePeak = 0.0f;
    bm.grDb = 0.0f;
    const int piece = std::max (1, e.config.maxBlock);
    for (int at = 0; at < numSamples; at += piece)
    {
        auto t = transport;
        t.sample += at;
        processPiece (e, l + at, r + at, std::min (piece, numSamples - at),
                      sidechainL != nullptr ? sidechainL + at : nullptr,
                      sidechainR != nullptr ? sidechainR + at : nullptr, t);
    }
    bm.correlation = analysis::value (e.outCorr);

    for (int i = 0; i < numSamples && ! bad; ++i)
        bad = ! std::isfinite (l[i]) || ! std::isfinite (r[i]);
    if (bad)
    {
        std::fill (l, l + numSamples, 0.0f);
        std::fill (r, r + numSamples, 0.0f);
        // Meter what is printed: the muted block (an inf would otherwise hold the program max).
        bm.outPeak = { 0.0f, 0.0f };
        bm.outTruePeak = 0.0f;
        bm.grDb = 0.0f;
        resetNonFinite (e);
        e.nonFinite.store (true, std::memory_order_release);
    }
}

} // namespace strat3::engine
