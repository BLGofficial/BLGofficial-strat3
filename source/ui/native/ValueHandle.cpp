#include "ui/native/ValueHandle.h"

#include "ui/model/UiModel.h"
#include "ui/native/Theme.h"

namespace strat3::ui {

ValueHandle::ValueHandle (juce::RangedAudioParameter& p, float coarseStep, float fineStep)
    : param (p),
      attachment (p, [this] (float) { repaint(); }),
      coarse (coarseStep),
      fine (fineStep)
{
    setWantsKeyboardFocus (false);   // the editor owns the keys (row selection)
    setTitle (p.getName (64));
    attachment.sendInitialUpdate();
}

ValueHandle::~ValueHandle()
{
    if (dragging)
        attachment.endGesture();
}

float ValueHandle::value() const noexcept { return param.convertFrom0to1 (param.getValue()); }

void ValueHandle::setAsGesture (float v)
{
    const float before = value();
    attachment.setValueAsCompleteGesture (v);
    if (onUserChange)
        onUserChange (before, value());
}

void ValueHandle::beginDrag()
{
    if (dragging)
        return;
    dragging = true;
    dragStart = value();
    attachment.beginGesture();
}

void ValueHandle::dragBy (float dxPx, bool fineDrag)
{
    if (! dragging)
        return;
    const auto range = param.getNormalisableRange();
    const float before = value();
    attachment.setValueAsPartOfGesture (dragValue (dragStart, dxPx, static_cast<float> (trackWidth()), fineDrag,
                                                   range.start, range.end));
    if (onUserChange)
        onUserChange (before, value());
}

void ValueHandle::endDrag()
{
    if (! dragging)
        return;
    dragging = false;
    attachment.endGesture();
}

void ValueHandle::resetToDefault() { setAsGesture (param.convertFrom0to1 (param.getDefaultValue())); }

void ValueHandle::step (int dir, bool fineStep)
{
    const auto range = param.getNormalisableRange();
    setAsGesture (stepValue (value(), dir, fineStep, coarse, fine, range.start, range.end));
}

void ValueHandle::paint (juce::Graphics& g)
{
    const auto range = param.getNormalisableRange();
    const auto track = getLocalBounds().withWidth (trackWidth()).toFloat();
    const float cy = track.getCentreY();
    g.setColour (theme::kRule);
    g.fillRect (track.getX(), cy - 0.5f, track.getWidth(), 1.0f);

    const float t = range.end > range.start ? (value() - range.start) / (range.end - range.start) : 0.0f;
    const float x = track.getX() + juce::jlimit (0.0f, 1.0f, t) * track.getWidth();
    g.setColour (accent ? theme::kText : theme::kSecondary);
    g.fillRect (track.getX(), cy - 1.0f, x - track.getX(), 2.0f);
    g.setColour (accent ? theme::kSignal : theme::kText);
    g.fillRect (x - 3.0f, cy - 6.0f, 6.0f, 12.0f);   // the handle: a hard block

    g.setColour (theme::kText);
    g.setFont (theme::mono (12.0f));
    const auto text = format ? format (value()) : juce::String (formatPercent (value()));
    g.drawText (text, getLocalBounds().removeFromRight (kValueW), juce::Justification::centredRight, false);
}

void ValueHandle::mouseDown (const juce::MouseEvent&) { beginDrag(); }
void ValueHandle::mouseDrag (const juce::MouseEvent& e) { dragBy (static_cast<float> (e.getDistanceFromDragStartX()), e.mods.isShiftDown()); }
void ValueHandle::mouseUp (const juce::MouseEvent&) { endDrag(); }
void ValueHandle::mouseDoubleClick (const juce::MouseEvent&) { endDrag(); resetToDefault(); }

std::unique_ptr<juce::AccessibilityHandler> ValueHandle::createAccessibilityHandler()
{
    struct Value final : juce::AccessibilityRangedNumericValueInterface
    {
        explicit Value (ValueHandle& h) : handle (h) {}
        bool isReadOnly() const override { return false; }
        double getCurrentValue() const override { return handle.value(); }
        void setValue (double v) override { handle.setAsGesture (static_cast<float> (v)); }
        AccessibleValueRange getRange() const override
        {
            const auto r = handle.param.getNormalisableRange();
            return { { r.start, r.end }, handle.coarse };
        }
        ValueHandle& handle;
    };
    return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::slider,
                                                         juce::AccessibilityActions{},
                                                         juce::AccessibilityHandler::Interfaces { std::make_unique<Value> (*this) });
}

} // namespace strat3::ui
