#pragma once
// The assistant session in each situation the WebView panels have to draw. Every scenario
// is made by driving the real Session and rule engine with band energies shaped like
// programme material; no move, reason text or number here is written by hand.
//
// tests/assistant/test_panel_contract.cpp serialises them with the real stateToVar. The
// copies committed under webui/contract/states/ are what the page's own tests (Vitest) run
// against, so the page is tested against what the plugin really sends and the two sides
// cannot drift apart unnoticed.
#include <cmath>
#include <string>
#include <vector>

#include "assistant/Protocol.h"
#include "params/ParameterSpecs.h"

namespace strat3::test::panel {

using namespace strat3::assistant;

struct Scenario
{
    std::string name;   // file name under webui/contract/states/, without ".json"
    juce::var state;
};

struct Spectrum
{
    BandEnergies mid {}, side {};
};

/** A level change over a run of 1/6-octave bands (first..last, inclusive). */
struct Bump
{
    int first = 0, last = 0;
    double db = 0.0;
};

// Band 34 is 1 kHz; six bands per octave.
inline constexpr int kBand250 = 22, kBand4k = 46, kBand8k = 52;
inline constexpr double kBandEnergy = 2.0e-4;   // about -37 dB per band: a master near -19 dBFS RMS

/** The plugin's own defaults, with the delivery target set to the first platform
 *  (-14 LUFS): out of the box it is OFF and the loudness rule has nothing to aim for. */
inline ParamValues pluginDefaults()
{
    ParamValues p;
    for (const auto& spec : params::allSpecs())
        p[spec.id] = spec.def;
    p["global.target"] = 1.0f;
    return p;
}

/** Programme-like band energies: equal energy per band (pink), a roll-off under 40 Hz and
 *  a top tilt in dB per octave above 1 kHz. Side is 14 dB under mid, except below 120 Hz,
 *  where `lowSideDb` says how wide the bass is. */
inline Spectrum spectrum (double tiltDbPerOct, double lowSideDb, std::initializer_list<Bump> bumps = {})
{
    Spectrum s;
    for (int b = 0; b < kBands; ++b)
    {
        const double hz = analysis::impactBandCentreHz (b);
        double db = 0.0;
        if (hz < 40.0)
            db -= 12.0 * std::log2 (40.0 / hz);
        if (hz > 1000.0)
            db += tiltDbPerOct * std::log2 (hz / 1000.0);
        for (const auto& bump : bumps)
            if (b >= bump.first && b <= bump.last)
                db += bump.db;
        const auto i = static_cast<std::size_t> (b);
        s.mid[i] = kBandEnergy * std::pow (10.0, db / 10.0);
        s.side[i] = s.mid[i] * std::pow (10.0, (hz < 120.0 ? lowSideDb : -14.0) / 10.0);
    }
    return s;
}

inline double meanSquareFor (double lufs) { return std::pow (10.0, (lufs + 0.691) / 10.0); }

/** Feeds `frames` more analysis frames (30 per second) and the matching 100 ms loudness
 *  blocks into the running capture. `frame (index)` gives the spectrum of one frame. */
template <typename FrameFn>
void feed (Session& s, int frames, double lufs, FrameFn&& frame)
{
    const int start = static_cast<int> (s.builder.mid.size());
    for (int f = start; f < start + frames; ++f)
    {
        if (f % kFramesPerBlock == 0)
            addLoudnessBlock (s, meanSquareFor (lufs));
        const Spectrum sp = frame (f);
        addFrame (s, sp.mid, sp.side, 0.82f);
    }
}

/** A whole capture, start to result. */
template <typename FrameFn>
void capture (Session& s, CaptureKind kind, double seconds, double lufs, const ParamValues& p, FrameFn&& frame)
{
    if (! startCapture (s, kind, seconds))
        return;
    feed (s, static_cast<int> (std::lround (seconds * kSnapshotFrameHz)), lufs, frame);
    completeCapture (s, p);
}

/** What the processor does with a returned ParamSet: the values become the current ones. */
inline void setValues (ParamValues& p, const ParamSet& set)
{
    for (const auto& [id, value] : set)
        p[id] = value;
}

inline int idOf (const Session& s, const std::string& layer, int nth = 0)
{
    for (const auto& m : s.log)
        if (m.move.layer == layer && nth-- == 0)
            return m.move.id;
    return 0;
}

/** Every panel situation, in the order a user would meet them. */
inline std::vector<Scenario> scenarios()
{
    const auto p = pluginDefaults();
    const auto steady = [] (const Spectrum& sp) { return [sp] (int) { return sp; }; };

    // Two reference masters with tight bass and slightly different top ends.
    const auto refA = spectrum (-1.2, -30.0);
    const auto refB = spectrum (-1.8, -28.0);

    // The mix under analysis: mud at 250 Hz, a hole at 4 kHz, wide bass, 3 LU too quiet, and
    // an 8 kHz band that is 12 dB hotter in one 100 ms block out of five (sibilance), which
    // the tonal rule answers with a dynamic dip instead of a static cut.
    const auto mixQuiet = spectrum (-1.5, -7.0, { { kBand250 - 2, kBand250 + 2, 4.5 }, { kBand4k - 2, kBand4k + 2, -4.0 }, { kBand8k - 1, kBand8k + 1, -2.0 } });
    const auto mixLoud = spectrum (-1.5, -7.0, { { kBand250 - 2, kBand250 + 2, 4.5 }, { kBand4k - 2, kBand4k + 2, -4.0 }, { kBand8k - 1, kBand8k + 1, 10.0 } });
    const auto mix = [=] (int f) { return (f / kFramesPerBlock) % 5 == 0 ? mixLoud : mixQuiet; };

    // The same mix heard through the decisions below: the 250 Hz cut took 3 dB off the mud,
    // the 4 kHz boost at 60 % filled 1.7 dB of the hole, the limiter move made up the level.
    // The bass is still wide and the sibilance is still there (neither move was accepted).
    const auto laterQuiet = spectrum (-1.5, -7.0, { { kBand250 - 2, kBand250 + 2, 1.5 }, { kBand4k - 2, kBand4k + 2, -2.3 }, { kBand8k - 1, kBand8k + 1, -2.0 } });
    const auto laterLoud = spectrum (-1.5, -7.0, { { kBand250 - 2, kBand250 + 2, 1.5 }, { kBand4k - 2, kBand4k + 2, -2.3 }, { kBand8k - 1, kBand8k + 1, 10.0 } });
    const auto mixLater = [=] (int f) { return (f / kFramesPerBlock) % 5 == 0 ? laterLoud : laterQuiet; };

    Session idle;
    prepare (idle);

    auto oneRef = idle;
    capture (oneRef, CaptureKind::Reference, 30.0, -9.6, p, steady (refA));
    auto twoRefs = oneRef;
    capture (twoRefs, CaptureKind::Reference, 60.0, -8.9, p, steady (refB));

    auto refRunning = oneRef;
    startCapture (refRunning, CaptureKind::Reference, 60.0);
    feed (refRunning, 1260, -8.9, steady (refB));   // 42 of 60 s

    auto full = idle;
    for (int n = 0; n < kMaxReferences; ++n)
        capture (full, CaptureKind::Reference, 30.0, -9.0 - 0.1 * n, p, steady (spectrum (-1.0 - 0.1 * n, -30.0)));

    auto analysing = twoRefs;
    startCapture (analysing, CaptureKind::Analyse, 30.0);
    feed (analysing, 372, -17.2, mix);   // 12.4 of 30 s

    auto pending = twoRefs;
    capture (pending, CaptureKind::Analyse, 30.0, -17.2, p, mix);

    // Decisions: first EQ move as proposed, second at 60 %, the bass move turned down.
    auto decided = pending;
    auto pDecided = p;
    setValues (pDecided, accept (decided, idOf (decided, "eq", 0), 1.0f));
    setValues (pDecided, accept (decided, idOf (decided, "eq", 1), 0.6f));
    setValues (pDecided, reject (decided, idOf (decided, "low")));
    setValues (pDecided, accept (decided, idOf (decided, "limit"), 1.0f));

    // A second analysis: the decided moves stay as locked history above the new proposals.
    auto history = decided;
    capture (history, CaptureKind::Analyse, 60.0, -14.3, pDecided, mixLater);

    // Everything applied as proposed except the sibilance dip, which was turned down; then
    // analysed again. The five moves stay as locked history, among them the bass move
    // (switches only, so it has no depth to show) and the rejected dip. Tone and bass now
    // match the target. The sibilance is still there, so the dip is proposed again. The
    // limiter's open-loop estimate came out 1.2 LU short, so the other new proposal is a
    // single threshold change, the limiter being on already.
    auto applied = pending;
    auto pApplied = p;
    setValues (pApplied, reject (applied, idOf (applied, "eq", 2)));
    setValues (pApplied, applyAll (applied));
    const auto appliedQuiet = spectrum (-1.5, -30.0, { { kBand8k - 1, kBand8k + 1, -2.0 } });
    const auto appliedLoud = spectrum (-1.5, -30.0, { { kBand8k - 1, kBand8k + 1, 10.0 } });
    capture (applied, CaptureKind::Analyse, 30.0, -15.2, pApplied, [=] (int f) { return (f / kFramesPerBlock) % 5 == 0 ? appliedLoud : appliedQuiet; });

    auto noTarget = idle;
    capture (noTarget, CaptureKind::Analyse, 30.0, -17.2, p, mix);

    // A master that already matches the target: nothing to propose.
    auto noMoves = twoRefs;
    capture (noMoves, CaptureKind::Analyse, 30.0, -14.2, p, steady (spectrum (-1.5, -30.0)));

    // Transport stopped at the source: the frames carry no signal.
    auto silent = pending;
    startCapture (silent, CaptureKind::Analyse, 30.0);
    for (int f = 0; f < 900; ++f)
    {
        BandEnergies none;
        none.fill (1.0e-14);
        addFrame (silent, none, none, 0.0f);
    }
    completeCapture (silent, p);

    auto cancelled = twoRefs;
    startCapture (cancelled, CaptureKind::Analyse, 60.0);
    feed (cancelled, 200, -17.2, mix);
    cancelCapture (cancelled);

    return {
        { "idle", stateToVar (idle) },
        { "reference-running", stateToVar (refRunning) },
        { "reference-added", stateToVar (oneRef) },
        { "references-full", stateToVar (full) },
        { "analyse-running", stateToVar (analysing) },
        { "analyse-paused", stateToVar (analysing, true) },
        { "moves-pending", stateToVar (pending) },
        { "moves-decided", stateToVar (decided) },
        { "moves-history", stateToVar (history) },
        { "moves-applied", stateToVar (applied) },
        { "moves-no-target", stateToVar (noTarget) },
        { "no-moves", stateToVar (noMoves) },
        { "silent", stateToVar (silent) },
        { "cancelled", stateToVar (cancelled) },
    };
}

} // namespace strat3::test::panel
