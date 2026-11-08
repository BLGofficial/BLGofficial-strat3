#pragma once
// UI model: the pure logic under the native editor (module 19a). No JUCE GUI types, so it is
// tested headless and the components stay thin (paint + forward events here).
//
// Phase 4 layout, chosen by Blago 2026-11-04 over the ARCHON card-row reference:
//   header | layer stack (rows top to bottom = signal flow) | assistant column | delivery bar
//   Each row: label column (name, depth handle, on / delta) | impact strip (20 Hz..20 kHz) |
//   stats column (latency). Double-click expands the row's editor in place; the stack scrolls
//   vertically, never horizontally.
// Threads: message thread only (all of it is plain value code).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "analysis/ImpactAnalyser.h"
#include "engine/Engine.h"

namespace strat3::ui {

inline constexpr int kMinW = 900, kMinH = 600, kMaxW = 2560, kMaxH = 1600;   // Phase 4
inline constexpr int kAssistCollapseW = 1100, kAssistW = 300;                // Phase 4 breakpoint
inline constexpr int kHeaderH = 44, kDeliveryH = 64;
// 9 rows * 52 = 468 fits the 492 px stack of the minimum window, so nothing scrolls collapsed.
inline constexpr int kRowH = 52, kEditorH = 300, kLabelW = 220, kStatsW = 96;
inline constexpr int kDefaultW = 1280, kDefaultH = 800;
inline constexpr float kDepthMax = 150.0f;
inline constexpr float kDepthFine = 0.1f;   // Shift: drag speed and key step
// Below the floor a cell is blank so analyser noise does not paint the whole map; 6 dB of
// change is full intensity [INFERRED own tuning: a mastering move rarely exceeds it].
inline constexpr float kImpactFloorDb = 0.1f, kImpactFullScaleDb = 6.0f;
inline constexpr float kLufsFloor = -70.0f;   // matches the gain-match gate
inline constexpr float kMeterFloorDb = -100.0f;   // = processor::kMeterFloorDb
inline constexpr float kMeterFallDbPerSec = 20.0f;   // [INFERRED own tuning, near IEC PPM fall-back]
inline constexpr float kPeakHoldSec = 1.5f;
inline constexpr float kTrimMinDb = -24.0f, kTrimMaxDb = 24.0f;
inline constexpr double kFreqLoHz = 20.0, kFreqHiHz = 20000.0;
inline constexpr std::array<double, 10> kFreqTicks { 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 };

struct Rect
{
    int x = 0, y = 0, w = 0, h = 0;
    constexpr int right() const noexcept { return x + w; }
    constexpr int bottom() const noexcept { return y + h; }
};

struct Layout { int width = 0, height = 0; Rect header, stack, assist, delivery; bool assistDrawer = false; };
struct RowParts { Rect label, strip, stats, editor; };
struct BandSpan { double x0 = 0.0, x1 = 0.0; };
struct ImpactCell { float intensity = 0.0f; bool boost = false; };   // boost: filled; cut: outlined
enum class RowKind { InTrim, Layer, OutTrim, Ceiling };
struct RowInfo { RowKind kind = RowKind::Layer; engine::DepthLayer layer = engine::kDepthLayers; int slot = -1; int impactRow = 0; };

// ------------------------------------------------------------------------------ geometry

/** Window regions for a requested size, clamped to the Phase 4 range. `assistant` false
 *  (the WebView panels are not built yet) gives its column to the stack. */
inline Layout computeLayout (int w, int h, bool assistant = true)
{
    Layout l;
    l.width = std::clamp (w, kMinW, kMaxW);
    l.height = std::clamp (h, kMinH, kMaxH);
    l.assistDrawer = assistant && l.width < kAssistCollapseW;
    const int assistW = assistant && ! l.assistDrawer ? kAssistW : 0;
    const int middleH = l.height - kHeaderH - kDeliveryH;
    l.header = { 0, 0, l.width, kHeaderH };
    l.stack = { 0, kHeaderH, l.width - assistW, middleH };
    l.assist = { l.stack.right(), kHeaderH, assistW, middleH };
    l.delivery = { 0, l.stack.bottom(), l.width, kDeliveryH };
    return l;
}

/** Row rectangles in stack content coordinates; `expanded` (or -1) carries the editor. */
inline std::vector<Rect> stackRows (Rect stack, int rows, int expanded)
{
    std::vector<Rect> out;
    out.reserve (static_cast<std::size_t> (std::max (rows, 0)));
    int y = stack.y;
    for (int i = 0; i < rows; ++i)
    {
        const int h = kRowH + (i == expanded ? kEditorH : 0);
        out.push_back ({ stack.x, y, stack.w, h });
        y += h;
    }
    return out;
}

/** Total height of the rows (the viewport scrolls when it exceeds the stack). */
inline int contentHeight (const std::vector<Rect>& rows, Rect stack)
{
    return rows.empty() ? 0 : rows.back().bottom() - stack.y;
}

/** Label / strip / stats in the top kRowH of a row; the editor below when expanded. */
inline RowParts rowParts (Rect row)
{
    RowParts p;
    const int stripW = std::max (0, row.w - kLabelW - kStatsW);
    p.label = { row.x, row.y, kLabelW, kRowH };
    p.strip = { p.label.right(), row.y, stripW, kRowH };
    p.stats = { p.strip.right(), row.y, kStatsW, kRowH };
    p.editor = { row.x, row.y + kRowH, row.w, std::max (0, row.h - kRowH) };
    return p;
}

// ------------------------------------------------------------------------------ depth

inline float clampDepth (float d) noexcept { return std::clamp (d, 0.0f, kDepthMax); }

/** Horizontal drag of any linear value: the full track spans lo..hi; `fine` (Shift) moves
 *  1/10 as fast. dxPx is measured from where the drag started. */
inline float dragValue (float start, float dxPx, float trackPx, bool fine, float lo, float hi)
{
    if (! (trackPx > 0.0f) || ! std::isfinite (dxPx))
        return start;
    return std::clamp (start + dxPx / trackPx * (hi - lo) * (fine ? kDepthFine : 1.0f), lo, hi);
}

/** Key step of `coarse` (or `fineStep` with Shift), snapped to the fineStep grid so repeated
 *  steps do not drift. */
inline float stepValue (float v, int dir, bool fine, float coarse, float fineStep, float lo, float hi)
{
    const float step = (fine ? fineStep : coarse) * static_cast<float> (dir > 0 ? 1 : (dir < 0 ? -1 : 0));
    return std::clamp (std::round ((v + step) / fineStep) * fineStep, lo, hi);
}

/** Depth: the full track width spans 0..150 %. */
inline float depthFromDrag (float start, float dxPx, float trackPx, bool fine)
{
    return dragValue (start, dxPx, trackPx, fine, 0.0f, kDepthMax);
}

inline float depthToX (float depth, float trackPx) { return clampDepth (depth) / kDepthMax * trackPx; }
inline float xToDepth (float x, float trackPx) { return trackPx > 0.0f ? clampDepth (x / trackPx * kDepthMax) : 0.0f; }

/** Left / Right: 1 %, or 0.1 % with Shift. Rounded to 0.1 so repeated steps do not drift. */
inline float depthStep (float depth, int dir, bool fine)
{
    return stepValue (depth, dir, fine, 1.0f, kDepthFine, 0.0f, kDepthMax);
}

// ------------------------------------------------------------------------------ frequency axis

inline double freqToX (double hz, double widthPx)
{
    const double f = std::clamp (hz, kFreqLoHz, kFreqHiHz);
    return widthPx * std::log (f / kFreqLoHz) / std::log (kFreqHiHz / kFreqLoHz);
}

inline double xToFreq (double x, double widthPx)
{
    const double t = widthPx > 0.0 ? std::clamp (x / widthPx, 0.0, 1.0) : 0.0;
    return kFreqLoHz * std::pow (kFreqHiHz / kFreqLoHz, t);
}

/** Pixel span of a 1/6-octave impact band: edges at centre * 2^(+-1/12), clamped to the strip. */
inline BandSpan bandSpan (int band, double widthPx)
{
    const double c = analysis::impactBandCentreHz (band);
    const double edge = std::pow (2.0, 1.0 / 12.0);
    const bool first = band <= 0, last = band >= analysis::kImpactBands - 1;
    return { first ? 0.0 : freqToX (c / edge, widthPx), last ? widthPx : freqToX (c * edge, widthPx) };
}

// ------------------------------------------------------------------------------ impact cells

/** The change a cell shows: mid gain or side (width) change, whichever is larger. */
inline float impactValue (float gainDb, float widthDb)
{
    const float g = std::isfinite (gainDb) ? gainDb : 0.0f;
    const float w = std::isfinite (widthDb) ? widthDb : 0.0f;
    return std::abs (w) > std::abs (g) ? w : g;
}

inline ImpactCell impactCell (float db)
{
    if (! std::isfinite (db) || std::abs (db) < kImpactFloorDb)
        return {};
    return { std::min (std::abs (db) / kImpactFullScaleDb, 1.0f), db > 0.0f };
}

// ------------------------------------------------------------------------------ text

namespace detail {
inline std::string fixed (double v, int decimals)
{
    const double scale = std::pow (10.0, decimals);
    double r = std::round (v * scale) / scale;
    if (r == 0.0)
        r = 0.0;   // drops the sign of -0.0
    char buf[32];
    std::snprintf (buf, sizeof (buf), "%.*f", decimals, r);
    return buf;
}
} // namespace detail

inline std::string formatLufs (float lufs)
{
    if (! std::isfinite (lufs) || lufs < kLufsFloor)
        return "--.-";
    return detail::fixed (lufs, 1);
}

inline std::string formatDb (float db, int decimals, bool sign)
{
    if (! std::isfinite (db))
        return "--.-";
    std::string s = detail::fixed (db, decimals);
    if (sign && s.front() != '-' && std::stod (s) != 0.0)
        s.insert (s.begin(), '+');
    return s;
}

inline std::string formatHz (float hz)
{
    const double r = std::round (static_cast<double> (hz));
    if (r < 1000.0)
        return detail::fixed (r, 0);
    const double k = static_cast<double> (hz) / 1000.0;
    return detail::fixed (k, k < 9.995 ? 2 : 1) + "k";
}

inline std::string formatPercent (float pct) { return std::to_string (std::lround (pct)); }
inline std::string formatCpu (float load) { return detail::fixed (static_cast<double> (load) * 100.0, 1) + "%"; }

/** "0", "16smp" below 1 ms, else "5.0ms". */
inline std::string formatLatency (int samples, double sampleRate)
{
    if (samples <= 0 || ! (sampleRate > 0.0))
        return "0";
    const double ms = 1000.0 * samples / sampleRate;
    return ms < 1.0 ? std::to_string (samples) + "smp" : detail::fixed (ms, 1) + "ms";
}

// ------------------------------------------------------------------------------ meters

/** Bar meter ballistics: rises at once, falls at kMeterFallDbPerSec. */
inline float meterFall (float shownDb, float newDb, float dtSec)
{
    const float shown = std::isfinite (shownDb) ? shownDb : kMeterFloorDb;
    const float fallen = shown - kMeterFallDbPerSec * std::max (0.0f, dtSec);
    const float next = std::isfinite (newDb) ? std::max (newDb, fallen) : fallen;
    return std::max (next, kMeterFloorDb);
}

/** Numeric peak readout (true peak): holds the maximum for kPeakHoldSec. */
struct PeakHold { float db = kMeterFloorDb; float age = 0.0f; };

inline void update (PeakHold& h, float newDb, float dtSec)
{
    h.age += std::max (0.0f, dtSec);
    if (! std::isfinite (newDb))
        newDb = kMeterFloorDb;
    if (newDb >= h.db || h.age >= kPeakHoldSec)
    {
        h.db = newDb;
        h.age = 0.0f;
    }
}

/** ARCHON-style trim link: OUT TRIM moves opposite to IN TRIM, so the level after the chain's
 *  own gain stays where it was. */
inline float linkedOutTrim (float inNewDb, float inOldDb, float outOldDb)
{
    return std::clamp (outOldDb - (inNewDb - inOldDb), kTrimMinDb, kTrimMaxDb);
}

// ------------------------------------------------------------------------------ rows and order

inline const char* layerLabel (engine::DepthLayer k)
{
    static constexpr const char* names[] { "EQ", "LOW", "MULTIBAND", "WIDTH", "SATURATE", "LIMIT" };
    return k >= 0 && k < engine::kDepthLayers ? names[k] : "";
}

inline const char* layerSpokenName (engine::DepthLayer k)
{
    static constexpr const char* names[] { "EQ", "Low", "Multiband", "Width", "Saturate", "Limit" };
    return k >= 0 && k < engine::kDepthLayers ? names[k] : "";
}

/** Parameter ID prefix ("eq" -> "eq.depth") and the layer ID in EXTRA's layer order. */
inline const char* paramPrefix (engine::DepthLayer k)
{
    static constexpr const char* ids[] { "eq", "low", "mb", "width", "sat", "limit" };
    return k >= 0 && k < engine::kDepthLayers ? ids[k] : "";
}

/** Stack rows top to bottom: IN TRIM, the six layers in chain order, OUT TRIM, CEILING.
 *  Row index == impact-map row (engine rows are chain positions). */
inline std::vector<RowInfo> rowsFor (const engine::LayerOrder& order)
{
    std::vector<RowInfo> rows;
    rows.reserve (engine::kRows);
    rows.push_back ({ RowKind::InTrim, engine::kDepthLayers, -1, 0 });
    for (int s = 0; s < engine::kDepthLayers; ++s)
        rows.push_back ({ RowKind::Layer, order[static_cast<std::size_t> (s)], s, 1 + s });
    rows.push_back ({ RowKind::OutTrim, engine::kDepthLayers, -1, engine::kRowOutTrim });
    rows.push_back ({ RowKind::Ceiling, engine::kDepthLayers, -1, engine::kRowCeiling });
    return rows;
}

/** Drag reorder: take the layer at slot `from` and insert it at slot `to`. */
inline engine::LayerOrder moveLayer (const engine::LayerOrder& order, int from, int to)
{
    if (from < 0 || to < 0 || from >= engine::kDepthLayers || to >= engine::kDepthLayers || from == to)
        return order;
    std::vector<engine::DepthLayer> v (order.begin(), order.end());
    const auto k = v[static_cast<std::size_t> (from)];
    v.erase (v.begin() + from);
    v.insert (v.begin() + to, k);
    engine::LayerOrder out {};
    std::copy (v.begin(), v.end(), out.begin());
    return out;
}

/** IDs for Strat3Processor::setLayerOrder. STEMS is not built yet and stays at the head. */
inline std::vector<std::string> orderToIds (const engine::LayerOrder& order)
{
    std::vector<std::string> ids { "stems" };
    for (auto k : order)
        ids.emplace_back (paramPrefix (k));
    return ids;
}

/** Slot a dragged layer lands on for a pointer at `y` (content coordinates). IN TRIM and
 *  above map to the first slot, OUT TRIM / CEILING and below to the last: they never move. */
inline int dropSlot (const std::vector<Rect>& rows, int y)
{
    for (std::size_t r = 0; r < rows.size(); ++r)
        if (y < rows[r].bottom())
            return std::clamp (static_cast<int> (r) - 1, 0, engine::kDepthLayers - 1);
    return engine::kDepthLayers - 1;
}

/** Up / Down selection; -1 (nothing selected) + Down selects the first row. */
inline int nextRow (int selected, int dir, int rows)
{
    if (rows <= 0)
        return -1;
    if (selected < 0)
        return dir > 0 ? 0 : rows - 1;
    return std::clamp (selected + (dir > 0 ? 1 : (dir < 0 ? -1 : 0)), 0, rows - 1);
}

/** Screen-reader label, e.g. "Layer 3, Multiband, depth 72 percent" (Phase 4). */
inline std::string accessibleLayerLabel (int slot, engine::DepthLayer k, float depth, bool on)
{
    return "Layer " + std::to_string (slot + 1) + ", " + layerSpokenName (k) + (on ? "" : ", off")
           + ", depth " + formatPercent (depth) + " percent";
}

} // namespace strat3::ui
