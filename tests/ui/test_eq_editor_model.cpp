// EQ editor model (module 20a): the pure logic under the spectrum canvas and band editor.
// Contracts tested here:
//   - y maps gain linearly over +-range (0 dB at the vertical centre), x reuses the log axis
//   - the vertical range fits itself: the smallest of 6 / 12 / 18 dB that holds every active
//     gain-capable band (HP / LP gain is ignored: it does nothing)
//   - nodes sit at (freq, gain); HP / LP nodes sit on the 0 dB line
//   - hit-test: only enabled bands, within kNodeHitPx; the selected band wins, then the higher
//     band number (it is drawn on top)
//   - drag: freq from x in the log domain, gain from y, Shift = 1/10, clamped to the parameter
//     ranges; HP / LP keep their gain
//   - wheel changes Q multiplicatively (an octave of Q per notch), clamped to 0.1..18
//   - double-click on empty space turns on the first free band at the pointer
//   - keys nudge freq by 1/12 octave (1/48 with Shift) and gain by 0.5 dB (0.1 dB with Shift)
//   - the drawn curve equals dsp::settingsMagnitudeDb (one source of truth with the DSP)
//   - parameter IDs are 1-based ("eq.b3.freq") and the accessible label reads the band
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <limits>

#include "dsp/eq/EqResponse.h"
#include "ui/model/EqEditorModel.h"

using namespace strat3::ui;
namespace dsp = strat3::dsp;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

constexpr EqPlot plot { 600.0f, 240.0f, 12.0f };

dsp::EqBand band (dsp::EqType t, float hz, float db, float q = 0.707f)
{
    dsp::EqBand b;
    b.on = true;
    b.type = t;
    b.freq = hz;
    b.gainDb = db;
    b.q = q;
    return b;
}

} // namespace

TEST_CASE ("EQ model: gain axis is linear over +-range, 0 dB centred", "[ui][eq]")
{
    CHECK_THAT (eqGainToY (0.0f, plot), WithinAbs (120.0, 1e-4));
    CHECK_THAT (eqGainToY (12.0f, plot), WithinAbs (0.0, 1e-4));
    CHECK_THAT (eqGainToY (-12.0f, plot), WithinAbs (240.0, 1e-4));
    CHECK_THAT (eqGainToY (6.0f, plot), WithinAbs (60.0, 1e-4));
    for (float db : { -11.5f, -3.0f, 0.25f, 7.0f })
        CHECK_THAT (eqYToGain (eqGainToY (db, plot), plot), WithinAbs (db, 1e-4));
    // Outside the range the node pins to the edge instead of leaving the canvas.
    CHECK_THAT (eqGainToY (18.0f, plot), WithinAbs (0.0, 1e-4));
    CHECK_THAT (eqYToGain (-50.0f, plot), WithinAbs (12.0, 1e-4));
}

TEST_CASE ("EQ model: vertical range fits the largest active gain", "[ui][eq]")
{
    dsp::EqSettings s {};
    CHECK (eqAutoRange (s) == 6.0f);   // flat

    s[0] = band (dsp::EqType::Bell, 100.0f, 5.5f);
    CHECK (eqAutoRange (s) == 6.0f);
    s[1] = band (dsp::EqType::HighShelf, 8000.0f, -6.0f);
    CHECK (eqAutoRange (s) == 6.0f);   // exactly on the edge still fits
    s[1].gainDb = -6.5f;
    CHECK (eqAutoRange (s) == 12.0f);
    s[2] = band (dsp::EqType::Bell, 2000.0f, 17.0f);
    CHECK (eqAutoRange (s) == 18.0f);

    s[2].on = false;   // an off band does not widen the range
    CHECK (eqAutoRange (s) == 12.0f);

    dsp::EqSettings hp {};
    hp[0] = band (dsp::EqType::HighPass, 30.0f, 15.0f);   // stale gain on a pass filter is ignored
    CHECK (eqAutoRange (hp) == 6.0f);

    dsp::EqSettings bad {};
    bad[0] = band (dsp::EqType::Bell, 100.0f, std::numeric_limits<float>::quiet_NaN());
    CHECK (eqAutoRange (bad) == 6.0f);
}

TEST_CASE ("EQ model: node position; pass filters sit on 0 dB", "[ui][eq]")
{
    const auto bell = eqNodePos (band (dsp::EqType::Bell, 1000.0f, 6.0f), plot);
    CHECK_THAT (bell.x, WithinAbs (freqToX (1000.0, plot.w), 1e-3));
    CHECK_THAT (bell.y, WithinAbs (60.0, 1e-3));

    const auto hp = eqNodePos (band (dsp::EqType::HighPass, 40.0f, 9.0f), plot);
    CHECK_THAT (hp.x, WithinAbs (freqToX (40.0, plot.w), 1e-3));
    CHECK_THAT (hp.y, WithinAbs (120.0, 1e-3));
}

TEST_CASE ("EQ model: hit-test picks enabled nodes, selected first, then the higher band", "[ui][eq]")
{
    dsp::EqSettings s {};
    s[1] = band (dsp::EqType::Bell, 1000.0f, 0.0f);
    s[4] = band (dsp::EqType::Bell, 1000.0f, 0.0f);   // same spot as band 1
    s[6] = band (dsp::EqType::Bell, 5000.0f, 3.0f);
    s[6].on = false;

    const auto p = eqNodePos (s[1], plot);
    CHECK (eqHitTest (s, plot, p.x, p.y, -1) == 4);   // higher band drawn on top
    CHECK (eqHitTest (s, plot, p.x, p.y, 1) == 1);    // selected wins
    CHECK (eqHitTest (s, plot, p.x + kEqNodeHitPx - 1.0f, p.y, -1) == 4);
    CHECK (eqHitTest (s, plot, p.x + kEqNodeHitPx + 1.0f, p.y, -1) == -1);

    const auto off = eqNodePos (s[6], plot);
    CHECK (eqHitTest (s, plot, off.x, off.y, -1) == -1);   // off bands have no node
    CHECK (eqHitTest (s, plot, off.x, off.y, 6) == -1);    // even when selected
}

TEST_CASE ("EQ model: drag moves freq in the log domain and gain linearly", "[ui][eq]")
{
    const auto start = band (dsp::EqType::Bell, 1000.0f, 0.0f);
    const auto octavePx = static_cast<float> (freqToX (2000.0, plot.w) - freqToX (1000.0, plot.w));

    auto b = eqDragNode (start, plot, octavePx, -60.0f, false);
    CHECK_THAT (b.freq, WithinRel (2000.0f, 1e-4f));
    CHECK_THAT (b.gainDb, WithinAbs (6.0, 1e-4));
    CHECK (b.q == start.q);
    CHECK (b.type == start.type);

    auto fine = eqDragNode (start, plot, octavePx, -60.0f, true);
    CHECK_THAT (fine.freq, WithinRel (1000.0f * std::pow (2.0f, 0.1f), 1e-4f));
    CHECK_THAT (fine.gainDb, WithinAbs (0.6, 1e-4));

    // Clamped to the parameter ranges, never beyond them, even with the range at 6 dB.
    auto far = eqDragNode (start, plot, 1.0e6f, -1.0e6f, false);
    CHECK (far.freq == kEqFreqMax);
    CHECK (far.gainDb == kEqGainMax);
    auto low = eqDragNode (start, plot, -1.0e6f, 1.0e6f, false);
    CHECK (low.freq == kEqFreqMin);
    CHECK (low.gainDb == kEqGainMin);

    auto hp = eqDragNode (band (dsp::EqType::HighPass, 40.0f, 0.0f), plot, octavePx, -60.0f, false);
    CHECK_THAT (hp.freq, WithinRel (80.0f, 1e-4f));
    CHECK (hp.gainDb == 0.0f);   // a pass filter has no gain to drag

    auto nan = eqDragNode (start, plot, std::numeric_limits<float>::quiet_NaN(), 10.0f, false);
    CHECK (nan == start);
}

TEST_CASE ("EQ model: wheel scales Q multiplicatively and clamps", "[ui][eq]")
{
    CHECK_THAT (eqWheelQ (1.0f, 1.0f, false), WithinRel (2.0f, 1e-5f));
    CHECK_THAT (eqWheelQ (1.0f, -1.0f, false), WithinRel (0.5f, 1e-5f));
    CHECK_THAT (eqWheelQ (1.0f, 1.0f, true), WithinRel (std::pow (2.0f, 0.1f), 1e-5f));
    CHECK (eqWheelQ (17.0f, 3.0f, false) == kEqQMax);
    CHECK (eqWheelQ (0.2f, -3.0f, false) == kEqQMin);
    CHECK (eqWheelQ (1.0f, std::numeric_limits<float>::infinity(), false) == 1.0f);
}

TEST_CASE ("EQ model: double-click adds the first free band at the pointer", "[ui][eq]")
{
    dsp::EqSettings s {};
    s[0].on = true;
    s[1].on = true;
    CHECK (eqFirstFreeBand (s) == 2);

    s[2].type = dsp::EqType::HighShelf;   // a stale type from an earlier use of the band
    s[2].q = 2.5f;
    const float x = static_cast<float> (freqToX (440.0, plot.w));
    const auto b = eqPlaceBand (s[2], plot, x, eqGainToY (-3.0f, plot));
    CHECK (b.q == 2.5f);   // Q, channel and dynamics are kept
    CHECK (b.on);
    CHECK (b.type == dsp::EqType::Bell);
    CHECK_THAT (b.freq, WithinRel (440.0f, 1e-4f));
    CHECK_THAT (b.gainDb, WithinAbs (-3.0, 1e-4));

    for (auto& band : s)
        band.on = true;
    CHECK (eqFirstFreeBand (s) == -1);
}

TEST_CASE ("EQ model: key nudges", "[ui][eq]")
{
    CHECK_THAT (eqNudgeFreq (1000.0f, 1, false), WithinRel (1000.0f * std::pow (2.0f, 1.0f / 12.0f), 1e-5f));
    CHECK_THAT (eqNudgeFreq (1000.0f, -1, true), WithinRel (1000.0f * std::pow (2.0f, -1.0f / 48.0f), 1e-5f));
    CHECK (eqNudgeFreq (19990.0f, 1, false) == kEqFreqMax);
    CHECK (eqNudgeFreq (20.5f, -1, false) == kEqFreqMin);

    CHECK_THAT (eqNudgeGain (0.0f, 1, false), WithinAbs (0.5, 1e-6));
    CHECK_THAT (eqNudgeGain (0.0f, -1, true), WithinAbs (-0.1, 1e-6));
    // Snaps to the 0.1 grid so repeated steps do not drift.
    float g = 0.0f;
    for (int i = 0; i < 30; ++i)
        g = eqNudgeGain (g, 1, true);
    CHECK_THAT (g, WithinAbs (3.0, 1e-5));
    CHECK (eqNudgeGain (17.8f, 1, false) == kEqGainMax);
}

TEST_CASE ("EQ model: curve equals the DSP magnitude for each view", "[ui][eq]")
{
    dsp::EqSettings s {};
    s[0] = band (dsp::EqType::HighPass, 30.0f, 0.0f);
    s[1] = band (dsp::EqType::Bell, 250.0f, -3.0f, 1.4f);
    s[2] = band (dsp::EqType::HighShelf, 9000.0f, 2.0f);
    s[2].channel = dsp::EqChannel::Side;
    s[3] = band (dsp::EqType::PassiveShelf, 60.0f, 4.0f);
    s[3].channel = dsp::EqChannel::Mid;

    constexpr double fs = 48000.0;
    constexpr int points = 97;
    for (auto view : { dsp::EqChannel::Stereo, dsp::EqChannel::Mid, dsp::EqChannel::Side })
    {
        const auto c = eqCurveDb (s, points, fs, view);
        REQUIRE (c.size() == static_cast<std::size_t> (points));
        for (int i = 0; i < points; i += 8)
        {
            const double hz = xToFreq (static_cast<double> (i), points - 1.0);
            CHECK_THAT (c[static_cast<std::size_t> (i)], WithinAbs (dsp::settingsMagnitudeDb (s, hz, fs, view), 1e-3));
        }
    }
    CHECK (eqCurveDb (s, 1, fs, dsp::EqChannel::Stereo).empty());   // a curve needs two ends
}

TEST_CASE ("EQ model: parameter IDs and accessible label", "[ui][eq]")
{
    CHECK (eqParamId (0, "freq") == "eq.b1.freq");
    CHECK (eqParamId (7, "on") == "eq.b8.on");

    CHECK (eqAccessibleLabel (2, band (dsp::EqType::Bell, 1000.0f, 2.0f, 0.707f))
           == "EQ band 3, Bell, 1.00k hertz, +2.0 dB, Q 0.71");
    CHECK (eqAccessibleLabel (0, band (dsp::EqType::HighPass, 30.0f, 0.0f, 0.707f))
           == "EQ band 1, High Pass, 30 hertz, Q 0.71");
    auto off = band (dsp::EqType::LowShelf, 80.0f, -1.5f, 0.5f);
    off.on = false;
    CHECK (eqAccessibleLabel (1, off) == "EQ band 2, off");
}

TEST_CASE ("EQ model: spectrum backdrop maps -12..-96 dB onto the canvas height", "[ui][eq]")
{
    CHECK_THAT (eqSpectrumToY (kEqSpectrumTopDb, plot), WithinAbs (0.0, 1e-4));
    CHECK_THAT (eqSpectrumToY (kEqSpectrumBottomDb, plot), WithinAbs (240.0, 1e-4));
    CHECK_THAT (eqSpectrumToY (-54.0f, plot), WithinAbs (120.0, 1e-4));
    CHECK_THAT (eqSpectrumToY (0.0f, plot), WithinAbs (0.0, 1e-4));       // pinned, not off-canvas
    CHECK_THAT (eqSpectrumToY (-120.0f, plot), WithinAbs (240.0, 1e-4));
    CHECK_THAT (eqSpectrumToY (std::numeric_limits<float>::quiet_NaN(), plot), WithinAbs (240.0, 1e-4));
}

TEST_CASE ("EQ model: band editor value text", "[ui][eq]")
{
    CHECK (eqValueText ("freq", 499.9997559f) == "500 Hz");
    CHECK (eqValueText ("freq", 2500.0f) == "2.50k Hz");
    CHECK (eqValueText ("gain", 3.0f) == "+3.0 dB");
    CHECK (eqValueText ("gain", -0.04f) == "0.0 dB");
    CHECK (eqValueText ("q", 0.707f) == "0.71");
    CHECK (eqValueText ("thresh", -20.0f) == "-20.0 dB");
    CHECK (eqValueText ("range", -3.0f) == "-3.0 dB");
    CHECK (eqValueText ("atk", 10.0000019f) == "10.0 ms");
    CHECK (eqValueText ("rel", 99.9999924f) == "100 ms");
    CHECK (eqValueText ("phase", 37.4f) == "37 %");
    CHECK (eqValueText ("other", 1.5f).empty());   // unknown: caller keeps the default text
}

TEST_CASE ("EQ model: axis tick labels are short", "[ui][eq]")
{
    CHECK (eqTickText (20.0) == "20");
    CHECK (eqTickText (500.0) == "500");
    CHECK (eqTickText (1000.0) == "1k");
    CHECK (eqTickText (20000.0) == "20k");
}
