// UI model (module 19a): the pure logic under the native editor, tested without painting.
// Phase 4 layout chosen by Blago 2026-11-04 (vertical stack, impact map, delivery bar; ARCHON
// reference used only for the band editor, clip LEDs, linked trims and the loudness box).
// Contracts tested here:
//   - the window clamps to 900x600 .. 2560x1600 and tiles exactly into header / stack /
//     assistant column / delivery bar; below 1100 px the assistant becomes a drawer
//   - stack rows are contiguous, fixed height, and the expanded row pushes the rows below
//   - a depth drag maps pixels to 0..150 % (Shift = 1/10 speed), keys step 1 % / 0.1 %
//   - the frequency axis is log 20 Hz .. 20 kHz and the 61 impact bands tile it
//   - impact cells: magnitude -> intensity, sign -> fill (boost) or outline (cut)
//   - numbers have fixed decimals, no "-0.0", and a placeholder below the meter floor
//   - rows follow the engine order; reordering is a move between layer slots only
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

#include "processor/Strat3Processor.h"
#include "ui/model/UiModel.h"

using namespace strat3::ui;
namespace engine = strat3::engine;
using Catch::Matchers::WithinAbs;

namespace {

bool tiles (const Layout& l, int w, int h)
{
    const bool cols = l.stack.x == 0 && l.stack.w + l.assist.w == w && l.assist.x == l.stack.right();
    const bool header = l.header.x == 0 && l.header.y == 0 && l.header.w == w;
    const bool middle = l.stack.y == l.header.bottom() && l.assist.y == l.stack.y && l.assist.h == l.stack.h;
    const bool bottom = l.delivery.x == 0 && l.delivery.w == w && l.delivery.y == l.stack.bottom()
                        && l.delivery.bottom() == h;
    return cols && header && middle && bottom;
}

} // namespace

TEST_CASE ("layout tiles the window and clamps to the Phase 4 size range", "[ui][model]")
{
    for (auto [w, h] : { std::pair { 1400, 900 }, std::pair { 900, 600 }, std::pair { 2560, 1600 }, std::pair { 1100, 700 } })
    {
        const auto l = computeLayout (w, h);
        CHECK (l.width == w);
        CHECK (l.height == h);
        CHECK (tiles (l, w, h));
        CHECK (l.header.h == kHeaderH);
        CHECK (l.delivery.h == kDeliveryH);
    }

    const auto small = computeLayout (300, 200);
    CHECK (small.width == kMinW);
    CHECK (small.height == kMinH);
    CHECK (tiles (small, kMinW, kMinH));

    const auto big = computeLayout (5000, 4000);
    CHECK (big.width == kMaxW);
    CHECK (big.height == kMaxH);
}

TEST_CASE ("assistant column collapses to a drawer below 1100 px", "[ui][model]")
{
    const auto wide = computeLayout (1100, 700);
    CHECK_FALSE (wide.assistDrawer);
    CHECK (wide.assist.w == kAssistW);

    const auto narrow = computeLayout (1099, 700);
    CHECK (narrow.assistDrawer);
    CHECK (narrow.assist.w == 0);
    CHECK (narrow.stack.w == 1099);
}

TEST_CASE ("stack rows are contiguous and the expanded row pushes the rest down", "[ui][model]")
{
    const Rect stack { 0, 44, 1100, 500 };
    const auto rows = stackRows (stack, 9, -1);
    REQUIRE (rows.size() == 9);
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        CHECK (rows[i].x == stack.x);
        CHECK (rows[i].w == stack.w);
        CHECK (rows[i].h == kRowH);
        CHECK (rows[i].y == (i == 0 ? stack.y : rows[i - 1].bottom()));
    }
    CHECK (contentHeight (rows, stack) == 9 * kRowH);

    const auto open = stackRows (stack, 9, 2);
    CHECK (open[2].h == kRowH + kEditorH);
    CHECK (open[3].y == open[2].bottom());
    CHECK (open[1].y == rows[1].y);
    CHECK (contentHeight (open, stack) == 9 * kRowH + kEditorH);

    CHECK (stackRows (stack, 9, 9)[8].h == kRowH);   // out of range: nothing expanded
}

TEST_CASE ("row parts: label column, impact strip, stats column", "[ui][model]")
{
    const Rect row { 0, 100, 1100, kRowH };
    const auto p = rowParts (row);
    CHECK (p.label.x == 0);
    CHECK (p.label.w == kLabelW);
    CHECK (p.strip.x == p.label.right());
    CHECK (p.stats.x == p.strip.right());
    CHECK (p.stats.right() == row.right());
    CHECK (p.stats.w == kStatsW);
    CHECK (p.strip.w == 1100 - kLabelW - kStatsW);
    for (const auto& r : { p.label, p.strip, p.stats })
    {
        CHECK (r.y == row.y);
        CHECK (r.h == kRowH);
    }

    // expanded row: the parts stay in the top kRowH, the editor takes the rest
    const Rect open { 0, 100, 1100, kRowH + kEditorH };
    const auto q = rowParts (open);
    CHECK (q.strip.h == kRowH);
    CHECK (q.editor.y == open.y + kRowH);
    CHECK (q.editor.h == kEditorH);
    CHECK (q.editor.w == open.w);
    CHECK (p.editor.h == 0);
}

TEST_CASE ("depth drag maps the track width to 0..150 % and clamps", "[ui][model]")
{
    CHECK_THAT (depthFromDrag (100.0f, 0.0f, 300.0f, false), WithinAbs (100.0, 1e-4));
    CHECK_THAT (depthFromDrag (0.0f, 300.0f, 300.0f, false), WithinAbs (150.0, 1e-4));
    CHECK_THAT (depthFromDrag (100.0f, -100.0f, 300.0f, false), WithinAbs (50.0, 1e-4));
    CHECK_THAT (depthFromDrag (100.0f, -100.0f, 300.0f, true), WithinAbs (95.0, 1e-4));   // Shift: 1/10
    CHECK_THAT (depthFromDrag (140.0f, 500.0f, 300.0f, false), WithinAbs (150.0, 1e-4));
    CHECK_THAT (depthFromDrag (10.0f, -500.0f, 300.0f, false), WithinAbs (0.0, 1e-4));
    CHECK_THAT (depthFromDrag (70.0f, 50.0f, 0.0f, false), WithinAbs (70.0, 1e-4));       // no track: no change
    CHECK_THAT (depthFromDrag (70.0f, std::numeric_limits<float>::quiet_NaN(), 300.0f, false), WithinAbs (70.0, 1e-4));

    for (float d : { 0.0f, 37.5f, 100.0f, 150.0f })
        CHECK_THAT (xToDepth (depthToX (d, 240.0f), 240.0f), WithinAbs (d, 1e-3));
    CHECK_THAT (xToDepth (-10.0f, 240.0f), WithinAbs (0.0, 1e-4));
    CHECK_THAT (xToDepth (999.0f, 240.0f), WithinAbs (150.0, 1e-4));
}

TEST_CASE ("depth keys step 1 % or 0.1 % without drift", "[ui][model]")
{
    CHECK_THAT (depthStep (72.0f, +1, false), WithinAbs (73.0, 1e-5));
    CHECK_THAT (depthStep (72.0f, -1, true), WithinAbs (71.9, 1e-5));
    CHECK_THAT (depthStep (150.0f, +1, false), WithinAbs (150.0, 1e-5));
    CHECK_THAT (depthStep (0.05f, -1, true), WithinAbs (0.0, 1e-5));

    float d = 0.0f;
    for (int i = 0; i < 1000; ++i)
        d = depthStep (d, +1, true);
    CHECK_THAT (d, WithinAbs (100.0, 1e-4));   // rounded to 0.1 each step: no accumulated error
}

TEST_CASE ("frequency axis is log 20 Hz .. 20 kHz and invertible", "[ui][model]")
{
    CHECK_THAT (freqToX (20.0, 600.0), WithinAbs (0.0, 1e-9));
    CHECK_THAT (freqToX (20000.0, 600.0), WithinAbs (600.0, 1e-9));
    CHECK_THAT (freqToX (632.455532, 600.0), WithinAbs (300.0, 1e-4));   // geometric centre
    CHECK_THAT (freqToX (5.0, 600.0), WithinAbs (0.0, 1e-9));
    CHECK_THAT (freqToX (96000.0, 600.0), WithinAbs (600.0, 1e-9));
    for (double hz : { 20.0, 100.0, 1000.0, 3200.0, 20000.0 })
        CHECK_THAT (xToFreq (freqToX (hz, 777.0), 777.0), WithinAbs (hz, hz * 1e-9));

    REQUIRE (kFreqTicks.front() == 20.0);
    REQUIRE (kFreqTicks.back() == 20000.0);
    CHECK (std::is_sorted (kFreqTicks.begin(), kFreqTicks.end()));
}

TEST_CASE ("impact bands tile the strip without gaps", "[ui][model]")
{
    const double w = 640.0;
    CHECK_THAT (bandSpan (0, w).x0, WithinAbs (0.0, 1e-9));
    CHECK_THAT (bandSpan (strat3::analysis::kImpactBands - 1, w).x1, WithinAbs (w, 1e-9));
    for (int b = 0; b + 1 < strat3::analysis::kImpactBands; ++b)
    {
        const auto s = bandSpan (b, w);
        CHECK (s.x1 > s.x0);
        CHECK_THAT (s.x1, WithinAbs (bandSpan (b + 1, w).x0, 1e-9));
    }
    // band 34 is centred on 1 kHz
    const auto k = bandSpan (34, w);
    CHECK_THAT (0.5 * (k.x0 + k.x1), WithinAbs (freqToX (1000.0, w), 0.5));
}

TEST_CASE ("impact cells: magnitude sets intensity, sign sets fill or outline", "[ui][model]")
{
    CHECK (impactCell (0.0f).intensity == 0.0f);
    CHECK (impactCell (0.5f * kImpactFloorDb).intensity == 0.0f);
    CHECK_THAT (impactCell (0.5f * kImpactFullScaleDb).intensity, WithinAbs (0.5, 1e-5));
    CHECK (impactCell (3.0f * kImpactFullScaleDb).intensity == 1.0f);
    CHECK (impactCell (2.0f).boost);
    CHECK_FALSE (impactCell (-2.0f).boost);
    CHECK (impactCell (-2.0f).intensity == impactCell (2.0f).intensity);
    CHECK (impactCell (std::numeric_limits<float>::quiet_NaN()).intensity == 0.0f);
    CHECK (impactCell (std::numeric_limits<float>::infinity()).intensity == 0.0f);

    // WIDTH changes the side channel: the cell shows whichever change is larger
    CHECK (impactValue (0.2f, -3.0f) == -3.0f);
    CHECK (impactValue (1.5f, 0.4f) == 1.5f);
    CHECK (impactValue (std::numeric_limits<float>::quiet_NaN(), 1.0f) == 1.0f);
}

TEST_CASE ("number formatting: fixed decimals, no negative zero, floor placeholder", "[ui][model]")
{
    CHECK (formatLufs (-9.83f) == "-9.8");
    CHECK (formatLufs (-14.0f) == "-14.0");
    CHECK (formatLufs (-70.5f) == "--.-");
    CHECK (formatLufs (-100.0f) == "--.-");
    CHECK (formatLufs (std::numeric_limits<float>::quiet_NaN()) == "--.-");

    CHECK (formatDb (1.5f, 1, true) == "+1.5");
    CHECK (formatDb (-1.5f, 1, true) == "-1.5");
    CHECK (formatDb (-0.04f, 1, true) == "0.0");
    CHECK (formatDb (0.0f, 1, true) == "0.0");
    CHECK (formatDb (-0.04f, 1, false) == "0.0");
    CHECK (formatDb (-4.25f, 2, false) == "-4.25");
    CHECK (formatDb (std::numeric_limits<float>::quiet_NaN(), 1, true) == "--.-");

    CHECK (formatHz (240.0f) == "240");
    CHECK (formatHz (3200.0f) == "3.20k");
    CHECK (formatHz (12000.0f) == "12.0k");
    CHECK (formatHz (20000.0f) == "20.0k");
    CHECK (formatHz (999.6f) == "1.00k");
    CHECK (formatHz (31.4f) == "31");

    CHECK (formatPercent (85.4f) == "85");
    CHECK (formatPercent (72.5f) == "73");
    CHECK (formatCpu (0.16f) == "16.0%");
    CHECK (formatCpu (0.0f) == "0.0%");

    CHECK (formatLatency (0, 48000.0) == "0");
    CHECK (formatLatency (16, 48000.0) == "16smp");
    CHECK (formatLatency (240, 48000.0) == "5.0ms");
    CHECK (formatLatency (4608, 48000.0) == "96.0ms");
}

TEST_CASE ("rows follow the engine order with trims and ceiling fixed", "[ui][model]")
{
    const engine::LayerOrder order { engine::kLimit, engine::kEq, engine::kLow, engine::kMb, engine::kWidth, engine::kSat };
    const auto rows = rowsFor (order);
    REQUIRE (rows.size() == static_cast<std::size_t> (engine::kRows));
    CHECK (rows.front().kind == RowKind::InTrim);
    CHECK (rows[7].kind == RowKind::OutTrim);
    CHECK (rows.back().kind == RowKind::Ceiling);
    for (int r = 0; r < engine::kRows; ++r)
        CHECK (rows[static_cast<std::size_t> (r)].impactRow == r);
    for (int s = 0; s < engine::kDepthLayers; ++s)
    {
        const auto& row = rows[static_cast<std::size_t> (1 + s)];
        CHECK (row.kind == RowKind::Layer);
        CHECK (row.layer == order[static_cast<std::size_t> (s)]);
        CHECK (row.slot == s);
    }

    CHECK (layerLabel (engine::kMb) == std::string ("MULTIBAND"));
    CHECK (layerLabel (engine::kSat) == std::string ("SATURATE"));
    CHECK (paramPrefix (engine::kEq) == std::string ("eq"));
    CHECK (paramPrefix (engine::kLimit) == std::string ("limit"));
    std::set<std::string> prefixes;
    for (int k = 0; k < engine::kDepthLayers; ++k)
        prefixes.insert (paramPrefix (static_cast<engine::DepthLayer> (k)));
    CHECK (prefixes.size() == static_cast<std::size_t> (engine::kDepthLayers));
}

TEST_CASE ("reorder moves a layer between slots and round-trips through the processor ids", "[ui][model]")
{
    const auto d = engine::kDefaultOrder;   // EQ LOW MB WIDTH SAT LIMIT

    const auto down = moveLayer (d, 0, 5);
    CHECK (down == engine::LayerOrder { engine::kLow, engine::kMb, engine::kWidth, engine::kSat, engine::kLimit, engine::kEq });
    const auto up = moveLayer (d, 5, 1);
    CHECK (up == engine::LayerOrder { engine::kEq, engine::kLimit, engine::kLow, engine::kMb, engine::kWidth, engine::kSat });
    CHECK (moveLayer (d, 2, 2) == d);
    CHECK (moveLayer (d, -1, 3) == d);
    CHECK (moveLayer (d, 0, 6) == d);
    CHECK (engine::isValidOrder (moveLayer (d, 3, 0)));

    for (const auto& o : { d, down, up })
    {
        const auto ids = orderToIds (o);
        REQUIRE (ids.size() == 7);               // stems keeps its place at the head
        CHECK (ids.front() == "stems");
        CHECK (strat3::processor::engineOrder (ids) == o);
    }
}

TEST_CASE ("drop slot: trims and ceiling are locked, drops land on the nearest layer slot", "[ui][model]")
{
    const Rect stack { 0, 44, 1100, 500 };
    const auto rows = stackRows (stack, engine::kRows, -1);
    REQUIRE (rows.size() == static_cast<std::size_t> (engine::kRows));
    CHECK (dropSlot (rows, rows[0].y + 5) == 0);                       // on IN TRIM: first slot
    CHECK (dropSlot (rows, rows[1].y + 5) == 0);
    CHECK (dropSlot (rows, rows[4].y + kRowH / 2) == 3);
    CHECK (dropSlot (rows, rows[6].bottom() - 1) == 5);
    CHECK (dropSlot (rows, rows[8].y + 5) == 5);                       // on CEILING: last slot
    CHECK (dropSlot (rows, stack.y - 100) == 0);
    CHECK (dropSlot (rows, stack.bottom() + 1000) == 5);
}

TEST_CASE ("keyboard row selection clamps; accessibility label names slot, layer and depth", "[ui][model]")
{
    CHECK (nextRow (0, -1, 9) == 0);
    CHECK (nextRow (0, +1, 9) == 1);
    CHECK (nextRow (8, +1, 9) == 8);
    CHECK (nextRow (-1, +1, 9) == 0);   // nothing selected: Down selects the first row
    CHECK (nextRow (-1, -1, 9) == 8);   // ... and Up the last

    CHECK (accessibleLayerLabel (2, engine::kMb, 72.4f, true) == "Layer 3, Multiband, depth 72 percent");
    CHECK (accessibleLayerLabel (0, engine::kEq, 100.0f, false) == "Layer 1, EQ, off, depth 100 percent");
}

// ---- module 19c additions: the editor's own behaviour that is not drawing

TEST_CASE ("the assistant drawer lies over the right of the stack, the column's width", "[ui][model]")
{
    for (const auto& [w, h] : { std::pair { 900, 600 }, std::pair { 1099, 800 } })
    {
        const auto l = computeLayout (w, h);
        REQUIRE (l.assistDrawer);
        const auto d = drawerBounds (l);
        CHECK (d.w == kAssistW);
        CHECK (d.right() == l.stack.right());
        CHECK (d.y == l.stack.y);
        CHECK (d.h == l.stack.h);
    }
}

TEST_CASE ("layout without the assistant gives the stack the full width", "[ui][model]")
{
    const auto l = computeLayout (1400, 900, false);
    CHECK (l.assist.w == 0);
    CHECK_FALSE (l.assistDrawer);   // no drawer either: there is nothing to open yet
    CHECK (l.stack.w == 1400);
    CHECK (tiles (l, 1400, 900));
}

TEST_CASE ("peak meters fall at a fixed rate and never below the floor", "[ui][model]")
{
    CHECK_THAT (meterFall (-10.0f, -40.0f, 0.5f), WithinAbs (-10.0 - 0.5 * static_cast<double> (kMeterFallDbPerSec), 1e-4));
    CHECK_THAT (meterFall (-10.0f, -3.0f, 0.5f), WithinAbs (-3.0, 1e-6));   // rises at once
    CHECK_THAT (meterFall (-95.0f, -100.0f, 10.0f), WithinAbs (kMeterFloorDb, 1e-6));
    CHECK_THAT (meterFall (std::numeric_limits<float>::quiet_NaN(), -6.0f, 0.1f), WithinAbs (-6.0, 1e-6));
    CHECK_THAT (meterFall (-6.0f, std::numeric_limits<float>::quiet_NaN(), 0.1f), WithinAbs (-6.0 - 0.1 * static_cast<double> (kMeterFallDbPerSec), 1e-4));
}

TEST_CASE ("peak readouts hold their maximum for the hold time", "[ui][model]")
{
    PeakHold h;
    CHECK (h.db == kMeterFloorDb);
    update (h, -6.0f, 0.1f);
    CHECK (h.db == -6.0f);
    update (h, -20.0f, kPeakHoldSec - 0.2f);   // still held
    CHECK (h.db == -6.0f);
    update (h, -20.0f, 0.3f);                  // hold expired: takes the new reading
    CHECK (h.db == -20.0f);
    update (h, -3.0f, 0.01f);                  // a higher peak restarts the hold
    CHECK (h.db == -3.0f);
    update (h, -30.0f, kPeakHoldSec - 0.05f);
    CHECK (h.db == -3.0f);
}

TEST_CASE ("linked trims keep the gain through the chain constant", "[ui][model]")
{
    CHECK_THAT (linkedOutTrim (3.0f, 0.0f, 0.0f), WithinAbs (-3.0, 1e-6));
    CHECK_THAT (linkedOutTrim (-2.0f, 3.0f, -3.0f), WithinAbs (2.0, 1e-6));
    CHECK_THAT (linkedOutTrim (24.0f, 0.0f, -10.0f), WithinAbs (-24.0, 1e-6));   // clamped to the range
}
