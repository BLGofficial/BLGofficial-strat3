// EQ editor (module 20c): the EQ row's in-place editor, replacing the generic panel.
// Phase 4: "EQ nodes on the spectrum"; from the ARCHON reference only the band editor (value
// boxes for the selected band). Option A (Blago, 2026-11-09): the backdrop is the impact
// analyser's 1/6-octave level spectrum of the EQ's input.
// Contracts tested here (headless: components driven directly):
//   - the EQ row expands into the spectrum editor; other rows keep the generic panel
//   - the band editor binds the selected band's 11 parameters plus eq.phase, so every eq.*
//     parameter stays reachable (row: on / depth / delta)
//   - a node drag writes freq and gain, each inside one host gesture; HP / LP drag freq only
//   - double-click on empty canvas enables the first free band there; on a node, turns it off
//   - the wheel sets Q, keys nudge the selected band (Shift fine), Delete turns it off
//   - the vertical range follows the band gains (frozen during a drag); the backdrop follows
//     the EQ input spectrum
//   - a headless render paints the selected node in the signal colour
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "processor/Strat3Processor.h"
#include "ui/Strat3Editor.h"
#include "ui/model/EqEditorModel.h"
#include "ui/native/EqEditor.h"
#include "ui/native/Theme.h"

using namespace strat3;
using processor::Strat3Processor;
using ui::Strat3Editor;
using Catch::Approx;

namespace {

constexpr double kFs = 48000.0;
constexpr int kBlock = 512;

struct Fixture
{
    juce::ScopedJuceInitialiser_GUI init;
    std::unique_ptr<Strat3Processor> proc = std::make_unique<Strat3Processor> (false);
    std::unique_ptr<Strat3Editor> editor;
    int eqRow = -1;

    Fixture()
    {
        proc->prepareToPlay (kFs, kBlock);
        editor.reset (static_cast<Strat3Editor*> (proc->createEditor()));
        for (int r = 0; r < editor->rowCount(); ++r)
            if (editor->row (r).kind == ui::RowKind::Layer && editor->row (r).layer == engine::kEq)
                eqRow = r;
        REQUIRE (eqRow > 0);
        editor->toggleExpanded (eqRow);
    }
    ~Fixture()
    {
        editor.reset();
        proc->releaseResources();
    }

    ui::EqEditor& eq()
    {
        auto* e = editor->rowComponent (eqRow).eqEditor();
        REQUIRE (e != nullptr);
        return *e;
    }

    void set (const std::string& id, float v)
    {
        auto* p = proc->apvts.getParameter (id);
        REQUIRE (p != nullptr);
        p->setValueNotifyingHost (p->convertTo0to1 (v));
    }
    float raw (const std::string& id) { return proc->apvts.getRawParameterValue (id)->load(); }
};

struct GestureCounter final : juce::AudioProcessorParameter::Listener
{
    int begins = 0, ends = 0;
    void parameterValueChanged (int, float) override {}
    void parameterGestureChanged (int, bool starting) override { (starting ? begins : ends)++; }
};

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

/** Runs `seconds` of white noise (-26 dBFS RMS, independent channels) through the processor
 *  with the analysis thread's work done inline, then ticks the editor once. */
void feed (Fixture& f, int seconds)
{
    auto l = noise (seconds * 48000, 0.05f, 61), r = noise (seconds * 48000, 0.05f, 62);
    juce::MidiBuffer midi;
    for (int off = 0, k = 0; off < static_cast<int> (l.size()); off += kBlock, ++k)
    {
        float* ptrs[] { l.data() + off, r.data() + off };
        juce::AudioBuffer<float> buf (ptrs, 2, std::min (kBlock, static_cast<int> (l.size()) - off));
        f.proc->processBlock (buf, midi);
        if (k % 3 == 2)
            f.proc->analysisTick();
    }
    f.editor->tick (1.0f / 30.0f);
}

} // namespace

TEST_CASE ("EQ editor: the EQ row expands into the spectrum editor, others keep the panel", "[ui][eq][editor]")
{
    Fixture f;
    auto& row = f.editor->rowComponent (f.eqRow);
    REQUIRE (row.eqEditor() != nullptr);
    CHECK (row.eqEditor()->getHeight() == ui::kEditorH);
    CHECK (row.panelControlCount() == 12);

    int mbRow = -1;
    for (int r = 0; r < f.editor->rowCount(); ++r)
        if (f.editor->row (r).kind == ui::RowKind::Layer && f.editor->row (r).layer == engine::kMb)
            mbRow = r;
    f.editor->toggleExpanded (mbRow);
    CHECK (row.eqEditor() == nullptr);   // collapsed with the row
    CHECK (f.editor->rowComponent (mbRow).eqEditor() == nullptr);
    CHECK (f.editor->rowComponent (mbRow).panelControlCount() > 0);
}

TEST_CASE ("EQ editor: the band editor reaches every EQ parameter", "[ui][eq][editor]")
{
    Fixture f;
    auto& eq = f.eq();
    std::set<std::string> reached { "eq.on", "eq.depth", "eq.delta" };   // in the row itself
    for (int b = 0; b < 8; ++b)
    {
        eq.selectBand (b);
        CHECK (eq.selectedBand() == b);
        const auto ids = eq.boundParamIds();
        CHECK (ids.size() == 12);
        CHECK (ids.contains ("eq.phase"));
        for (const char* n : { "on", "type", "freq", "gain", "q", "ms", "dyn", "thresh", "range", "atk", "rel" })
            CHECK (ids.contains (juce::String (ui::eqParamId (b, n))));
        for (const auto& id : ids)
            reached.insert (id.toStdString());
    }
    std::set<std::string> all;
    for (auto* p : f.proc->getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p); rp != nullptr && rp->getParameterID().startsWith ("eq."))
            all.insert (rp->getParameterID().toStdString());
    CHECK (reached == all);
}

TEST_CASE ("EQ editor: a node drag writes freq and gain inside one gesture each", "[ui][eq][editor]")
{
    Fixture f;
    f.set ("eq.b3.on", 1.0f);
    f.set ("eq.b3.freq", 1000.0f);
    f.set ("eq.b3.gain", 0.0f);
    auto& eq = f.eq();
    eq.refresh();
    const auto pl = eq.plot();
    REQUIRE (pl.w > 200.0f);
    REQUIRE (pl.rangeDb == 6.0f);

    GestureCounter gf, gg;
    auto* pf = f.proc->apvts.getParameter ("eq.b3.freq");
    auto* pg = f.proc->apvts.getParameter ("eq.b3.gain");
    pf->addListener (&gf);
    pg->addListener (&gg);

    const auto n = ui::eqNodePos (eq.settings()[2], pl);
    eq.pointerDown (n.x, n.y);
    CHECK (eq.selectedBand() == 2);
    const float octave = static_cast<float> (ui::freqToX (2000.0, pl.w) - ui::freqToX (1000.0, pl.w));
    eq.pointerDrag (octave, -pl.h / 4.0f, false);   // +1 octave, +range/2
    CHECK (f.raw ("eq.b3.freq") == Approx (2000.0f).epsilon (0.002));
    CHECK (f.raw ("eq.b3.gain") == Approx (3.0f).margin (0.02));
    eq.pointerDrag (octave, -pl.h / 4.0f, true);    // from the drag start, Shift: 1/10
    CHECK (f.raw ("eq.b3.freq") == Approx (1000.0f * std::pow (2.0f, 0.1f)).epsilon (0.002));
    CHECK (f.raw ("eq.b3.gain") == Approx (0.3f).margin (0.02));
    eq.pointerUp();
    CHECK (gf.begins == 1);
    CHECK (gf.ends == 1);
    CHECK (gg.begins == 1);
    CHECK (gg.ends == 1);
    pf->removeListener (&gf);
    pg->removeListener (&gg);

    // A pass filter has no gain: the drag leaves it (and its gesture) alone.
    f.set ("eq.b3.type", 3.0f);   // High Pass
    f.set ("eq.b3.gain", 0.0f);
    eq.refresh();
    const auto hp = ui::eqNodePos (eq.settings()[2], pl);
    GestureCounter hg;
    pg->addListener (&hg);
    eq.pointerDown (hp.x, hp.y);
    eq.pointerDrag (0.0f, -pl.h / 4.0f, false);
    eq.pointerUp();
    CHECK (f.raw ("eq.b3.gain") == 0.0f);
    CHECK (hg.begins == 0);
    pg->removeListener (&hg);
}

TEST_CASE ("EQ editor: the range stays put during a drag, then fits", "[ui][eq][editor]")
{
    // A tick mid-drag must not rescale y under the pointer: with 6 dB shown, dragging past the
    // top keeps the 6 dB scale until release, so the same pointer offset gives the same gain.
    Fixture f;
    f.set ("eq.b3.on", 1.0f);
    f.set ("eq.b3.gain", 0.0f);
    auto& eq = f.eq();
    eq.refresh();
    const auto pl = eq.plot();
    REQUIRE (pl.rangeDb == 6.0f);
    const auto n = ui::eqNodePos (eq.settings()[2], pl);
    eq.pointerDown (n.x, n.y);
    eq.pointerDrag (0.0f, -0.75f * pl.h, false);
    CHECK (f.raw ("eq.b3.gain") == Approx (9.0f).margin (0.02));
    eq.refresh();   // the editor's tick
    CHECK (eq.plot().rangeDb == 6.0f);
    eq.pointerDrag (0.0f, -0.75f * pl.h, false);
    CHECK (f.raw ("eq.b3.gain") == Approx (9.0f).margin (0.02));
    eq.pointerUp();
    eq.refresh();
    CHECK (eq.plot().rangeDb == 12.0f);
}

TEST_CASE ("EQ editor: double-click adds a band at the pointer, or turns a node off", "[ui][eq][editor]")
{
    Fixture f;
    auto& eq = f.eq();
    f.set ("eq.b1.on", 1.0f);
    eq.refresh();
    const auto pl = eq.plot();
    const float x = static_cast<float> (ui::freqToX (440.0, pl.w)), y = ui::eqGainToY (-3.0f, pl);
    eq.pointerDoubleClick (x, y);
    CHECK (f.raw ("eq.b2.on") == 1.0f);   // band 1 was taken
    CHECK (f.raw ("eq.b2.type") == 0.0f);
    CHECK (f.raw ("eq.b2.freq") == Approx (440.0f).epsilon (0.01));
    CHECK (f.raw ("eq.b2.gain") == Approx (-3.0f).margin (0.05));
    CHECK (eq.selectedBand() == 1);

    eq.refresh();
    const auto n = ui::eqNodePos (eq.settings()[1], pl);
    eq.pointerDoubleClick (n.x, n.y);
    CHECK (f.raw ("eq.b2.on") == 0.0f);
}

TEST_CASE ("EQ editor: wheel sets Q, keys nudge the selected band, Delete turns it off", "[ui][eq][editor]")
{
    Fixture f;
    f.set ("eq.b5.on", 1.0f);
    f.set ("eq.b5.freq", 1000.0f);
    f.set ("eq.b5.gain", 0.0f);
    f.set ("eq.b5.q", 1.0f);
    auto& eq = f.eq();
    eq.refresh();
    const auto n = ui::eqNodePos (eq.settings()[4], eq.plot());
    eq.wheel (n.x, n.y, 1.0f, false);
    CHECK (f.raw ("eq.b5.q") == Approx (2.0f).epsilon (0.002));
    CHECK (eq.selectedBand() == 4);

    eq.refresh();
    CHECK (eq.keyPressed (juce::KeyPress (juce::KeyPress::rightKey)));
    CHECK (f.raw ("eq.b5.freq") == Approx (1000.0f * std::pow (2.0f, 1.0f / 12.0f)).epsilon (0.002));
    eq.refresh();
    CHECK (eq.keyPressed (juce::KeyPress (juce::KeyPress::upKey, juce::ModifierKeys::shiftModifier, 0)));
    CHECK (f.raw ("eq.b5.gain") == Approx (0.1f).margin (0.005));
    eq.refresh();
    CHECK (eq.keyPressed (juce::KeyPress (juce::KeyPress::deleteKey)));
    CHECK (f.raw ("eq.b5.on") == 0.0f);

    eq.selectBand (-1);
    CHECK_FALSE (eq.keyPressed (juce::KeyPress (juce::KeyPress::upKey)));   // falls through to the stack
}

TEST_CASE ("EQ editor: range follows the gains, backdrop follows the EQ input", "[ui][eq][editor]")
{
    Fixture f;
    auto& eq = f.eq();
    eq.refresh();
    CHECK (eq.plot().rangeDb == 6.0f);
    f.set ("eq.b1.on", 1.0f);
    f.set ("eq.b1.gain", 9.0f);
    eq.refresh();
    CHECK (eq.plot().rangeDb == 12.0f);

    for (float v : eq.spectrum())
        CHECK (v == analysis::kImpactLevelFloorDb);   // nothing analysed yet

    f.set ("eq.on", 1.0f);
    feed (f, 3);
    // White noise at -26 dBFS RMS per channel: a 1/6-octave band near 1 kHz holds roughly
    // -26 + 10 log10 (bandwidth / 24 kHz) ~ -50 dB. Only sanity-bound it: real, not floor.
    const float v1k = eq.spectrum()[34];
    CHECK (v1k > -70.0f);
    CHECK (v1k < -30.0f);
}

TEST_CASE ("EQ editor: a headless render paints the selected node in the signal colour", "[ui][eq][editor][render]")
{
    Fixture f;
    f.set ("eq.on", 1.0f);
    f.set ("eq.b1.on", 1.0f);
    f.set ("eq.b1.type", 3.0f);    // High Pass, 30 Hz
    f.set ("eq.b4.on", 1.0f);
    f.set ("eq.b4.freq", 500.0f);
    f.set ("eq.b4.gain", 3.0f);
    f.set ("eq.b7.on", 1.0f);      // side-only shelf: dimmed in the stereo view
    f.set ("eq.b7.type", 2.0f);
    f.set ("eq.b7.gain", -2.0f);
    f.set ("eq.b7.ms", 2.0f);
    f.editor->setSize (1400, 900);
    feed (f, 2);
    auto& eq = f.eq();
    eq.refresh();
    eq.selectBand (3);
    const auto img = eq.createComponentSnapshot (eq.getLocalBounds(), true, 1.0f);
    const auto c = eq.canvasBounds();
    const auto n = ui::eqNodePos (eq.settings()[3], eq.plot());
    CHECK (img.getPixelAt (c.getX() + juce::roundToInt (n.x), c.getY() + juce::roundToInt (n.y)) == ui::theme::kSignal);

    const auto out = juce::File (STRAT3_CANDIDATE_DIR).getChildFile ("strat3-eq-editor.png");
    out.deleteFile();
    juce::FileOutputStream os (out);
    REQUIRE (os.openedOk());
    CHECK (juce::PNGImageFormat().writeImageToStream (img, os));
}
