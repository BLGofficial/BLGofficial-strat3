#pragma once
// EQ editor model (module 20a): the pure logic under the EQ row's spectrum canvas and band
// editor. No JUCE GUI types, so it is tested headless; EqEditor paints and forwards events here.
//
// Interaction (Phase 4: "EQ nodes on the spectrum", ARCHON band editor below):
//   drag a node = freq (x, log) + gain (y); wheel = Q; double-click empty = enable the first
//   free band there; keys nudge the selected band. Shift = fine everywhere.
// The curve comes from dsp::settingsMagnitudeDb, the same function the DSP and the FIR design
// use, so the picture cannot drift from what is heard.
// Threads: message thread only (plain value code).
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "dsp/eq/EqResponse.h"
#include "ui/model/UiModel.h"

namespace strat3::ui {

// Parameter ranges (params/ParameterSpecs: eq.bN.freq / gain / q). Kept here so the model can
// clamp without the APVTS; test_parameters guards the specs themselves.
inline constexpr float kEqFreqMin = 20.0f, kEqFreqMax = 20000.0f;
inline constexpr float kEqGainMin = -18.0f, kEqGainMax = 18.0f;
inline constexpr float kEqQMin = 0.1f, kEqQMax = 18.0f;
// Grab radius around a node centre [INFERRED own tuning: about a fingertip at 100 % scale].
inline constexpr float kEqNodeHitPx = 10.0f;
// Auto range steps. A fixed +-18 would squash typical 0.5..2 dB mastering moves into a few
// pixels; a zoom button was ruled out (no controls beyond what is needed), so the range fits.
inline constexpr float kEqRanges[] { 6.0f, 12.0f, 18.0f };

/** Canvas size in px and the gain shown at the top edge (bottom = -rangeDb). */
struct EqPlot { float w = 0.0f, h = 0.0f, rangeDb = 6.0f; };
struct EqPoint { float x = 0.0f, y = 0.0f; };

inline float eqGainToY (float db, EqPlot p)
{
    const float d = std::isfinite (db) ? std::clamp (db, -p.rangeDb, p.rangeDb) : 0.0f;
    return 0.5f * p.h * (1.0f - d / p.rangeDb);
}

inline float eqYToGain (float y, EqPlot p)
{
    if (! (p.h > 0.0f) || ! std::isfinite (y))
        return 0.0f;
    const float t = std::clamp (y / p.h, 0.0f, 1.0f);
    return p.rangeDb * (1.0f - 2.0f * t);
}

/** Smallest range that holds every active band's gain. Pass filters are ignored: their gain
 *  parameter does nothing, and a stale value must not zoom the view out. */
inline float eqAutoRange (const dsp::EqSettings& s)
{
    float m = 0.0f;
    for (const auto& b : s)
        if (b.on && dsp::supportsGain (b.type) && std::isfinite (b.gainDb))
            m = std::max (m, std::abs (b.gainDb));
    for (float r : kEqRanges)
        if (m <= r)
            return r;
    return kEqRanges[std::size (kEqRanges) - 1];
}

inline EqPoint eqNodePos (const dsp::EqBand& b, EqPlot p)
{
    return { static_cast<float> (freqToX (b.freq, p.w)),
             eqGainToY (dsp::supportsGain (b.type) ? b.gainDb : 0.0f, p) };
}

/** Band under the pointer or -1. The selected band wins so a node stacked under another can
 *  still be dragged once picked from the band editor; otherwise the higher band (drawn last,
 *  so on top) wins. */
inline int eqHitTest (const dsp::EqSettings& s, EqPlot p, float x, float y, int selected)
{
    const auto inside = [&] (int i)
    {
        const auto& b = s[static_cast<std::size_t> (i)];
        if (! b.on)
            return false;
        const auto n = eqNodePos (b, p);
        return std::hypot (n.x - x, n.y - y) <= kEqNodeHitPx;
    };
    if (selected >= 0 && selected < dsp::kEqBands && inside (selected))
        return selected;
    for (int i = dsp::kEqBands - 1; i >= 0; --i)
        if (inside (i))
            return i;
    return -1;
}

/** Node drag; dx / dy are measured from where the drag started (no accumulation drift).
 *  Freq moves in the log domain (same px per octave everywhere), clamped before exp so a huge
 *  dx cannot overflow. */
inline dsp::EqBand eqDragNode (const dsp::EqBand& start, EqPlot p, float dx, float dy, bool fine)
{
    if (! std::isfinite (dx) || ! std::isfinite (dy))
        return start;
    const float speed = fine ? 0.1f : 1.0f;
    dsp::EqBand b = start;
    if (p.w > 0.0f)
    {
        const double span = std::log (kFreqHiHz / kFreqLoHz);
        const double lf = std::log (static_cast<double> (start.freq)) + static_cast<double> (dx * speed / p.w) * span;
        b.freq = static_cast<float> (std::exp (std::clamp (lf, std::log (static_cast<double> (kEqFreqMin)),
                                                           std::log (static_cast<double> (kEqFreqMax)))));
        b.freq = std::clamp (b.freq, kEqFreqMin, kEqFreqMax);   // exp(log(x)) may land 1 ulp outside
    }
    if (p.h > 0.0f && dsp::supportsGain (start.type))
        b.gainDb = std::clamp (start.gainDb - dy * speed * 2.0f * p.rangeDb / p.h, kEqGainMin, kEqGainMax);
    return b;
}

/** One wheel notch (delta 1) doubles or halves Q; Shift = a tenth of that. */
inline float eqWheelQ (float q, float delta, bool fine)
{
    if (! std::isfinite (delta) || ! std::isfinite (q))
        return q;
    return std::clamp (q * std::pow (2.0f, delta * (fine ? 0.1f : 1.0f)), kEqQMin, kEqQMax);
}

inline int eqFirstFreeBand (const dsp::EqSettings& s)
{
    for (int i = 0; i < dsp::kEqBands; ++i)
        if (! s[static_cast<std::size_t> (i)].on)
            return i;
    return -1;
}

/** Double-click on empty canvas: the band turns on as a bell at the pointer. Q, channel and
 *  dynamics keep their stored values so re-enabling a band restores its character. */
inline dsp::EqBand eqPlaceBand (const dsp::EqBand& current, EqPlot p, float x, float y)
{
    dsp::EqBand b = current;
    b.on = true;
    b.type = dsp::EqType::Bell;
    b.freq = std::clamp (static_cast<float> (xToFreq (x, p.w)), kEqFreqMin, kEqFreqMax);
    b.gainDb = std::clamp (eqYToGain (y, p), kEqGainMin, kEqGainMax);
    return b;
}

/** Left / Right: a semitone (1/12 octave), Shift a quarter of that. */
inline float eqNudgeFreq (float hz, int dir, bool fine)
{
    const float d = static_cast<float> (dir > 0 ? 1 : (dir < 0 ? -1 : 0));
    return std::clamp (hz * std::pow (2.0f, d / (fine ? 48.0f : 12.0f)), kEqFreqMin, kEqFreqMax);
}

/** Up / Down: 0.5 dB, Shift 0.1 dB, on the 0.1 grid. */
inline float eqNudgeGain (float db, int dir, bool fine)
{
    return stepValue (db, dir, fine, 0.5f, 0.1f, kEqGainMin, kEqGainMax);
}

/** `points` samples spread evenly over the log axis, first = 20 Hz, last = 20 kHz. */
inline std::vector<double> eqCurveDb (const dsp::EqSettings& s, int points, double sampleRate, dsp::EqChannel view)
{
    std::vector<double> out;
    if (points < 2)
        return out;
    out.reserve (static_cast<std::size_t> (points));
    for (int i = 0; i < points; ++i)
        out.push_back (dsp::settingsMagnitudeDb (s, xToFreq (i, points - 1.0), sampleRate, view));
    return out;
}

// Backdrop scale for the EQ input spectrum (dB per 1/6-octave band, ImpactFrame::levelDb).
// A loud master puts roughly -20..-40 dB in each band (its total spread over ~60 bands);
// -12..-96 keeps that in the upper half with quiet tails visible [INFERRED own tuning].
inline constexpr float kEqSpectrumTopDb = -12.0f, kEqSpectrumBottomDb = -96.0f;

inline float eqSpectrumToY (float db, EqPlot p)
{
    const float d = std::isfinite (db) ? std::clamp (db, kEqSpectrumBottomDb, kEqSpectrumTopDb) : kEqSpectrumBottomDb;
    return p.h * (kEqSpectrumTopDb - d) / (kEqSpectrumTopDb - kEqSpectrumBottomDb);
}

/** 0-based band, 1-based ID: eqParamId (2, "freq") == "eq.b3.freq". */
inline std::string eqParamId (int band, const char* name)
{
    return "eq.b" + std::to_string (band + 1) + "." + name;
}

inline const char* eqTypeName (dsp::EqType t)
{
    static constexpr const char* names[] { "Bell", "Low Shelf", "High Shelf", "High Pass", "Low Pass", "Passive Shelf" };
    const auto i = static_cast<int> (t);
    return i >= 0 && i < static_cast<int> (std::size (names)) ? names[i] : "";
}

/** Band editor value box text by parameter suffix ("freq", "gain", ...). Empty for a name it
 *  does not know, so the caller keeps the slider's default. */
inline std::string eqValueText (const std::string& name, float v)
{
    if (name == "freq")
        return formatHz (v) + " Hz";
    if (name == "gain" || name == "thresh" || name == "range")
        return formatDb (v, 1, name == "gain") + " dB";
    if (name == "q")
        return detail::fixed (v, 2);
    if (name == "atk" || name == "rel")
        return detail::fixed (v, std::abs (v) < 99.95f ? 1 : 0) + " ms";
    if (name == "phase")
        return formatPercent (v) + " %";
    return {};
}

/** Frequency axis labels: "20", "500", "1k", "20k". */
inline std::string eqTickText (double hz)
{
    return hz < 1000.0 ? detail::fixed (hz, 0) : detail::fixed (hz / 1000.0, 0) + "k";
}

/** Screen-reader label, e.g. "EQ band 3, Bell, 1.00k hertz, +2.0 dB, Q 0.71". */
inline std::string eqAccessibleLabel (int band, const dsp::EqBand& b)
{
    std::string s = "EQ band " + std::to_string (band + 1);
    if (! b.on)
        return s + ", off";
    s += std::string (", ") + eqTypeName (b.type) + ", " + formatHz (b.freq) + " hertz";
    if (dsp::supportsGain (b.type))
        s += ", " + formatDb (b.gainDb, 1, true) + " dB";
    return s + ", Q " + detail::fixed (b.q, 2);
}

} // namespace strat3::ui
