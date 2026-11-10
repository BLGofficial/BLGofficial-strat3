#include "ui/web/WebPanelHost.h"

#include "processor/Strat3Processor.h"
#include "ui/native/Theme.h"

#if JUCE_WEB_BROWSER && STRAT3_WITH_WEBUI
#include <BinaryData.h>
#endif

namespace strat3::ui {

std::unique_ptr<AssistantPanel> makeAssistantPanel ([[maybe_unused]] processor::Strat3Processor& p)
{
   #if JUCE_WEB_BROWSER && STRAT3_WITH_WEBUI
    const bool built = true, supported = web::WebPanelHost::supported();
   #else
    const bool built = false, supported = false;
   #endif
    const auto kind = web::choosePanel (built, supported, false);
   #if JUCE_WEB_BROWSER && STRAT3_WITH_WEBUI
    if (kind == web::PanelKind::Web)
        return std::make_unique<web::WebPanelHost> (p);
   #endif
    return std::make_unique<PanelNotice> (kind);
}

} // namespace strat3::ui

#if JUCE_WEB_BROWSER && STRAT3_WITH_WEBUI
namespace strat3::ui::web {

namespace {

PanelFile embedded (const char* name)
{
    int size = 0;
    const char* data = BinaryData::getNamedResource (name, size);
    return { data, size };
}

/** WebView2 keeps a profile folder; the default (next to the host's executable) is often not
 *  writable, so it goes in the user's temp folder, one per product. */
juce::File webViewDataFolder()
{
    return juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("STRAT3-WebView2");
}

} // namespace

juce::WebBrowserComponent::Options WebPanelHost::options (WebPanelHost* owner)
{
    using Options = juce::WebBrowserComponent::Options;
    auto o = Options {}
                // Only Windows has a choice. On macOS and Linux areOptionsSupported is false for
                // any backend but the default, so naming webview2 there disables the panels.
               #if JUCE_WINDOWS
                 .withBackend (Options::Backend::webview2)
               #endif
                 .withWinWebView2Options (Options::WinWebView2 {}.withUserDataFolder (webViewDataFolder()))
                 .withNativeIntegrationEnabled()
                 // The drawer hides the panel often; reloading the page each time would flash
                 // and drop its local state (the open tab, a half-asked question).
                 .withKeepPageLoadedWhenBrowserIsHidden();
    if (owner == nullptr)
        return o;   // only asked whether these options can work at all
    return o.withResourceProvider ([owner] (const juce::String& path)
                                   {
                                       owner->asked.push_back (path);
                                       auto r = panelResource (path, embedded);
                                       if (! r.has_value())
                                           return std::optional<juce::WebBrowserComponent::Resource> {};
                                       return std::optional<juce::WebBrowserComponent::Resource> ({ std::move (r->data), r->mimeType });
                                   })
        .withEventListener (kReadyEvent, [owner] (const juce::var&) { owner->link.ready(); })
        .withEventListener (kCommandEvent, [owner] (const juce::var& payload) { owner->link.command (payload); });
}

bool WebPanelHost::supported() { return juce::WebBrowserComponent::areOptionsSupported (options (nullptr)); }

WebPanelHost::WebPanelHost (processor::Strat3Processor& p)
    : link (portFor (p), [this] (const juce::var& state)
            {
                if (browser != nullptr)
                    browser->emitEventIfBrowserIsVisible (kStateEvent, state);
            })
{
    setTitle ("Assistant");
    browser = std::make_unique<juce::WebBrowserComponent> (options (this));
    addAndMakeVisible (*browser);
    browser->goToURL (juce::WebBrowserComponent::getResourceProviderRoot());
}

WebPanelHost::~WebPanelHost()
{
    // The WebView's callbacks point at this object: it goes first.
    removeChildComponent (browser.get());
    browser.reset();
}

void WebPanelHost::paint (juce::Graphics& g)
{
    g.fillAll (theme::kRule);   // shows as the column's 1 px left rule, as the notice draws it
}

void WebPanelHost::resized()
{
    browser->setBounds (getLocalBounds().withTrimmedLeft (1));
}

void WebPanelHost::visibilityChanged()
{
    if (isVisible())
        link.shown();
}

void WebPanelHost::evaluate (const juce::String& script, std::function<void (const juce::var&)> done)
{
    browser->evaluateJavascript (script, [done = std::move (done)] (juce::WebBrowserComponent::EvaluationResult r)
                                 {
                                     const auto* v = r.getResult();
                                     done (v != nullptr ? *v : juce::var());
                                 });
}

} // namespace strat3::ui::web
#endif
