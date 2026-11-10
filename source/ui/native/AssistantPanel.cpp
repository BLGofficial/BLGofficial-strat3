#include "ui/native/AssistantPanel.h"

#include "ui/native/Theme.h"

namespace strat3::ui {

PanelNotice::PanelNotice (web::PanelKind k) : panelKind (k), text (web::noticeFor (k))
{
    setTitle (text.title);
    setDescription (text.body);   // screen readers get the whole notice
}

void PanelNotice::paint (juce::Graphics& g)
{
    g.fillAll (theme::kBase);
    g.setColour (theme::kRule);
    g.fillRect (0, 0, 1, getHeight());   // the column's left rule, as the panels draw it

    auto area = getLocalBounds().reduced (12, 12).withTrimmedLeft (1);
    g.setColour (theme::kText);
    g.setFont (theme::label (11.0f, true));
    g.drawText (text.title.toUpperCase(), area.removeFromTop (16), juce::Justification::centredLeft, false);
    area.removeFromTop (8);

    g.setColour (theme::kText.withAlpha (0.7f));
    juce::AttributedString body;
    body.append (text.body, theme::label (12.0f), theme::kText.withAlpha (0.7f));
    body.setWordWrap (juce::AttributedString::byWord);
    body.setLineSpacing (4.0f);
    body.draw (g, area.toFloat());
}

} // namespace strat3::ui
