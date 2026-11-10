#pragma once
// What the editor's assistant column holds (module 21c): the WebView panels, or a native
// notice saying why there are none. The editor only knows this interface, so it builds the
// same with or without the WebView.
#include <juce_gui_basics/juce_gui_basics.h>

#include "ui/web/PanelLink.h"

namespace strat3::ui {

class AssistantPanel : public juce::Component
{
public:
    /** The editor's 30 Hz timer, with a monotonic clock in seconds. */
    virtual void tick (double /*nowSec*/) {}
    /** True when the panel gave up (the page never loaded): the editor swaps in a notice. */
    virtual bool failed() const { return false; }
    virtual web::PanelKind kind() const = 0;
};

/** The native stand-in: title and one paragraph, in the theme. */
class PanelNotice final : public AssistantPanel
{
public:
    explicit PanelNotice (web::PanelKind);
    void paint (juce::Graphics&) override;
    web::PanelKind kind() const override { return panelKind; }
    const web::Notice& notice() const noexcept { return text; }

private:
    web::PanelKind panelKind;
    web::Notice text;
};

} // namespace strat3::ui
