#pragma once
// Phase 4 aesthetic: modern brutalism. 0-radius rectangles, 1 px rules, no gradients or glows.
// Monochrome base + one signal colour (#FF4A1C) for active, impact and warning states; the
// source's navy / teal / purple and rounded cards are avoided on purpose (trade dress).
// Numbers: monospaced with tabular figures. Labels: grotesk caps with wide tracking.
// Fonts are the system's defaults until JetBrains Mono (OFL) is embedded [UNKNOWN: which
// grotesk Blago wants; not chosen yet].
#include <juce_gui_basics/juce_gui_basics.h>

namespace strat3::ui::theme {

inline const juce::Colour kBase { 0xff0b0b0b };
inline const juce::Colour kRow { 0xff1a1a1a };
inline const juce::Colour kRowSelected { 0xff242424 };
inline const juce::Colour kRule { 0xff2e2e2e };
inline const juce::Colour kText { 0xffededed };
inline const juce::Colour kSecondary { 0xff6b6b6b };
inline const juce::Colour kSignal { 0xffff4a1c };

inline juce::Font mono (float size) { return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), size, juce::Font::plain)); }
inline juce::Font label (float size, bool bold = false)
{
    return juce::Font (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain).withKerningFactor (0.08f));
}

/** Square controls in the theme colours (buttons, toggles, combo boxes, bar sliders). */
class LookAndFeel final : public juce::LookAndFeel_V4
{
public:
    LookAndFeel();
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool over, bool down) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool over, bool down) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool over, bool down) override;
    void drawComboBox (juce::Graphics&, int w, int h, bool down, int bx, int by, int bw, int bh, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override { return mono (12.0f); }
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;
    void drawLinearSlider (juce::Graphics&, int x, int y, int w, int h, float pos, float minPos, float maxPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;
    juce::Font getTextButtonFont (juce::TextButton&, int) override { return label (11.0f, true); }
    juce::Label* createSliderTextBox (juce::Slider&) override;
    void drawPopupMenuBackground (juce::Graphics& g, int, int) override { g.fillAll (kRow); }
};

} // namespace strat3::ui::theme
