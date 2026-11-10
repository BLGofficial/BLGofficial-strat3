#pragma once
// The assistant column as a WebView (module 21c): JUCE's WebBrowserComponent showing the
// embedded React bundle (webui/), linked to the processor's assistant session by PanelLink.
// Module 21d adds the advanced view's parameters: one JUCE control relay per entry of
// kAdvancedRelays (AdvancedLink.h), attached to the processor's parameters.
// Only in a build with the panels (STRAT3_WITH_WEBUI) and the WebView (JUCE_WEB_BROWSER);
// otherwise makeAssistantPanel gives the native notice. Message thread only.
#include <functional>
#include <memory>
#include <vector>

#include "ui/native/AssistantPanel.h"

namespace strat3::processor { class Strat3Processor; }

namespace strat3::ui {

/** The panel for this build and platform: the WebView, or a notice saying why there is none. */
std::unique_ptr<AssistantPanel> makeAssistantPanel (processor::Strat3Processor&);

} // namespace strat3::ui

#if JUCE_WEB_BROWSER && STRAT3_WITH_WEBUI
#include <juce_gui_extra/juce_gui_extra.h>

namespace strat3::ui::web {

class WebPanelHost final : public AssistantPanel
{
public:
    explicit WebPanelHost (processor::Strat3Processor&);
    ~WebPanelHost() override;

    /** False when the platform cannot create the WebView (Windows without the WebView2
     *  Runtime); the editor shows the notice instead. */
    static bool supported();

    void tick (double nowSec) override { link.tick (nowSec); }
    void showAdvanced() override { link.showAdvanced(); }
    bool failed() const override { return link.failed(); }
    PanelKind kind() const override { return PanelKind::Web; }
    void paint (juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;

    // For the WebView check app (tests/webview/).
    bool pageReady() const noexcept { return link.pageReady(); }
    const std::vector<juce::String>& requestedPaths() const noexcept { return asked; }
    /** Runs a script in the page; `done` gets its result (void when it failed). */
    void evaluate (const juce::String& script, std::function<void (const juce::var&)> done);

private:
    static juce::WebBrowserComponent::Options options (WebPanelHost* owner);

    struct Relays;

    PanelLink link;
    std::vector<juce::String> asked;   // every path the page requested, in order
    // Before the browser: its options name the relays, and it tells them when it goes.
    std::unique_ptr<Relays> relays;
    std::unique_ptr<juce::WebBrowserComponent> browser;
};

} // namespace strat3::ui::web
#endif
