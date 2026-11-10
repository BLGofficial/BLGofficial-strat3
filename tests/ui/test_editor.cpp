// Native editor (module 19c), with the assistant column (module 21c).
// Layout chosen by Blago 2026-11-04: vertical layer stack + impact strips + delivery bar; from
// the ARCHON reference only the clip LEDs, the linked I/O trim and the loudness readouts.
// Contracts tested here (headless: the components are driven directly, nothing is shown):
//   - the processor opens this editor; its regions follow the layout model
//   - rows follow the prepared layer order and rebuild after a drag reorder
//   - depth handles write their parameter inside a host gesture (Shift = fine)
//   - keyboard: Up/Down select, Left/Right depth (Shift fine), Enter expands, D delta, G gain match
//   - the expanded row holds every parameter of its layer
//   - the impact strips show the processor's impact map; the delivery bar its meters
//   - clip LEDs show and clear; linked trims move OUT TRIM opposite to IN TRIM
//   - controls carry the Phase 4 accessibility labels
//   - a headless render paints the theme (and is saved as a preview PNG)
//   - the editor can tick and close while the audio thread runs
//   - the assistant column (21c): beside the stack from 1100 px wide; below that a drawer
//     the header opens over the right of the stack, closed whenever the window narrows.
//     These tests build without the WebView, so the column holds the native notice saying
//     the panels are not in this build; the WebView itself is checked by the webview app
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cmath>
#include <memory>
#include <numbers>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "processor/Strat3Processor.h"
#include "ui/Strat3Editor.h"

using namespace strat3;
using processor::Strat3Processor;
using ui::Strat3Editor;
using Catch::Approx;

namespace {

constexpr double kFs = 48000.0;
constexpr int kBlock = 512;

struct JuceScope
{
    juce::ScopedJuceInitialiser_GUI init;
};

void setParam (Strat3Processor& p, const std::string& id, float value)
{
    auto* param = p.apvts.getParameter (id);
    REQUIRE (param != nullptr);
    param->setValueNotifyingHost (param->convertTo0to1 (value));
}

float rawValue (Strat3Processor& p, const std::string& id) { return p.apvts.getRawParameterValue (id)->load(); }

std::vector<float> noise (int n, float rms, unsigned seed)
{
    std::mt19937 rng (seed);
    const float a = rms * std::sqrt (3.0f);
    std::uniform_real_distribution<float> d (-a, a);
    std::vector<float> x (static_cast<std::size_t> (n));
    for (auto& v : x)
        v = d (rng);
    return x;
}

std::vector<float> sine (int n, double hz, float amp)
{
    std::vector<float> x (static_cast<std::size_t> (n));
    for (int i = 0; i < n; ++i)
        x[static_cast<std::size_t> (i)] = amp * static_cast<float> (std::sin (2.0 * std::numbers::pi * hz * i / kFs));
    return x;
}

void runStereo (Strat3Processor& p, std::vector<float> l, std::vector<float> r, int tickEvery = 3)
{
    juce::MidiBuffer midi;
    const int n = static_cast<int> (l.size());
    int count = 0;
    for (int off = 0; off < n; off += kBlock)
    {
        const int len = std::min (kBlock, n - off);
        float* ptrs[] { l.data() + off, r.data() + off };
        juce::AudioBuffer<float> buf (ptrs, 2, len);
        p.processBlock (buf, midi);
        if (tickEvery > 0 && ++count % tickEvery == 0)
            p.analysisTick();
    }
}

struct Fixture
{
    JuceScope juce;
    std::unique_ptr<Strat3Processor> proc = std::make_unique<Strat3Processor> (false);
    std::unique_ptr<Strat3Editor> editor;

    Fixture()
    {
        proc->prepareToPlay (kFs, kBlock);
        auto* ed = proc->createEditor();
        REQUIRE (dynamic_cast<Strat3Editor*> (ed) != nullptr);
        editor.reset (static_cast<Strat3Editor*> (ed));
    }
    ~Fixture()
    {
        editor.reset();   // before the processor, as a host does
        proc->releaseResources();
    }
};

/** Counts gesture begin / end on one parameter. */
struct GestureCounter final : juce::AudioProcessorParameter::Listener
{
    int begins = 0, ends = 0;
    void parameterValueChanged (int, float) override {}
    void parameterGestureChanged (int, bool starting) override { (starting ? begins : ends)++; }
};

int rowOfLayer (const Strat3Editor& e, engine::DepthLayer k)
{
    for (int r = 0; r < e.rowCount(); ++r)
        if (e.row (r).kind == ui::RowKind::Layer && e.row (r).layer == k)
            return r;
    return -1;
}

juce::Colour pixel (const juce::Image& img, int x, int y) { return img.getPixelAt (x, y); }

} // namespace

TEST_CASE ("editor: the processor opens it; regions follow the layout model", "[ui][editor]")
{
    Fixture f;
    auto& e = *f.editor;
    CHECK (e.getWidth() == ui::kDefaultW);
    CHECK (e.getHeight() == ui::kDefaultH);
    CHECK (e.isResizable());

    e.setSize (1400, 900);
    const auto l = ui::computeLayout (1400, 900);
    auto same = [] (juce::Rectangle<int> a, ui::Rect b) { return a.getX() == b.x && a.getY() == b.y && a.getWidth() == b.w && a.getHeight() == b.h; };
    CHECK (same (e.headerBounds(), l.header));
    CHECK (same (e.stackBounds(), l.stack));
    CHECK (same (e.deliveryBounds(), l.delivery));
    CHECK (same (e.panelBounds(), l.assist));
    CHECK (e.panelShown());
}

TEST_CASE ("editor: rows follow the prepared order and rebuild after a reorder", "[ui][editor]")
{
    Fixture f;
    auto& e = *f.editor;
    REQUIRE (e.rowCount() == engine::kRows);
    CHECK (e.row (0).kind == ui::RowKind::InTrim);
    CHECK (e.row (1).layer == engine::kEq);
    CHECK (e.row (8).kind == ui::RowKind::Ceiling);

    e.dropLayer (0, 5);   // drag EQ to the bottom slot
    CHECK (f.proc->layerOrder() == ui::orderToIds (ui::moveLayer (engine::kDefaultOrder, 0, 5)));
    CHECK (e.row (1).layer == engine::kEq);   // not applied until the re-prepare ran
    f.proc->applyPendingConfig();
    e.tick (1.0f / 30.0f);
    CHECK (e.row (1).layer == engine::kLow);
    CHECK (e.row (6).layer == engine::kEq);
}

TEST_CASE ("editor: a depth drag writes the parameter inside one gesture", "[ui][editor]")
{
    Fixture f;
    auto& e = *f.editor;
    const int r = rowOfLayer (e, engine::kMb);
    REQUIRE (r > 0);
    auto& h = e.rowComponent (r).handle();
    GestureCounter g;
    auto* param = f.proc->apvts.getParameter ("mb.depth");
    param->addListener (&g);

    const float track = static_cast<float> (h.trackWidth());
    REQUIRE (track > 50.0f);
    h.beginDrag();
    h.dragBy (-track / 3.0f, false);       // -50 %
    CHECK (rawValue (*f.proc, "mb.depth") == Approx (50.0f).margin (0.01));
    h.dragBy (-track / 3.0f + 30.0f, true); // dx is from the drag start; Shift: 1/10
    CHECK (rawValue (*f.proc, "mb.depth") == Approx (100.0f + (30.0f - track / 3.0f) / track * 15.0f).margin (0.05));
    h.endDrag();
    CHECK (g.begins == 1);
    CHECK (g.ends == 1);

    h.resetToDefault();                    // double-click
    CHECK (rawValue (*f.proc, "mb.depth") == Approx (100.0f));
    param->removeListener (&g);
}

TEST_CASE ("editor: keyboard selects rows, steps depth, expands, toggles delta and gain match", "[ui][editor]")
{
    Fixture f;
    auto& e = *f.editor;
    CHECK (e.selectedRow() == -1);
    CHECK (e.keyPressed (juce::KeyPress (juce::KeyPress::downKey)));
    CHECK (e.selectedRow() == 0);
    e.keyPressed (juce::KeyPress (juce::KeyPress::downKey));
    REQUIRE (e.selectedRow() == 1);   // EQ

    e.keyPressed (juce::KeyPress (juce::KeyPress::rightKey));
    CHECK (rawValue (*f.proc, "eq.depth") == Approx (101.0f));
    e.keyPressed (juce::KeyPress (juce::KeyPress::leftKey, juce::ModifierKeys::shiftModifier, 0));
    CHECK (rawValue (*f.proc, "eq.depth") == Approx (100.9f).margin (1e-3));

    e.keyPressed (juce::KeyPress (juce::KeyPress::returnKey));
    CHECK (e.expandedRow() == 1);
    e.keyPressed (juce::KeyPress (juce::KeyPress::returnKey));
    CHECK (e.expandedRow() == -1);

    e.keyPressed (juce::KeyPress ('d'));
    CHECK (rawValue (*f.proc, "global.delta") == 1.0f);
    e.keyPressed (juce::KeyPress ('g'));
    CHECK (rawValue (*f.proc, "global.gainMatch") == 1.0f);
    e.keyPressed (juce::KeyPress ('g'));
    CHECK (rawValue (*f.proc, "global.gainMatch") == 0.0f);

    e.keyPressed (juce::KeyPress (juce::KeyPress::upKey));
    e.keyPressed (juce::KeyPress (juce::KeyPress::rightKey));   // IN TRIM: 0.5 dB steps
    CHECK (rawValue (*f.proc, "global.inTrim") == Approx (0.5f));
}

TEST_CASE ("editor: the expanded row holds every parameter of its layer", "[ui][editor]")
{
    Fixture f;
    auto& e = *f.editor;
    const int r = rowOfLayer (e, engine::kLimit);
    e.toggleExpanded (r);
    CHECK (e.expandedRow() == r);
    // limit.* minus on / depth / delta (those live in the row itself)
    int expected = 0;
    for (auto* p : f.proc->getParameters())
        if (auto* wp = dynamic_cast<juce::RangedAudioParameter*> (p))
        {
            const auto id = wp->getParameterID();
            if (id.startsWith ("limit.") && id != "limit.on" && id != "limit.depth" && id != "limit.delta")
                ++expected;
        }
    CHECK (expected > 0);
    CHECK (e.rowComponent (r).panelControlCount() == expected);
    CHECK (e.rowComponent (r).getHeight() == ui::kRowH + ui::kEditorH);

    e.toggleExpanded (rowOfLayer (e, engine::kEq));   // one at a time
    CHECK (e.expandedRow() == rowOfLayer (e, engine::kEq));
    CHECK (e.rowComponent (r).panelControlCount() == 0);
}

TEST_CASE ("editor: impact strips show the processor's impact map", "[ui][editor]")
{
    Fixture f;
    setParam (*f.proc, "eq.on", 1.0f);
    setParam (*f.proc, "eq.b1.on", 1.0f);
    setParam (*f.proc, "eq.b1.freq", 1000.0f);
    setParam (*f.proc, "eq.b1.gain", 6.0f);
    setParam (*f.proc, "eq.b1.q", 1.0f);
    runStereo (*f.proc, noise (3 * 48000, 0.05f, 31), noise (3 * 48000, 0.05f, 32));
    auto& e = *f.editor;
    e.tick (1.0f / 30.0f);

    constexpr int k1k = 34;
    const int eqRow = rowOfLayer (e, engine::kEq), lowRow = rowOfLayer (e, engine::kLow);
    CHECK (e.rowComponent (eqRow).impact()[k1k] == Approx (6.0f).margin (1.0));
    CHECK (std::abs (e.rowComponent (lowRow).impact()[k1k]) < 0.5f);

    // An off layer outputs its band-split reference (an all-pass, for click-free switching).
    // Magnitude is unchanged, but the analyser's windows see the group delay at low
    // frequencies as up to ~2 dB of change; an off row therefore shows nothing at all.
    for (const engine::DepthLayer k : { engine::kLow, engine::kMb, engine::kSat, engine::kLimit })
        for (float v : e.rowComponent (rowOfLayer (e, k)).impact())
            CHECK (v == 0.0f);
}

TEST_CASE ("editor: the delivery bar shows the processor's meters", "[ui][editor]")
{
    Fixture f;
    const auto x = sine (4 * 48000, 1000.0, 0.25f);
    runStereo (*f.proc, x, x);
    auto& e = *f.editor;
    e.tick (1.0f / 30.0f);   // holds the onset overshoot (~1 dB through the band split's all-pass)
    runStereo (*f.proc, x, x);
    e.tick (2.0f);           // hold expired: the steady crest
    e.tick (1.0f / 30.0f);   // numbers refresh every other frame
    const auto m = f.proc->meters();
    const auto text = e.deliveryText();
    CHECK (text.contains ("LUFS-I " + ui::formatLufs (m.integratedLufs)));
    CHECK (text.contains ("S " + ui::formatLufs (m.shortTermLufs)));
    CHECK (text.contains ("PLR " + ui::formatDb (m.plr, 1, false)));
    CHECK (text.contains ("CORR +1.00"));
    CHECK (text.contains ("LATENCY " + ui::formatLatency (f.proc->getLatencySamples(), kFs)));
    CHECK (text.contains ("TP -12.0"));    // 0.25 = -12.0 dBTP
    CHECK (text.contains ("SPOTIFY "));
    CHECK_FALSE (text.contains ("NON-FINITE"));
}

TEST_CASE ("editor: clip LEDs show and clear", "[ui][editor]")
{
    Fixture f;
    auto& e = *f.editor;
    const auto hot = sine (24000, 1000.0, 1.2f);
    runStereo (*f.proc, hot, hot);
    e.tick (1.0f / 30.0f);
    CHECK (e.inClipShown());
    e.clickClipLeds();
    e.tick (1.0f / 30.0f);
    CHECK_FALSE (e.inClipShown());
    CHECK_FALSE (f.proc->takePeaks().inClip);
}

TEST_CASE ("editor: linked trims move OUT TRIM opposite to IN TRIM", "[ui][editor]")
{
    Fixture f;
    auto& e = *f.editor;
    auto& in = e.rowComponent (0).handle();
    const float track = static_cast<float> (in.trackWidth());

    in.beginDrag();
    in.dragBy (track / 16.0f, false);   // 48 dB range: +3 dB
    in.endDrag();
    CHECK (rawValue (*f.proc, "global.inTrim") == Approx (3.0f).margin (0.01));
    CHECK (rawValue (*f.proc, "global.outTrim") == 0.0f);   // not linked by default

    e.setTrimsLinked (true);
    in.beginDrag();
    in.dragBy (track / 16.0f, false);
    in.endDrag();
    CHECK (rawValue (*f.proc, "global.inTrim") == Approx (6.0f).margin (0.01));
    CHECK (rawValue (*f.proc, "global.outTrim") == Approx (-3.0f).margin (0.01));
}

TEST_CASE ("editor: controls carry the Phase 4 accessibility labels", "[ui][editor]")
{
    Fixture f;
    auto& e = *f.editor;
    const int r = rowOfLayer (e, engine::kMb);
    setParam (*f.proc, "mb.on", 1.0f);
    setParam (*f.proc, "mb.depth", 72.0f);
    e.tick (1.0f / 30.0f);
    CHECK (e.rowComponent (r).handle().getTitle() == juce::String (ui::accessibleLayerLabel (r - 1, engine::kMb, 72.0f, true)));
    CHECK (e.rowComponent (0).handle().getTitle().startsWith ("Input trim"));
}

TEST_CASE ("editor: a headless render paints the theme", "[ui][editor][render]")
{
    Fixture f;
    setParam (*f.proc, "limit.on", 1.0f);
    runStereo (*f.proc, noise (2 * 48000, 0.2f, 41), noise (2 * 48000, 0.2f, 42));
    auto& e = *f.editor;
    e.setSize (1400, 900);
    e.tick (1.0f / 30.0f);
    const auto img = e.createComponentSnapshot (e.getLocalBounds(), true, 1.0f);
    REQUIRE (img.getWidth() == 1400);

    const auto l = ui::computeLayout (1400, 900);
    CHECK (pixel (img, l.header.right() - 2, l.header.y + 1) == ui::theme::kBase);   // header background
    // The left marker: filled in the signal colour on an active layer, not on an off one.
    auto markerAt = [&] (engine::DepthLayer k)
    { return pixel (img, 1, l.stack.y + e.rowComponent (rowOfLayer (e, k)).getY() + ui::kRowH / 2); };
    CHECK (markerAt (engine::kLimit) == ui::theme::kSignal);
    CHECK (markerAt (engine::kEq) != ui::theme::kSignal);

    const auto out = juce::File (STRAT3_CANDIDATE_DIR).getChildFile ("strat3-editor.png");
    out.deleteFile();
    juce::FileOutputStream os (out);
    REQUIRE (os.openedOk());
    CHECK (juce::PNGImageFormat().writeImageToStream (img, os));
}

TEST_CASE ("editor: the assistant column holds the panel beside the stack", "[ui][editor][panel]")
{
    Fixture f;
    auto& e = *f.editor;
    auto same = [] (juce::Rectangle<int> a, ui::Rect b) { return a.getX() == b.x && a.getY() == b.y && a.getWidth() == b.w && a.getHeight() == b.h; };
    for (const auto& [w, h] : { std::pair { 1100, 600 }, std::pair { 1280, 800 }, std::pair { 2560, 1600 } })
    {
        INFO ("size " << w << " x " << h);
        e.setSize (w, h);
        const auto l = ui::computeLayout (w, h);
        CHECK_FALSE (e.drawerMode());
        CHECK (e.panelShown());
        CHECK (same (e.panelBounds(), l.assist));
        CHECK (same (e.stackBounds(), l.stack));
        CHECK_FALSE (e.drawerButton().isVisible());
        e.setDrawerOpen (true);   // nothing to open while the column fits
        CHECK_FALSE (e.drawerOpen());
    }
}

TEST_CASE ("editor: below 1100 px the panel is a drawer the header opens", "[ui][editor][panel]")
{
    Fixture f;
    auto& e = *f.editor;
    auto same = [] (juce::Rectangle<int> a, ui::Rect b) { return a.getX() == b.x && a.getY() == b.y && a.getWidth() == b.w && a.getHeight() == b.h; };
    e.setSize (1000, 700);
    const auto l = ui::computeLayout (1000, 700);
    CHECK (e.drawerMode());
    CHECK (same (e.stackBounds(), l.stack));   // the stack keeps the full width
    CHECK (l.stack.w == 1000);
    CHECK_FALSE (e.drawerOpen());
    CHECK_FALSE (e.panelShown());
    CHECK (e.drawerButton().isVisible());
    CHECK (e.drawerButton().getTitle() == "Assistant");
    CHECK_FALSE (e.drawerButton().getToggleState());

    e.drawerButton().setToggleState (true, juce::sendNotificationSync);   // as a click does
    CHECK (e.drawerOpen());
    CHECK (e.panelShown());
    CHECK (e.drawerButton().getToggleState());
    CHECK (same (e.panelBounds(), ui::drawerBounds (l)));
    CHECK (same (e.stackBounds(), l.stack));   // the drawer lies over the stack, it does not push it
    // Over the stack, so it must be in front of it.
    CHECK (e.getIndexOfChildComponent (e.panelComponent()) > e.getIndexOfChildComponent (e.stackComponent()));

    e.setDrawerOpen (false);
    CHECK_FALSE (e.panelShown());
    CHECK_FALSE (e.drawerButton().getToggleState());
}

TEST_CASE ("editor: the drawer closes when the window narrows and the column returns when it widens", "[ui][editor][panel]")
{
    Fixture f;
    auto& e = *f.editor;
    e.setSize (1000, 700);
    e.setDrawerOpen (true);
    REQUIRE (e.panelShown());

    e.setSize (1400, 800);   // wide: a column, whatever the drawer was
    CHECK_FALSE (e.drawerMode());
    CHECK (e.panelShown());
    CHECK_FALSE (e.drawerButton().isVisible());

    e.setSize (1050, 800);   // narrow again: the drawer starts closed
    CHECK (e.drawerMode());
    CHECK_FALSE (e.drawerOpen());
    CHECK_FALSE (e.panelShown());

    e.setDrawerOpen (true);
    e.setSize (1080, 800);   // still narrow: an open drawer stays open
    CHECK (e.drawerOpen());
    CHECK (e.panelShown());
}

namespace {

/** A WebView panel whose page never says it is ready: what a broken runtime looks like. */
struct NeverLoads final : ui::AssistantPanel
{
    void tick (double) override { ++ticks; }
    bool failed() const override { return ticks >= 2; }
    ui::web::PanelKind kind() const override { return ui::web::PanelKind::Web; }
    int ticks = 0;
};

/** A WebView panel that loads: what the gear talks to. */
struct LoadedWeb final : ui::AssistantPanel
{
    void showAdvanced() override { ++advancedAsked; }
    ui::web::PanelKind kind() const override { return ui::web::PanelKind::Web; }
    int advancedAsked = 0;
};

} // namespace

TEST_CASE ("editor: the header's buttons overlap nothing at any width", "[ui][editor][panel]")
{
    Fixture f;
    auto& e = *f.editor;
    e.replacePanel (std::make_unique<LoadedWeb>());   // the gear shows with the WebView only
    auto* header = e.drawerButton().getParentComponent();
    REQUIRE (header != nullptr);
    for (int w : { 900, 950, 1000, 1050, 1099, 1100, 1280, 1400, 2560 })
    {
        INFO ("width " << w);
        e.setSize (w, 700);
        CHECK (e.advancedButton().isVisible());
        CHECK (e.drawerButton().isVisible() == (w < ui::kAssistCollapseW));
        std::vector<juce::Component*> shown;
        for (auto* c : header->getChildren())
            if (c->isVisible())
                shown.push_back (c);
        for (std::size_t i = 0; i < shown.size(); ++i)
        {
            INFO ("control: " << shown[i]->getTitle());
            CHECK (header->getLocalBounds().contains (shown[i]->getBounds()));
            for (std::size_t j = i + 1; j < shown.size(); ++j)
            {
                INFO ("and: " << shown[j]->getTitle());
                CHECK_FALSE (shown[i]->getBounds().intersects (shown[j]->getBounds()));
            }
        }
    }
}

TEST_CASE ("editor: the gear shows only while the column holds the WebView", "[ui][editor][panel]")
{
    Fixture f;
    auto& e = *f.editor;
    for (int w : { 1000, 1400 })
    {
        e.setSize (w, 800);
        CHECK (e.panelKind() == ui::web::PanelKind::NotBuilt);
        CHECK_FALSE (e.advancedButton().isVisible());   // the notice has no advanced view
    }
    e.replacePanel (std::make_unique<LoadedWeb>());
    CHECK (e.advancedButton().isVisible());
    CHECK (e.advancedButton().getTitle() == "Advanced");
    e.replacePanel (std::make_unique<ui::PanelNotice> (ui::web::PanelKind::DidNotLoad));
    CHECK_FALSE (e.advancedButton().isVisible());
}

TEST_CASE ("editor: the gear opens the advanced view, and the drawer when the window is narrow", "[ui][editor][panel]")
{
    Fixture f;
    auto& e = *f.editor;
    auto web = std::make_unique<LoadedWeb>();
    auto* panel = web.get();
    e.replacePanel (std::move (web));

    e.setSize (1400, 800);
    REQUIRE (e.advancedButton().onClick != nullptr);
    e.advancedButton().onClick();   // as a click does
    CHECK (panel->advancedAsked == 1);
    CHECK (e.panelShown());
    CHECK_FALSE (e.drawerOpen());

    e.setSize (1000, 700);
    REQUIRE_FALSE (e.panelShown());
    e.advancedButton().onClick();
    CHECK (e.drawerOpen());
    CHECK (e.panelShown());
    CHECK (e.drawerButton().getToggleState());
    CHECK (panel->advancedAsked == 2);

    e.advancedButton().onClick();   // already open: stays open
    CHECK (e.drawerOpen());
    CHECK (panel->advancedAsked == 3);
}

TEST_CASE ("editor: a panel that does not load is replaced by the notice", "[ui][editor][panel]")
{
    Fixture f;
    auto& e = *f.editor;
    e.setSize (1400, 900);
    e.replacePanel (std::make_unique<NeverLoads>());
    REQUIRE (e.panelKind() == ui::web::PanelKind::Web);
    e.tick (1.0f / 30.0f);
    CHECK (e.panelKind() == ui::web::PanelKind::Web);
    e.tick (1.0f / 30.0f);
    CHECK (e.panelKind() == ui::web::PanelKind::DidNotLoad);
    CHECK (e.panelText() == ui::web::noticeFor (ui::web::PanelKind::DidNotLoad).body);
    CHECK (e.panelShown());
    const auto a = ui::computeLayout (1400, 900).assist;
    CHECK (e.panelBounds() == juce::Rectangle<int> (a.x, a.y, a.w, a.h));   // in the column, not at 0,0
}

TEST_CASE ("editor: without the WebView panels the column says so", "[ui][editor][panel]")
{
    Fixture f;
    auto& e = *f.editor;
    e.setSize (1400, 900);
    CHECK (e.panelKind() == ui::web::PanelKind::NotBuilt);
    const auto notice = ui::web::noticeFor (ui::web::PanelKind::NotBuilt);
    CHECK (e.panelTitle() == notice.title);
    CHECK (e.panelText() == notice.body);

    // Painted in the column: the notice is the theme's base colour with text on it, and
    // nothing of it reaches into the stack.
    const auto img = e.createComponentSnapshot (e.getLocalBounds(), true, 1.0f);
    const auto l = ui::computeLayout (1400, 900);
    CHECK (pixel (img, l.assist.x + l.assist.w / 2, l.assist.bottom() - 4) == ui::theme::kBase);
    bool text = false;
    for (int y = l.assist.y; y < l.assist.y + 120 && ! text; ++y)
        for (int x = l.assist.x; x < l.assist.right() && ! text; ++x)
            text = pixel (img, x, y) != ui::theme::kBase && pixel (img, x, y) != ui::theme::kRule;
    CHECK (text);
}

TEST_CASE ("editor: ticks and closes while the audio thread runs", "[ui][editor][threads]")
{
    JuceScope juce;
    auto proc = std::make_unique<Strat3Processor> (false);
    proc->prepareToPlay (kFs, kBlock);
    std::unique_ptr<juce::AudioProcessorEditor> ed (proc->createEditor());
    auto* e = dynamic_cast<Strat3Editor*> (ed.get());
    REQUIRE (e != nullptr);

    std::atomic<bool> stop { false };
    std::thread audio ([&] {
        auto l = noise (kBlock, 0.1f, 51), r = l;
        juce::MidiBuffer midi;
        while (! stop.load())
        {
            float* ptrs[] { l.data(), r.data() };
            juce::AudioBuffer<float> buf (ptrs, 2, kBlock);
            proc->processBlock (buf, midi);
        }
    });
    std::thread analysis ([&] {
        while (! stop.load())
        {
            proc->analysisTick();
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
        }
    });
    for (int i = 0; i < 60; ++i)
    {
        e->tick (1.0f / 30.0f);
        std::this_thread::sleep_for (std::chrono::milliseconds (2));
    }
    ed.reset();
    stop.store (true);
    audio.join();
    analysis.join();
    CHECK_FALSE (proc->meters().nonFinite);
}
