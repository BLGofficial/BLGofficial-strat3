#pragma once
// The native side of the WebView panels, without the browser (module 21c). The WebView host
// (WebPanelHost) wires this to a WebBrowserComponent; the tests drive it directly.
//
// Events (fixed by the page, webui/src/bridge/juce.ts):
//   page -> native  "assistantReady"    payload ignored: "send me the state"
//                   "assistantCommand"  one command object for assistant::parseCommand
//   native -> page  "assistantState"    the whole session (assistant::stateToVar)
//                   "advancedInfo"      what the engine runs with (module 21d, AdvancedLink.h)
//                   "showView"          open the advanced view (module 21d)
// The page re-renders from every state it gets, so the link only has to make sure it gets one
// after anything that changed, and never before it listens.
//
// Resources: the bundle is three files embedded with juce_add_binary_data. Only those three
// paths are served; anything else is refused, so a page can never reach other embedded data.
#include <juce_core/juce_core.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include "processor/Strat3Processor.h"
#include "ui/web/AdvancedLink.h"

namespace strat3::ui::web {

inline constexpr const char* kStateEvent = "assistantState";
inline constexpr const char* kReadyEvent = "assistantReady";
inline constexpr const char* kCommandEvent = "assistantCommand";

/** How long the page may take to say it is ready before the panel counts as failed. A cold
 *  WebView2 start takes a few seconds on a slow machine [INFERRED own margin]. */
inline constexpr double kLoadTimeoutSec = 10.0;

/** One embedded file, as BinaryData gives it: null data when the name is unknown. */
struct PanelFile
{
    const char* data = nullptr;
    int size = 0;
};

/** Looks an embedded file up by its BinaryData name ("index_html", "app_js", "style_css"). */
using FileLookup = std::function<PanelFile (const char* resourceName)>;

/** What the provider hands the WebView (the same shape as WebBrowserComponent::Resource,
 *  which does not exist in a build without the WebView). */
struct PanelResource
{
    std::vector<std::byte> data;
    juce::String mimeType;
};

/**
 * The bundle file for a request path, or nothing.
 * @param path  the path the WebView asks for, relative to the resource root ("/" is the page)
 * @param files the embedded files
 * @returns the file and its type for one of the four accepted paths, else nullopt
 */
inline std::optional<PanelResource> panelResource (const juce::String& path, const FileLookup& files)
{
    // Exact matches only: no normalising, so "..", "//", case tricks or a query string can
    // never resolve to something the table does not list.
    struct Entry { const char* path; const char* name; const char* mime; };
    static constexpr Entry table[] {
        { "/", "index_html", "text/html" },
        { "/index.html", "index_html", "text/html" },
        { "/assets/app.js", "app_js", "text/javascript" },
        { "/assets/style.css", "style_css", "text/css" },
    };
    for (const auto& e : table)
    {
        if (path != e.path)
            continue;
        const auto file = files (e.name);
        if (file.data == nullptr || file.size <= 0)
            return std::nullopt;
        const auto* bytes = reinterpret_cast<const std::byte*> (file.data);
        return PanelResource { std::vector<std::byte> (bytes, bytes + file.size), e.mime };
    }
    return std::nullopt;
}

/** What the link needs from the processor. */
struct SessionPort
{
    std::function<juce::var()> state;
    std::function<std::uint32_t()> revision;
    std::function<bool (const juce::var&)> command;
    std::function<juce::var()> info;   // advancedInfo
};

/** The port for a real processor. Message thread, like everything in the link. */
inline SessionPort portFor (processor::Strat3Processor& p)
{
    return { [&p] { return p.assistantState(); },
             [&p] { return p.assistantRevision(); },
             [&p] (const juce::var& c) { return p.assistantCommand (c); },
             [&p] { return advancedInfo (p); } };
}

/** Keeps the page's view of the session current. Message thread only. */
class PanelLink
{
public:
    using Emit = std::function<void (const char* eventId, const juce::var& payload)>;

    PanelLink (SessionPort portIn, Emit emitIn) : port (std::move (portIn)), emit (std::move (emitIn)) {}

    /** The page says it listens (on load, and again after a reload): it gets the state now,
     *  and the advanced view if the gear was pressed while it loaded. */
    void ready()
    {
        isReady = true;
        send();
        if (std::exchange (viewPending, false))
            emit (kViewEvent, viewPayload());
    }

    /** A command from the page. Answered with the state whether the processor took it or
     *  not: a refused command must not leave the page showing what it hoped for. */
    void command (const juce::var& payload)
    {
        port.command (payload);
        if (isReady)
            send();
    }

    /** The editor's timer. Sends the state when the session changed since the last send, and
     *  starts the load clock on its first call. */
    void tick (double nowSec)
    {
        if (! firstTick.has_value())
            firstTick = nowSec;
        if (! isReady)
        {
            loadFailed = nowSec - *firstTick >= kLoadTimeoutSec;
            return;
        }
        if (port.revision() != sentRevision)
            send();
        else if (juce::JSON::toString (port.info(), true) != sentInfo)
            sendInfo();
    }

    /** The panel became visible. A hidden WebView drops events, so whatever was sent while
     *  it was hidden may never have arrived. */
    void shown()
    {
        if (isReady)
            send();
    }

    /** The header's gear: the page opens its advanced view. Remembered while the page loads,
     *  not replayed after a later reload (the press was long ago by then). */
    void showAdvanced()
    {
        if (isReady)
            emit (kViewEvent, viewPayload());
        else
            viewPending = true;
    }

    bool pageReady() const noexcept { return isReady; }
    /** True once the page has taken kLoadTimeoutSec without saying it is ready. */
    bool failed() const noexcept { return loadFailed && ! isReady; }

private:
    void send()
    {
        // Revision first: a change landing between the two reads is sent again next tick
        // rather than lost.
        sentRevision = port.revision();
        emit (kStateEvent, port.state());
        sendInfo();
    }

    void sendInfo()
    {
        const auto info = port.info();
        // Compared as text: the info is a handful of numbers, and a var compares by identity.
        sentInfo = juce::JSON::toString (info, true);
        emit (kInfoEvent, info);
    }

    static juce::var viewPayload()
    {
        auto* o = new juce::DynamicObject;
        o->setProperty ("view", "advanced");
        return o;
    }

    SessionPort port;
    Emit emit;
    bool isReady = false, loadFailed = false, viewPending = false;
    std::uint32_t sentRevision = 0;
    juce::String sentInfo;
    std::optional<double> firstTick;
};

// ------------------------------------------------------------------------------ which panel

enum class PanelKind { Web, NotBuilt, NoWebView, DidNotLoad };

/**
 * @param builtWithPanels  the binary embeds the bundle and the WebView (STRAT3_WITH_WEBUI)
 * @param webViewSupported WebBrowserComponent::areOptionsSupported for the panel's options
 * @param loadFailed       the page never said it was ready (PanelLink::failed)
 */
inline PanelKind choosePanel (bool builtWithPanels, bool webViewSupported, bool loadFailed) noexcept
{
    if (! builtWithPanels)
        return PanelKind::NotBuilt;
    if (! webViewSupported)
        return PanelKind::NoWebView;
    return loadFailed ? PanelKind::DidNotLoad : PanelKind::Web;
}

struct Notice
{
    juce::String title, body;
};

/** What the column says instead of the panels. Empty for PanelKind::Web. */
inline Notice noticeFor (PanelKind kind)
{
    const juce::String rest = " The rest of STRAT3 works as usual.";
    switch (kind)
    {
        case PanelKind::NotBuilt:
            return { "Assistant unavailable", "This build of STRAT3 was made without the assistant panels." + rest };
        case PanelKind::NoWebView:
           #if JUCE_WINDOWS
            return { "Assistant unavailable", "The assistant panels need the Microsoft Edge WebView2 Runtime, which is not installed. "
                                              "Install it, then close and reopen this window." + rest };
           #else
            return { "Assistant unavailable", "The assistant panels need the system web view, which could not be started." + rest };
           #endif
        case PanelKind::DidNotLoad:
            return { "Assistant unavailable", "The assistant panels did not load. Close and reopen this window to try again." + rest };
        case PanelKind::Web:
            break;
    }
    return {};
}

} // namespace strat3::ui::web
