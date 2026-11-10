#include "ui/native/Bars.h"

#include "ui/model/UiModel.h"
#include "ui/native/Theme.h"

namespace strat3::ui {

// ------------------------------------------------------------------------------ header

Header::Header (juce::AudioProcessorValueTreeState& s)
    : modeParam (*s.getParameter ("global.latencyMode")),
      modeAttach (modeParam, [this] (float v)
                  {
                      for (std::size_t i = 0; i < modes.size(); ++i)
                          modes[i].setToggleState (static_cast<int> (std::lround (v)) == static_cast<int> (i), juce::dontSendNotification);
                  })
{
    static constexpr const char* names[] { "TRACK", "MIX", "MASTER" };
    for (std::size_t i = 0; i < modes.size(); ++i)
    {
        modes[i].setButtonText (names[i]);
        modes[i].setTitle (juce::String ("Latency mode ") + names[i]);
        modes[i].onClick = [this, i] { modeAttach.setValueAsCompleteGesture (static_cast<float> (i)); };
        addAndMakeVisible (modes[i]);
    }
    modeAttach.sendInitialUpdate();

    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (s.getParameter ("global.target")))
    {
        target.addItemList (choice->choices, 1);
        targetAttach = std::make_unique<juce::ComboBoxParameterAttachment> (*choice, target);
    }
    target.setTitle ("Loudness target");
    addAndMakeVisible (target);

    gainMatch.setTitle ("Gain match");
    gainMatchAttach = std::make_unique<juce::ButtonParameterAttachment> (*s.getParameter ("global.gainMatch"), gainMatch);
    addAndMakeVisible (gainMatch);

    delta.setButtonText (juce::String::fromUTF8 ("\xce\x94"));
    delta.setTitle ("Global delta");
    deltaAttach = std::make_unique<juce::ButtonParameterAttachment> (*s.getParameter ("global.delta"), delta);
    addAndMakeVisible (delta);

    assistant.setTitle ("Assistant");
    assistant.setClickingTogglesState (true);
    addChildComponent (assistant);   // the editor shows it below the column breakpoint
}

void Header::setDrawerMode (bool on)
{
    assistant.setVisible (on);
}

Header::~Header() = default;

void Header::paint (juce::Graphics& g)
{
    g.fillAll (theme::kBase);
    g.setColour (theme::kText);
    g.setFont (theme::label (18.0f, true));
    g.drawText ("STRAT3", 16, 0, 120, getHeight(), juce::Justification::centredLeft, false);
    g.setColour (theme::kSecondary);
    g.setFont (theme::label (10.0f));
    g.drawText ("TARGET", 420, 0, 60, getHeight(), juce::Justification::centredRight, false);
    g.setColour (theme::kRule);
    g.fillRect (0, getHeight() - 1, getWidth(), 1);
}

void Header::resized()
{
    const int h = 24, y = (getHeight() - h) / 2;
    int x = 140;
    for (auto& b : modes)
    {
        b.setBounds (x, y, 80, h);
        x += 80;
    }
    target.setBounds (488, y, 150, h);
    delta.setBounds (getWidth() - 16 - 32, y, 32, h);
    gainMatch.setBounds (delta.getX() - 8 - 110, y, 110, h);
    // Left of GAIN-MATCH; at the minimum width (900) it still clears the target box (ends 638).
    assistant.setBounds (gainMatch.getX() - 4 - 88, y, 88, h);
}

// ------------------------------------------------------------------------------ delivery bar

DeliveryBar::DeliveryBar() { setTitle ("Delivery meters"); }

void DeliveryBar::show (const DeliveryReadout& next)
{
    r = next;
    setDescription (r.line1 + " | " + r.line2);   // screen readers get the same numbers
    repaint();
}

juce::Rectangle<int> DeliveryBar::meterArea() const { return getLocalBounds().removeFromRight (300).reduced (12, 10); }

void DeliveryBar::paint (juce::Graphics& g)
{
    g.fillAll (theme::kBase);
    g.setColour (theme::kRule);
    g.fillRect (0, 0, getWidth(), 1);

    auto text = getLocalBounds().withTrimmedRight (300).reduced (16, 8);
    g.setFont (theme::mono (13.0f));
    g.setColour (theme::kText);
    g.drawText (r.line1, text.removeFromTop (text.getHeight() / 2), juce::Justification::centredLeft, true);
    g.setColour (r.nonFinite ? theme::kSignal : theme::kSecondary);
    g.drawText (r.line2, text, juce::Justification::centredLeft, true);

    // I/O bar meters, -60..0 dBFS, with clip LEDs (ARCHON reference). A lit LED is a filled
    // square, an unlit one an outline.
    const auto area = meterArea();
    const int barH = 8;
    const std::array<std::pair<const char*, const std::array<float, 2>*>, 2> groups { { { "IN", &r.inDb }, { "OUT", &r.outDb } } };
    int y = area.getY();
    for (std::size_t gi = 0; gi < groups.size(); ++gi)
    {
        g.setColour (theme::kSecondary);
        g.setFont (theme::label (10.0f));
        g.drawText (groups[gi].first, area.getX(), y, 32, 2 * barH + 2, juce::Justification::centredLeft, false);
        const int bx = area.getX() + 36, bw = area.getWidth() - 36 - 18;
        for (std::size_t c = 0; c < 2; ++c)
        {
            const int by = y + static_cast<int> (c) * (barH + 2);
            g.setColour (theme::kRow);
            g.fillRect (bx, by, bw, barH);
            const float t = juce::jlimit (0.0f, 1.0f, ((*groups[gi].second)[c] + 60.0f) / 60.0f);
            g.setColour ((*groups[gi].second)[c] > -0.1f ? theme::kSignal : theme::kText);
            g.fillRect (bx, by, static_cast<int> (t * static_cast<float> (bw)), barH);
        }
        const bool lit = gi == 0 ? r.inClip : r.outClip;
        const juce::Rectangle<int> led (area.getRight() - 12, y + 2, 12, 2 * barH - 2);
        g.setColour (lit ? theme::kSignal : theme::kSecondary);
        if (lit)
            g.fillRect (led);
        else
            g.drawRect (led, 1);
        y += 2 * barH + 8;
    }
}

void DeliveryBar::mouseDown (const juce::MouseEvent& e)
{
    if (meterArea().contains (e.getPosition()))
    {
        if (onClearClips)
            onClearClips();
    }
    else if (r.nonFinite && onClearNonFinite)
        onClearNonFinite();
}

} // namespace strat3::ui
