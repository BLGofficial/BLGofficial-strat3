#include "ui/native/LayerRow.h"

#include "ui/native/Theme.h"

namespace strat3::ui {

namespace {

constexpr int kCellW = 180, kCellH = 40, kGap = 8;
constexpr int kMarkerW = 4, kButtonW = 28, kButtonH = 18;

juce::String deltaGlyph() { return juce::String::fromUTF8 ("\xce\x94"); }   // the Greek capital delta

} // namespace

// ------------------------------------------------------------------------------ panel

struct LayerPanel::Cell
{
    juce::Label caption;
    std::unique_ptr<juce::Component> control;
    std::unique_ptr<juce::ButtonParameterAttachment> button;
    std::unique_ptr<juce::ComboBoxParameterAttachment> combo;
    std::unique_ptr<juce::SliderParameterAttachment> slider;
};

LayerPanel::LayerPanel (juce::AudioProcessorValueTreeState& state, const juce::String& prefix, const juce::StringArray& exclude)
{
    for (auto* raw : state.processor.getParameters())
    {
        auto* p = dynamic_cast<juce::RangedAudioParameter*> (raw);
        if (p == nullptr || ! p->getParameterID().startsWith (prefix + ".") || exclude.contains (p->getParameterID()))
            continue;
        auto c = std::make_unique<Cell>();
        c->caption.setText (p->getName (40).toUpperCase(), juce::dontSendNotification);
        c->caption.setFont (theme::label (10.0f));
        c->caption.setColour (juce::Label::textColourId, theme::kSecondary);
        c->caption.setInterceptsMouseClicks (false, false);
        if (auto* b = dynamic_cast<juce::AudioParameterBool*> (p))
        {
            auto t = std::make_unique<juce::ToggleButton> ("ON");
            c->button = std::make_unique<juce::ButtonParameterAttachment> (*b, *t);
            c->control = std::move (t);
        }
        else if (auto* ch = dynamic_cast<juce::AudioParameterChoice*> (p))
        {
            auto box = std::make_unique<juce::ComboBox>();
            box->addItemList (ch->choices, 1);
            c->combo = std::make_unique<juce::ComboBoxParameterAttachment> (*ch, *box);
            c->control = std::move (box);
        }
        else
        {
            auto s = std::make_unique<juce::Slider> (juce::Slider::LinearBar, juce::Slider::TextBoxRight);
            s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, kCellH / 2);
            c->slider = std::make_unique<juce::SliderParameterAttachment> (*p, *s);
            c->control = std::move (s);
        }
        c->control->setTitle (p->getName (64));
        content.addAndMakeVisible (c->caption);
        content.addAndMakeVisible (*c->control);
        cells.push_back (std::move (c));
    }
    view.setViewedComponent (&content, false);
    view.setScrollBarsShown (true, false);
    addAndMakeVisible (view);
}

void LayerPanel::resized()
{
    view.setBounds (getLocalBounds());
    const int w = std::max (kCellW, getWidth() - view.getScrollBarThickness());
    const int cols = std::max (1, (w - kGap) / (kCellW + kGap));
    const int rows = (static_cast<int> (cells.size()) + cols - 1) / cols;
    content.setSize (w, kGap + rows * (kCellH + kGap));
    for (std::size_t i = 0; i < cells.size(); ++i)
    {
        const int col = static_cast<int> (i) % cols, row = static_cast<int> (i) / cols;
        const juce::Rectangle<int> r (kGap + col * (kCellW + kGap), kGap + row * (kCellH + kGap), kCellW, kCellH);
        cells[i]->caption.setBounds (r.withHeight (14));
        cells[i]->control->setBounds (r.withTrimmedTop (16));
    }
}

void LayerPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::kBase);
    g.setColour (theme::kRule);
    g.fillRect (0, 0, getWidth(), 1);
}

// ------------------------------------------------------------------------------ row

LayerRow::LayerRow (juce::AudioProcessorValueTreeState& s, RowInfo info)
    : state (s), rowInfo (info)
{
    levels.fill (analysis::kImpactLevelFloorDb);   // "nothing analysed yet", not 0 dB
    auto param = [this] (const juce::String& id) -> juce::RangedAudioParameter&
    {
        auto* p = state.getParameter (id);
        jassert (p != nullptr);
        return *p;
    };
    auto dbText = [] (float v) { return juce::String (formatDb (v, 1, true)); };

    switch (rowInfo.kind)
    {
        case RowKind::InTrim:
        case RowKind::OutTrim:
            handleId = rowInfo.kind == RowKind::InTrim ? "global.inTrim" : "global.outTrim";
            valueHandle = std::make_unique<ValueHandle> (param (handleId), 0.5f, 0.1f);
            valueHandle->format = dbText;
            if (rowInfo.kind == RowKind::OutTrim)
            {
                link = std::make_unique<juce::ToggleButton> ("LINK");
                link->setTitle ("Link output trim to input trim");
                addAndMakeVisible (*link);
            }
            break;
        case RowKind::Ceiling:
            handleId = "ceiling.dBTP";
            panelPrefix = "ceiling";
            valueHandle = std::make_unique<ValueHandle> (param (handleId), 0.1f, 0.01f);
            valueHandle->format = [] (float v) { return juce::String (formatDb (v, 1, false)); };
            onParam = &param ("ceiling.on");
            break;
        case RowKind::Layer:
        {
            panelPrefix = paramPrefix (rowInfo.layer);
            handleId = panelPrefix + ".depth";
            valueHandle = std::make_unique<ValueHandle> (param (handleId), 1.0f, kDepthFine);
            onParam = &param (panelPrefix + ".on");
            delta = std::make_unique<juce::ToggleButton> (deltaGlyph());
            delta->setTitle (juce::String (layerSpokenName (rowInfo.layer)) + " delta");
            deltaAttach = std::make_unique<juce::ButtonParameterAttachment> (param (panelPrefix + ".delta"), *delta);
            addAndMakeVisible (*delta);
            break;
        }
    }
    if (onParam != nullptr)
    {
        on = std::make_unique<juce::ToggleButton> ("ON");
        on->setTitle (name() + " on");
        onAttach = std::make_unique<juce::ButtonParameterAttachment> (*onParam, *on);
        addAndMakeVisible (*on);
    }
    addAndMakeVisible (*valueHandle);
    refresh();
}

LayerRow::~LayerRow() = default;

juce::String LayerRow::name() const
{
    switch (rowInfo.kind)
    {
        case RowKind::InTrim: return "IN TRIM";
        case RowKind::OutTrim: return "OUT TRIM";
        case RowKind::Ceiling: return "CEILING";
        case RowKind::Layer: break;
    }
    return layerLabel (rowInfo.layer);
}

bool LayerRow::active() const { return onParam != nullptr && onParam->getValue() > 0.5f; }

void LayerRow::setExpanded (bool shouldExpand)
{
    if (shouldExpand && eqEd == nullptr && rowInfo.kind == RowKind::Layer && rowInfo.layer == engine::kEq)
    {
        eqEd = std::make_unique<EqEditor> (state);
        eqEd->setSpectrum (levels);
        addAndMakeVisible (*eqEd);
    }
    else if (shouldExpand && panel == nullptr && eqEd == nullptr && expandable())
    {
        juce::StringArray exclude { handleId };
        if (onParam != nullptr)
            exclude.add (onParam->getParameterID());
        exclude.add (panelPrefix + ".delta");
        panel = std::make_unique<LayerPanel> (state, panelPrefix, exclude);
        addAndMakeVisible (*panel);
    }
    else if (! shouldExpand)
    {
        panel.reset();
        eqEd.reset();
    }
    resized();
}

void LayerRow::setSpectrum (const std::array<float, analysis::kImpactBands>& levelDb)
{
    levels = levelDb;
    if (eqEd != nullptr)
        eqEd->setSpectrum (levelDb);
}

void LayerRow::setSelected (bool s)
{
    if (selected != s)
    {
        selected = s;
        repaint();
    }
}

void LayerRow::setImpact (const std::array<float, analysis::kImpactBands>& gainDb, const std::array<float, analysis::kImpactBands>& widthDb)
{
    // Off: the layer outputs its band-split reference (an all-pass). Its magnitude change is
    // zero; what the analyser reads there is group delay at low frequencies, not impact.
    const bool off = onParam != nullptr && ! active();
    for (std::size_t b = 0; b < cells.size(); ++b)
        cells[b] = off ? 0.0f : impactValue (gainDb[b], widthDb[b]);
    const auto parts = rowParts ({ 0, 0, getWidth(), getHeight() });
    repaint (parts.strip.x, parts.strip.y, parts.strip.w, parts.strip.h);
}

void LayerRow::setStats (const juce::String& text)
{
    if (stats != text)
    {
        stats = text;
        const auto parts = rowParts ({ 0, 0, getWidth(), getHeight() });
        repaint (parts.stats.x, parts.stats.y, parts.stats.w, parts.stats.h);
    }
}

void LayerRow::refresh()
{
    const float v = valueHandle->value();
    juce::String title;
    switch (rowInfo.kind)
    {
        case RowKind::InTrim: title = "Input trim, " + juce::String (formatDb (v, 1, true)) + " dB"; break;
        case RowKind::OutTrim: title = "Output trim, " + juce::String (formatDb (v, 1, true)) + " dB"; break;
        case RowKind::Ceiling: title = "Ceiling, " + juce::String (formatDb (v, 1, false)) + " dBTP" + (active() ? "" : ", off"); break;
        case RowKind::Layer: title = accessibleLayerLabel (rowInfo.slot, rowInfo.layer, v, active()); break;
    }
    if (valueHandle->getTitle() != title)
        valueHandle->setTitle (title);
    valueHandle->setAccent (active());
    repaint (0, 0, kMarkerW, kRowH);
    if (eqEd != nullptr)
        eqEd->refresh();   // host automation and undo move the nodes too
}

void LayerRow::paint (juce::Graphics& g)
{
    const auto parts = rowParts ({ 0, 0, getWidth(), getHeight() });
    g.setColour (selected ? theme::kRowSelected : theme::kRow);
    g.fillRect (0, 0, getWidth(), kRowH);

    // Active marker (the wireframe's left bar). Off layers keep a 1 px outline: shape, not
    // only colour, carries the state.
    if (active())
    {
        g.setColour (theme::kSignal);
        g.fillRect (0, 0, kMarkerW, kRowH);
    }
    else if (onParam != nullptr)
    {
        g.setColour (theme::kSecondary);
        g.drawRect (0, 0, kMarkerW, kRowH, 1);
    }

    g.setColour (rowInfo.kind == RowKind::Layer || rowInfo.kind == RowKind::Ceiling ? theme::kText : theme::kSecondary);
    g.setFont (theme::label (12.0f, true));
    g.drawText (name(), kMarkerW + 8, 4, parts.label.w - kMarkerW - 8 - 2 * (kButtonW + 4), kButtonH, juce::Justification::centredLeft, true);

    // Impact strip: a 1/6-octave cell per band. Boost = filled, cut = outlined (Phase 4: shape
    // as well as colour), intensity = magnitude.
    const auto strip = juce::Rectangle<int> (parts.strip.x, parts.strip.y, parts.strip.w, parts.strip.h).reduced (0, 8);
    g.setColour (theme::kBase);
    g.fillRect (strip);
    g.setColour (theme::kRule);
    for (double hz : kFreqTicks)
        g.fillRect (strip.getX() + static_cast<int> (freqToX (hz, strip.getWidth())), strip.getY(), 1, strip.getHeight());
    for (int b = 0; b < analysis::kImpactBands; ++b)
    {
        const auto cell = impactCell (cells[static_cast<std::size_t> (b)]);
        if (cell.intensity <= 0.0f)
            continue;
        const auto span = bandSpan (b, strip.getWidth());
        const juce::Rectangle<float> r (static_cast<float> (strip.getX() + span.x0) + 0.5f, static_cast<float> (strip.getY()) + 0.5f,
                                        static_cast<float> (span.x1 - span.x0) - 1.0f, static_cast<float> (strip.getHeight()) - 1.0f);
        g.setColour (theme::kSignal.withAlpha (0.25f + 0.75f * cell.intensity));
        if (cell.boost)
            g.fillRect (r);
        else
            g.drawRect (r, 1.0f);
    }

    g.setColour (theme::kSecondary);
    g.setFont (theme::mono (11.0f));
    g.drawText (stats, parts.stats.x, parts.stats.y, parts.stats.w - 8, parts.stats.h, juce::Justification::centredRight, false);

    g.setColour (theme::kRule);
    g.fillRect (0, kRowH - 1, getWidth(), 1);
}

void LayerRow::resized()
{
    const auto parts = rowParts ({ 0, 0, getWidth(), getHeight() });
    int bx = parts.label.right() - 8 - kButtonW;
    for (auto* b : { on.get(), delta.get(), link.get() })
        if (b != nullptr)
        {
            b->setBounds (bx, 4, kButtonW, kButtonH);
            bx -= kButtonW + 4;
        }
    valueHandle->setBounds (kMarkerW + 8, 28, parts.label.w - kMarkerW - 16, 18);
    if (panel != nullptr)
        panel->setBounds (parts.editor.x, parts.editor.y, parts.editor.w, parts.editor.h);
    if (eqEd != nullptr)
        eqEd->setBounds (parts.editor.x, parts.editor.y, parts.editor.w, parts.editor.h);
}

void LayerRow::mouseDown (const juce::MouseEvent& e)
{
    if (onSelect)
        onSelect();
    // Reorder grab: the name line of a layer row (trims and ceiling never move).
    reordering = rowInfo.kind == RowKind::Layer && e.y < 26 && e.x < kLabelW;
}

void LayerRow::mouseDrag (const juce::MouseEvent& e)
{
    if (reordering && onReorderDrag && std::abs (e.getDistanceFromDragStartY()) > 4)
        onReorderDrag (rowInfo.slot, getY() + e.y);
}

void LayerRow::mouseUp (const juce::MouseEvent& e)
{
    if (reordering && onReorderDrop && std::abs (e.getDistanceFromDragStartY()) > 4)
        onReorderDrop (rowInfo.slot, getY() + e.y);
    reordering = false;
}

void LayerRow::mouseDoubleClick (const juce::MouseEvent&)
{
    if (expandable() && onToggleExpand)
        onToggleExpand();
}

} // namespace strat3::ui
