#include "ui/Strat3Editor.h"

#include "analysis/Loudness1770.h"
#include "processor/Strat3Processor.h"

namespace strat3::ui {

/** The stack's scrolled content: rows, plus the drop marker while a layer is dragged. */
class Strat3Editor::StackContent final : public juce::Component
{
public:
    int dropY = -1;
    void paint (juce::Graphics& g) override { g.fillAll (theme::kBase); }
    void paintOverChildren (juce::Graphics& g) override
    {
        if (dropY >= 0)
        {
            g.setColour (theme::kSignal);
            g.fillRect (0, dropY - 1, getWidth(), 2);
        }
    }
};

Strat3Editor::Strat3Editor (processor::Strat3Processor& p)
    : AudioProcessorEditor (p),
      proc (p),
      header (p.apvts),
      stackContent (std::make_unique<StackContent>())
{
    setLookAndFeel (&laf);
    setTitle ("STRAT3");
    setWantsKeyboardFocus (true);

    stackView.setViewedComponent (stackContent.get(), false);
    stackView.setScrollBarsShown (true, false);
    addAndMakeVisible (header);
    addAndMakeVisible (stackView);
    addAndMakeVisible (delivery);
    delivery.onClearClips = [this] { clickClipLeds(); };
    delivery.onClearNonFinite = [this] { proc.clearNonFiniteFlag(); };

    rebuildRows (proc.preparedOrder());

    setResizable (true, true);
    setResizeLimits (kMinW, kMinH, kMaxW, kMaxH);
    setSize (kDefaultW, kDefaultH);
    startTimerHz (30);
}

Strat3Editor::~Strat3Editor()
{
    stopTimer();
    rows.clear();
    setLookAndFeel (nullptr);
}

void Strat3Editor::paint (juce::Graphics& g) { g.fillAll (theme::kBase); }

void Strat3Editor::resized()
{
    const auto l = computeLayout (getWidth(), getHeight(), false);
    header.setBounds (l.header.x, l.header.y, l.header.w, l.header.h);
    stackView.setBounds (l.stack.x, l.stack.y, l.stack.w, l.stack.h);
    delivery.setBounds (l.delivery.x, l.delivery.y, l.delivery.w, l.delivery.h);
    layoutRows();
}

void Strat3Editor::rebuildRows (const engine::LayerOrder& order)
{
    const int keepSelected = selected;
    rows.clear();
    expanded = -1;
    shownOrder = order;
    for (const auto& info : rowsFor (order))
    {
        auto row = std::make_unique<LayerRow> (proc.apvts, info);
        LayerRow* raw = row.get();
        const int index = static_cast<int> (rows.size());
        row->onSelect = [this, index] { select (index); };
        row->onToggleExpand = [this, index] { toggleExpanded (index); };
        row->onReorderDrag = [this] (int, int y)
        {
            std::vector<Rect> rects;
            for (auto& r : rows)
                rects.push_back ({ r->getX(), r->getY(), r->getWidth(), r->getHeight() });
            const int slot = dropSlot (rects, y);
            stackContent->dropY = rows[static_cast<std::size_t> (1 + slot)]->getY();
            stackContent->repaint();
        };
        row->onReorderDrop = [this] (int from, int y)
        {
            std::vector<Rect> rects;
            for (auto& r : rows)
                rects.push_back ({ r->getX(), r->getY(), r->getWidth(), r->getHeight() });
            stackContent->dropY = -1;
            stackContent->repaint();
            dropLayer (from, dropSlot (rects, y));
        };
        if (info.kind == RowKind::OutTrim && raw->linkButton() != nullptr)
        {
            raw->linkButton()->setToggleState (trimsLinked, juce::dontSendNotification);
            raw->linkButton()->onClick = [this, raw] { trimsLinked = raw->linkButton()->getToggleState(); };
        }
        stackContent->addAndMakeVisible (*row);
        rows.push_back (std::move (row));
    }
    // IN TRIM drives OUT TRIM when linked (the out row exists by now).
    rows.front()->handle().onUserChange = [this] (float before, float after)
    {
        if (! trimsLinked || before == after)
            return;
        if (auto* out = proc.apvts.getParameter ("global.outTrim"))
        {
            const float v = linkedOutTrim (after, before, out->convertFrom0to1 (out->getValue()));
            out->beginChangeGesture();
            out->setValueNotifyingHost (out->convertTo0to1 (v));
            out->endChangeGesture();
        }
    };
    select (keepSelected < rowCount() ? keepSelected : -1);
    layoutRows();
}

void Strat3Editor::layoutRows()
{
    if (rows.empty())
        return;
    const int w = stackView.getWidth();
    const Rect stack { 0, 0, w, stackView.getHeight() };
    auto rects = stackRows (stack, rowCount(), expanded);
    const int h = contentHeight (rects, stack);
    const int contentW = h > stack.h ? w - stackView.getScrollBarThickness() : w;
    for (std::size_t i = 0; i < rows.size(); ++i)
        rows[i]->setBounds (rects[i].x, rects[i].y, contentW, rects[i].h);
    stackContent->setSize (contentW, std::max (h, stack.h));
}

void Strat3Editor::select (int r)
{
    selected = r;
    for (std::size_t i = 0; i < rows.size(); ++i)
        rows[i]->setSelected (static_cast<int> (i) == r);
}

void Strat3Editor::toggleExpanded (int r)
{
    if (r < 0 || r >= rowCount() || ! rows[static_cast<std::size_t> (r)]->expandable())
        return;
    const int next = expanded == r ? -1 : r;
    if (expanded >= 0)
        rows[static_cast<std::size_t> (expanded)]->setExpanded (false);
    expanded = next;
    if (expanded >= 0)
        rows[static_cast<std::size_t> (expanded)]->setExpanded (true);
    layoutRows();
}

void Strat3Editor::dropLayer (int fromSlot, int toSlot)
{
    const auto base = processor::engineOrder (proc.layerOrder());   // a pending order counts
    const auto next = moveLayer (base, fromSlot, toSlot);
    if (next != base)
        proc.setLayerOrder (orderToIds (next));
}

void Strat3Editor::toggleParam (const juce::String& id)
{
    if (auto* p = proc.apvts.getParameter (id))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->getValue() > 0.5f ? 0.0f : 1.0f);
        p->endChangeGesture();
    }
}

bool Strat3Editor::keyPressed (const juce::KeyPress& key)
{
    const bool shift = key.getModifiers().isShiftDown();
    const int code = key.getKeyCode();
    if (code == juce::KeyPress::upKey || code == juce::KeyPress::downKey)
    {
        select (nextRow (selected, code == juce::KeyPress::downKey ? 1 : -1, rowCount()));
        if (selected >= 0)
            stackView.setViewPosition (0, std::max (0, std::min (stackView.getViewPositionY(), rows[static_cast<std::size_t> (selected)]->getY())));
        return true;
    }
    if ((code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey) && selected >= 0)
    {
        rows[static_cast<std::size_t> (selected)]->handle().step (code == juce::KeyPress::rightKey ? 1 : -1, shift);
        return true;
    }
    if (code == juce::KeyPress::returnKey)
    {
        toggleExpanded (selected);
        return true;
    }
    const auto upper = juce::CharacterFunctions::toUpperCase (static_cast<juce::juce_wchar> (code));
    if (upper == 'D')
    {
        toggleParam ("global.delta");
        return true;
    }
    if (upper == 'G')
    {
        toggleParam ("global.gainMatch");
        return true;
    }
    return false;
}

void Strat3Editor::clickClipLeds()
{
    proc.clearClips();
    inClip = outClip = false;
    auto r = delivery.shown();
    r.inClip = r.outClip = false;
    delivery.show (r);
}

void Strat3Editor::setTrimsLinked (bool linked)
{
    trimsLinked = linked;
    for (auto& r : rows)
        if (auto* b = r->linkButton())
            b->setToggleState (linked, juce::dontSendNotification);
}

void Strat3Editor::updateMeters (float dt, bool text)
{
    const auto pk = proc.takePeaks();
    for (std::size_t c = 0; c < 2; ++c)
    {
        inBar[c] = meterFall (inBar[c], pk.inPeakDb[c], dt);
        outBar[c] = meterFall (outBar[c], pk.outPeakDb[c], dt);
    }
    update (tpHold, pk.outTruePeakDb, dt);
    update (grHold, -pk.grDb, dt);   // held as a positive amount
    inClip = pk.inClip;
    outClip = pk.outClip;

    auto r = delivery.shown();
    r.inDb = inBar;
    r.outDb = outBar;
    r.inClip = inClip;
    r.outClip = outClip;
    if (text)
    {
        const auto m = proc.meters();
        using analysis::Platform;
        const float gr = grHold.db > 0.0f ? -grHold.db : 0.0f;
        static constexpr const char* modeNames[] { "TRACK", "MIX", "MASTER" };
        r.line1 = juce::String ("LUFS-I ") + formatLufs (m.integratedLufs) + "  S " + formatLufs (m.shortTermLufs)
                  + "  M " + formatLufs (m.momentaryLufs) + " | TP " + formatDb (tpHold.db, 1, false)
                  + " | PLR " + formatDb (m.plr, 1, false) + " | CORR " + formatDb (m.correlation, 2, true)
                  + " | GR " + formatDb (gr, 1, false);
        r.line2 = juce::String ("SPOTIFY ") + formatDb (analysis::platformPenaltyDb (Platform::Spotify, m.integratedLufs), 1, true)
                  + "  APPLE " + formatDb (analysis::platformPenaltyDb (Platform::AppleMusic, m.integratedLufs), 1, true)
                  + "  YT " + formatDb (analysis::platformPenaltyDb (Platform::YouTube, m.integratedLufs), 1, true)
                  + " | CPU " + formatCpu (m.cpuLoad) + " | LATENCY " + formatLatency (proc.getLatencySamples(), proc.getEngine().config.sampleRate)
                  + " " + modeNames[static_cast<int> (proc.preparedMode())]
                  + (proc.configPending() ? " (applying)" : "")
                  + (m.nonFinite ? " | NON-FINITE INPUT MUTED (click to clear)" : "");
        r.nonFinite = m.nonFinite;
    }
    delivery.show (r);
}

void Strat3Editor::tick (float dt)
{
    if (proc.preparedOrder() != shownOrder)
        rebuildRows (proc.preparedOrder());

    if (proc.readImpact (frame))
        for (auto& r : rows)
            if (r->info().impactRow < frame.layers)
                r->setImpact (frame.gainDb[static_cast<std::size_t> (r->info().impactRow)],
                              frame.widthDb[static_cast<std::size_t> (r->info().impactRow)]);

    const auto lat = engine::rowLatencies (proc.getEngine());
    const double fs = proc.getEngine().config.sampleRate;   // the host may not have set getSampleRate()
    for (auto& r : rows)
    {
        const auto& info = r->info();
        if (info.kind == RowKind::Ceiling)
            r->setStats ("LOCKED " + juce::String (formatLatency (lat[static_cast<std::size_t> (info.impactRow)], fs)));
        else if (info.kind == RowKind::Layer)
            r->setStats ("LAT " + juce::String (formatLatency (lat[static_cast<std::size_t> (info.impactRow)], fs)));
        r->refresh();
    }

    updateMeters (dt, frameCount++ % 2 == 0);   // bars at 30 Hz, numbers at 15 Hz (Phase 4)
}

} // namespace strat3::ui
