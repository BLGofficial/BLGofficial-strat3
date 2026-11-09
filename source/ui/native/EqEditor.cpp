#include "ui/native/EqEditor.h"

#include <cmath>

#include "ui/native/Theme.h"

namespace strat3::ui {

namespace {

constexpr int kPad = 8, kCaptionH = 14, kBandLabelW = 64, kViewW = 28;
constexpr float kNodeR = 6.0f;
// JUCE wheel deltas are ~0.15 per detent on most platforms [INFERRED from juce_MouseEvent docs
// and observed values; trackpads send many small deltas, which this scales smoothly].
constexpr float kWheelPerNotch = 0.15f;

struct BandParam { const char* name; const char* caption; };
constexpr BandParam kBandParams[] {
    { "on", "ON" }, { "type", "TYPE" }, { "freq", "FREQ" }, { "gain", "GAIN" }, { "q", "Q" },
    { "ms", "CH" }, { "dyn", "DYN" }, { "thresh", "THR" }, { "range", "RNG" }, { "atk", "ATK" }, { "rel", "REL" },
};

} // namespace

// One value box of the band editor (ARCHON reference: caption above a value control).
struct EqEditor::Box
{
    juce::String id;
    juce::Label caption;
    std::unique_ptr<juce::Component> control;
    std::unique_ptr<juce::ButtonParameterAttachment> button;
    std::unique_ptr<juce::ComboBoxParameterAttachment> combo;
    std::unique_ptr<juce::SliderParameterAttachment> slider;

    Box (juce::RangedAudioParameter& p, const juce::String& text, const std::string& name) : id (p.getParameterID())
    {
        caption.setText (text, juce::dontSendNotification);
        caption.setFont (theme::label (10.0f));
        caption.setColour (juce::Label::textColourId, theme::kSecondary);
        caption.setInterceptsMouseClicks (false, false);
        if (auto* b = dynamic_cast<juce::AudioParameterBool*> (&p))
        {
            auto t = std::make_unique<juce::ToggleButton> ("ON");
            button = std::make_unique<juce::ButtonParameterAttachment> (*b, *t);
            control = std::move (t);
        }
        else if (auto* ch = dynamic_cast<juce::AudioParameterChoice*> (&p))
        {
            auto box = std::make_unique<juce::ComboBox>();
            box->addItemList (ch->choices, 1);
            combo = std::make_unique<juce::ComboBoxParameterAttachment> (*ch, *box);
            control = std::move (box);
        }
        else
        {
            auto s = std::make_unique<juce::Slider> (juce::Slider::LinearBar, juce::Slider::TextBoxRight);
            s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 56, 20);
            slider = std::make_unique<juce::SliderParameterAttachment> (p, *s);
            // After the attachment: it installs the parameter's own (unrounded) text functions.
            if (! eqValueText (name, 0.0f).empty())
            {
                s->textFromValueFunction = [name] (double v) { return juce::String (eqValueText (name, static_cast<float> (v))); };
                s->updateText();
            }
            control = std::move (s);
        }
        control->setTitle (p.getName (64));
    }
};

EqEditor::EqEditor (juce::AudioProcessorValueTreeState& s) : state (s)
{
    setTitle ("EQ editor");
    setWantsKeyboardFocus (true);
    levels.fill (analysis::kImpactLevelFloorDb);

    static constexpr const char* viewNames[] { "ST", "M", "S" };
    static constexpr const char* viewTitles[] { "Show stereo curve", "Show mid curve", "Show side curve" };
    for (std::size_t i = 0; i < viewButtons.size(); ++i)
    {
        auto b = std::make_unique<juce::TextButton> (viewNames[i]);
        b->setTitle (viewTitles[i]);
        b->setClickingTogglesState (true);
        b->setRadioGroupId (1);
        b->setToggleState (i == 0, juce::dontSendNotification);
        b->onClick = [this, i] { setView (static_cast<dsp::EqChannel> (i)); };
        addAndMakeVisible (*b);
        viewButtons[i] = std::move (b);
    }
    bandLabel.setFont (theme::label (11.0f, true));
    bandLabel.setColour (juce::Label::textColourId, theme::kText);
    addAndMakeVisible (bandLabel);

    if (auto* p = state.getParameter ("eq.phase"))
    {
        phaseBox = std::make_unique<Box> (*p, "PHASE", "phase");
        addAndMakeVisible (phaseBox->caption);
        addAndMakeVisible (*phaseBox->control);
    }
    refresh();
    selectBand (0);
}

EqEditor::~EqEditor()
{
    pointerUp();   // a drag cut short by a collapse must still close its gestures
}

juce::RangedAudioParameter* EqEditor::param (int band, const char* name) const
{
    return state.getParameter (juce::String (eqParamId (band, name)));
}

void EqEditor::setAsGesture (juce::RangedAudioParameter* p, float value)
{
    if (p == nullptr)
        return;
    p->beginChangeGesture();
    p->setValueNotifyingHost (p->convertTo0to1 (value));
    p->endChangeGesture();
}

void EqEditor::refresh()
{
    const auto raw = [this] (int b, const char* n)
    {
        auto* v = state.getRawParameterValue (juce::String (eqParamId (b, n)));
        return v != nullptr ? v->load() : 0.0f;
    };
    const auto before = bands;
    const float rangeBefore = rangeDb;
    for (int b = 0; b < dsp::kEqBands; ++b)
    {
        auto& d = bands[static_cast<std::size_t> (b)];
        d.on = raw (b, "on") > 0.5f;
        d.type = static_cast<dsp::EqType> (juce::roundToInt (raw (b, "type")));
        d.freq = raw (b, "freq");
        d.gainDb = raw (b, "gain");
        d.q = raw (b, "q");
        d.channel = static_cast<dsp::EqChannel> (juce::roundToInt (raw (b, "ms")));
        d.dyn = static_cast<dsp::EqDynMode> (juce::roundToInt (raw (b, "dyn")));
        d.threshDb = raw (b, "thresh");
        d.rangeDb = raw (b, "range");
        d.attackMs = raw (b, "atk");
        d.releaseMs = raw (b, "rel");
    }
    // Frozen while dragging: a range change mid-drag would rescale y under the pointer.
    if (dragBand < 0)
        rangeDb = eqAutoRange (bands);
    if (selected >= 0)
        setDescription (juce::String (eqAccessibleLabel (selected, bands[static_cast<std::size_t> (selected)])));
    if (bands != before || rangeDb != rangeBefore)   // 30 Hz tick: repaint only on change
        repaint (canvas);
}

void EqEditor::setSpectrum (const std::array<float, analysis::kImpactBands>& levelDb)
{
    levels = levelDb;
    repaint (canvas);
}

void EqEditor::selectBand (int band)
{
    band = std::clamp (band, -1, dsp::kEqBands - 1);
    if (band == selected && (band < 0 || ! boxes.empty()))
        return;
    selected = band;
    rebind();
    refresh();
}

void EqEditor::setView (dsp::EqChannel c)
{
    viewChannel = c;
    viewButtons[static_cast<std::size_t> (c)]->setToggleState (true, juce::dontSendNotification);
    repaint();
}

void EqEditor::rebind()
{
    for (auto& b : boxes)
    {
        removeChildComponent (&b->caption);
        removeChildComponent (b->control.get());
    }
    boxes.clear();
    if (selected >= 0)
        for (const auto& bp : kBandParams)
            if (auto* p = param (selected, bp.name))
            {
                auto box = std::make_unique<Box> (*p, bp.caption, bp.name);
                addAndMakeVisible (box->caption);
                addAndMakeVisible (*box->control);
                boxes.push_back (std::move (box));
            }
    bandLabel.setText (selected >= 0 ? "BAND " + juce::String (selected + 1) : "NO BAND", juce::dontSendNotification);
    resized();
}

juce::StringArray EqEditor::boundParamIds() const
{
    juce::StringArray ids;
    for (const auto& b : boxes)
        ids.add (b->id);
    if (phaseBox != nullptr)
        ids.add (phaseBox->id);
    return ids;
}

EqPlot EqEditor::plot() const noexcept
{
    return { static_cast<float> (canvas.getWidth()), static_cast<float> (canvas.getHeight()), rangeDb };
}

// ------------------------------------------------------------------------------ pointer

void EqEditor::pointerDown (float x, float y)
{
    pointerUp();
    const int hit = eqHitTest (bands, plot(), x, y, selected);
    if (hit < 0)
        return;   // empty canvas keeps the selection: the band editor stays where it was
    selectBand (hit);
    dragBand = hit;
    dragStart = bands[static_cast<std::size_t> (hit)];
    downAt = { x, y };
    if (auto* p = param (hit, "freq"))
    {
        p->beginChangeGesture();
        gestureFreq = true;
    }
    if (dsp::supportsGain (dragStart.type))
        if (auto* p = param (hit, "gain"))
        {
            p->beginChangeGesture();
            gestureGain = true;
        }
}

void EqEditor::pointerDrag (float dx, float dy, bool fine)
{
    if (dragBand < 0)
        return;
    const auto b = eqDragNode (dragStart, plot(), dx, dy, fine);
    if (gestureFreq)
        if (auto* p = param (dragBand, "freq"))
            p->setValueNotifyingHost (p->convertTo0to1 (b.freq));
    if (gestureGain)
        if (auto* p = param (dragBand, "gain"))
            p->setValueNotifyingHost (p->convertTo0to1 (b.gainDb));
    bands[static_cast<std::size_t> (dragBand)] = b;
    repaint (canvas);
}

void EqEditor::pointerUp()
{
    if (dragBand >= 0)
    {
        if (gestureFreq)
            if (auto* p = param (dragBand, "freq"))
                p->endChangeGesture();
        if (gestureGain)
            if (auto* p = param (dragBand, "gain"))
                p->endChangeGesture();
    }
    dragBand = -1;
    gestureFreq = gestureGain = false;
}

void EqEditor::pointerDoubleClick (float x, float y)
{
    pointerUp();
    const int hit = eqHitTest (bands, plot(), x, y, selected);
    if (hit >= 0)
    {
        setAsGesture (param (hit, "on"), 0.0f);
        refresh();
        return;
    }
    const int free = eqFirstFreeBand (bands);
    if (free < 0)
        return;   // all 8 in use: nothing to add (no message: the nodes show why)
    const auto b = eqPlaceBand (bands[static_cast<std::size_t> (free)], plot(), x, y);
    setAsGesture (param (free, "type"), static_cast<float> (b.type));
    setAsGesture (param (free, "freq"), b.freq);
    setAsGesture (param (free, "gain"), b.gainDb);
    setAsGesture (param (free, "on"), 1.0f);   // last, so the band appears already in place
    selectBand (free);
    refresh();
}

void EqEditor::wheel (float x, float y, float notches, bool fine)
{
    const int hit = eqHitTest (bands, plot(), x, y, selected);
    const int target = hit >= 0 ? hit : selected;
    if (target < 0 || ! bands[static_cast<std::size_t> (target)].on)
        return;
    selectBand (target);
    const float q = eqWheelQ (bands[static_cast<std::size_t> (target)].q, notches, fine);
    setAsGesture (param (target, "q"), q);
    bands[static_cast<std::size_t> (target)].q = q;
    repaint (canvas);
}

bool EqEditor::keyPressed (const juce::KeyPress& k)
{
    if (selected < 0 || dragBand >= 0)
        return false;
    const auto& b = bands[static_cast<std::size_t> (selected)];
    const bool fine = k.getModifiers().isShiftDown();
    const int code = k.getKeyCode();
    if (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey)
    {
        setAsGesture (param (selected, "freq"), eqNudgeFreq (b.freq, code == juce::KeyPress::rightKey ? 1 : -1, fine));
    }
    else if ((code == juce::KeyPress::upKey || code == juce::KeyPress::downKey) && dsp::supportsGain (b.type))
    {
        setAsGesture (param (selected, "gain"), eqNudgeGain (b.gainDb, code == juce::KeyPress::upKey ? 1 : -1, fine));
    }
    else if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey)
    {
        setAsGesture (param (selected, "on"), 0.0f);
    }
    else
        return false;
    refresh();
    return true;
}

void EqEditor::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    if (canvas.contains (e.getPosition()))
        pointerDown (e.position.x - static_cast<float> (canvas.getX()), e.position.y - static_cast<float> (canvas.getY()));
}

void EqEditor::mouseDrag (const juce::MouseEvent& e)
{
    pointerDrag (static_cast<float> (e.getDistanceFromDragStartX()), static_cast<float> (e.getDistanceFromDragStartY()),
                 e.mods.isShiftDown());
}

void EqEditor::mouseUp (const juce::MouseEvent&) { pointerUp(); }

void EqEditor::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (canvas.contains (e.getPosition()))
        pointerDoubleClick (e.position.x - static_cast<float> (canvas.getX()), e.position.y - static_cast<float> (canvas.getY()));
}

void EqEditor::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (! canvas.contains (e.getPosition()))
        return;
    wheel (e.position.x - static_cast<float> (canvas.getX()), e.position.y - static_cast<float> (canvas.getY()),
           w.deltaY / kWheelPerNotch, e.mods.isShiftDown());
}

// ------------------------------------------------------------------------------ layout / paint

void EqEditor::resized()
{
    auto r = getLocalBounds();
    auto bottom = r.removeFromBottom (kBandEditorH).reduced (kPad, 6);
    canvas = r.reduced (kPad, kPad);

    bandLabel.setBounds (bottom.removeFromLeft (kBandLabelW).withTrimmedTop (kCaptionH));
    for (auto it = viewButtons.rbegin(); it != viewButtons.rend(); ++it)
        (*it)->setBounds (bottom.removeFromRight (kViewW).withTrimmedTop (kCaptionH).reduced (1, 0));
    bottom.removeFromRight (kPad);

    std::vector<Box*> all;
    for (auto& b : boxes)
        all.push_back (b.get());
    if (phaseBox != nullptr)
        all.push_back (phaseBox.get());
    if (all.empty())
        return;
    const int w = bottom.getWidth() / static_cast<int> (all.size());
    for (auto* b : all)
    {
        auto cell = bottom.removeFromLeft (w).reduced (2, 0);
        b->caption.setBounds (cell.removeFromTop (kCaptionH));
        b->control->setBounds (cell);
    }
}

void EqEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::kBase);
    g.setColour (theme::kRule);
    g.fillRect (0, 0, getWidth(), 1);
    g.fillRect (0, getHeight() - kBandEditorH, getWidth(), 1);
    paintCanvas (g);
}

void EqEditor::paintCanvas (juce::Graphics& g)
{
    const auto p = plot();
    if (p.w < 2.0f || p.h < 2.0f)
        return;
    const float cx = static_cast<float> (canvas.getX()), cy = static_cast<float> (canvas.getY());
    juce::Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (canvas);

    // Grid: decade ticks and dB lines at 0, +-range/2, +-range.
    g.setFont (theme::mono (10.0f));
    for (double hz : kFreqTicks)
    {
        const float x = cx + static_cast<float> (freqToX (hz, p.w));
        g.setColour (theme::kRule);
        g.drawVerticalLine (juce::roundToInt (x), cy, cy + p.h);
        g.setColour (theme::kSecondary);
        g.drawText (juce::String (eqTickText (hz)), juce::roundToInt (x) + 3, canvas.getBottom() - 14, 40, 12,
                    juce::Justification::left);
    }
    for (float db : { -p.rangeDb, -0.5f * p.rangeDb, 0.0f, 0.5f * p.rangeDb, p.rangeDb })
    {
        const float y = cy + eqGainToY (db, p);
        g.setColour (db == 0.0f ? theme::kSecondary : theme::kRule);
        g.drawHorizontalLine (juce::roundToInt (y), cx, cx + p.w);
        g.setColour (theme::kSecondary);
        g.drawText (juce::String (formatDb (db, 0, true)), canvas.getRight() - 30, juce::roundToInt (y) + 1, 28, 12,
                    juce::Justification::right);
    }

    // Backdrop: EQ input level per 1/6-octave band, joined through band centres.
    {
        juce::Path s;
        s.startNewSubPath (cx, cy + p.h);
        for (int b = 0; b < analysis::kImpactBands; ++b)
            s.lineTo (cx + static_cast<float> (freqToX (analysis::impactBandCentreHz (b), p.w)),
                      cy + eqSpectrumToY (levels[static_cast<std::size_t> (b)], p));
        s.lineTo (cx + p.w, cy + p.h);
        s.closeSubPath();
        g.setColour (theme::kSecondary.withAlpha (0.22f));
        g.fillPath (s);
    }

    // Summed response for the chosen view (same function as the DSP).
    const double fs = state.processor.getSampleRate() > 0.0 ? state.processor.getSampleRate() : 48000.0;
    const int points = std::max (2, canvas.getWidth() / 2);
    const auto curve = eqCurveDb (bands, points, fs, viewChannel);
    juce::Path c;
    for (int i = 0; i < points; ++i)
    {
        const float x = cx + p.w * static_cast<float> (i) / static_cast<float> (points - 1);
        const float y = cy + eqGainToY (static_cast<float> (curve[static_cast<std::size_t> (i)]), p);
        if (i == 0)
            c.startNewSubPath (x, y);
        else
            c.lineTo (x, y);
    }
    g.setColour (theme::kText);
    g.strokePath (c, juce::PathStrokeType (1.5f));

    // Nodes: selected filled in the signal colour; bands outside the shown view are dimmed.
    // Drawn in band order so the higher band is on top (matches the hit-test).
    for (int b = 0; b < dsp::kEqBands; ++b)
    {
        const auto& band = bands[static_cast<std::size_t> (b)];
        if (! band.on)
            continue;
        const auto n = eqNodePos (band, p);
        const juce::Rectangle<float> r (cx + n.x - kNodeR, cy + n.y - kNodeR, 2.0f * kNodeR, 2.0f * kNodeR);
        const bool inView = band.channel == dsp::EqChannel::Stereo || band.channel == viewChannel;
        if (b == selected)
        {
            g.setColour (theme::kSignal);
            g.fillEllipse (r);
        }
        else
        {
            g.setColour (theme::kBase);
            g.fillEllipse (r);
            g.setColour (inView ? theme::kText : theme::kSecondary);
            g.drawEllipse (r, 1.0f);
        }
        g.setColour (inView ? theme::kText : theme::kSecondary);
        g.drawText (juce::String (b + 1), juce::roundToInt (r.getX()) - 4, juce::roundToInt (r.getY()) - 14,
                    juce::roundToInt (r.getWidth()) + 8, 12, juce::Justification::centred);
    }
}

} // namespace strat3::ui
