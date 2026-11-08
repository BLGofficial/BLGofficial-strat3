#include "ui/native/Theme.h"

namespace strat3::ui::theme {

LookAndFeel::LookAndFeel()
{
    setColour (juce::ResizableWindow::backgroundColourId, kBase);
    setColour (juce::TextButton::buttonColourId, kRow);
    setColour (juce::TextButton::buttonOnColourId, kSignal);
    setColour (juce::TextButton::textColourOffId, kText);
    setColour (juce::TextButton::textColourOnId, kBase);
    setColour (juce::ToggleButton::textColourId, kText);
    setColour (juce::ComboBox::backgroundColourId, kRow);
    setColour (juce::ComboBox::textColourId, kText);
    setColour (juce::ComboBox::outlineColourId, kRule);
    setColour (juce::ComboBox::arrowColourId, kSecondary);
    setColour (juce::PopupMenu::backgroundColourId, kRow);
    setColour (juce::PopupMenu::textColourId, kText);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, kSignal);
    setColour (juce::PopupMenu::highlightedTextColourId, kBase);
    setColour (juce::Slider::trackColourId, kText);
    setColour (juce::Slider::backgroundColourId, kRow);
    setColour (juce::Slider::textBoxTextColourId, kText);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Label::textColourId, kText);
    setColour (juce::ScrollBar::thumbColourId, kSecondary);
}

void LookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool)
{
    const auto r = b.getLocalBounds().toFloat();
    g.setColour (b.getToggleState() ? kSignal : (over ? kRowSelected : kRow));
    g.fillRect (r);
    g.setColour (b.getToggleState() ? kSignal : kRule);
    g.drawRect (r, 1.0f);
}

void LookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool)
{
    g.setFont (getTextButtonFont (b, b.getHeight()));
    g.setColour (b.getToggleState() ? kBase : (b.isEnabled() ? kText : kSecondary));
    g.drawText (b.getButtonText(), b.getLocalBounds(), juce::Justification::centred, false);
}

void LookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool over, bool)
{
    // A toggle is a square text button: filled in the signal colour when on (shape + label,
    // so the state never rests on colour alone).
    const auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    const bool on = b.getToggleState();
    g.setColour (on ? kSignal : (over ? kRowSelected : kRow));
    g.fillRect (r);
    g.setColour (on ? kSignal : kRule);
    g.drawRect (r, 1.0f);
    g.setColour (on ? kBase : kText);
    g.setFont (label (11.0f, true));
    g.drawText (b.getButtonText(), b.getLocalBounds(), juce::Justification::centred, false);
}

void LookAndFeel::drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox&)
{
    const juce::Rectangle<float> r (0.0f, 0.0f, static_cast<float> (w), static_cast<float> (h));
    g.setColour (kRow);
    g.fillRect (r);
    g.setColour (kRule);
    g.drawRect (r, 1.0f);
    g.setColour (kSecondary);
    juce::Path arrow;
    const float ax = static_cast<float> (w) - 14.0f, ay = static_cast<float> (h) * 0.5f;
    arrow.addTriangle (ax, ay - 2.0f, ax + 8.0f, ay - 2.0f, ax + 4.0f, ay + 3.0f);
    g.fillPath (arrow);
}

void LookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& l)
{
    l.setBounds (4, 1, box.getWidth() - 22, box.getHeight() - 2);
    l.setFont (getComboBoxFont (box));
}

void LookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float, float,
                                    juce::Slider::SliderStyle, juce::Slider& s)
{
    // Bar slider: 1 px track rule, filled from the left edge to the value.
    const juce::Rectangle<float> r (static_cast<float> (x), static_cast<float> (y), static_cast<float> (w), static_cast<float> (h));
    g.setColour (kRow);
    g.fillRect (r);
    g.setColour (s.isMouseOverOrDragging() ? kText : kSecondary);
    g.fillRect (juce::Rectangle<float> (r.getX(), r.getY(), pos - r.getX(), r.getHeight()).reduced (0.0f, r.getHeight() * 0.35f));
    g.setColour (kRule);
    g.drawRect (r, 1.0f);
}

juce::Label* LookAndFeel::createSliderTextBox (juce::Slider& s)
{
    auto* l = LookAndFeel_V4::createSliderTextBox (s);
    l->setFont (mono (11.0f));
    l->setJustificationType (juce::Justification::centredRight);
    return l;
}

} // namespace strat3::ui::theme
