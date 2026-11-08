#pragma once
// Depth / trim / ceiling handle: a horizontal bar dragged left-right (Phase 4 primary
// interaction). The whole track spans the parameter range; Shift drags 1/10 as fast;
// double-click resets to the default. Keys step it (the editor routes Left / Right here).
// Every edit reaches the host inside a gesture (ParameterAttachment). Message thread only.
#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>

namespace strat3::ui {

class ValueHandle final : public juce::Component
{
public:
    /** coarse / fine: key step in parameter units (fine also sets the snap grid). */
    ValueHandle (juce::RangedAudioParameter& param, float coarseStep, float fineStep);
    ~ValueHandle() override;

    float value() const noexcept;                  // parameter units
    int trackWidth() const noexcept { return getWidth() - kValueW; }

    void beginDrag();
    void dragBy (float dxPx, bool fine);           // from where the drag began
    void endDrag();
    void resetToDefault();
    void step (int dir, bool fine);

    void setAccent (bool on) { if (accent != on) { accent = on; repaint(); } }
    /** Called after each user edit with the value before and after (linked trims). */
    std::function<void (float before, float after)> onUserChange;
    /** Formats the value text (default: integer). */
    std::function<juce::String (float)> format;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

    static constexpr int kValueW = 44;   // value text right of the track

private:
    void setAsGesture (float v);

    juce::RangedAudioParameter& param;
    juce::ParameterAttachment attachment;
    const float coarse, fine;
    float dragStart = 0.0f;
    bool dragging = false, accent = false;
};

} // namespace strat3::ui
