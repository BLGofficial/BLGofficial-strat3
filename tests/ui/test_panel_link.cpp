// The native side of the WebView panels (module 21c), without a browser. The page side and
// the protocol are fixed by 21a/21b; this is the part between them that the plugin runs.
// Contracts tested here:
//   - the resource provider serves exactly the three files of the bundle (index.html,
//     assets/app.js, assets/style.css) with their types, the page for "/", and nothing else:
//     no other path, no traversal, no query string trick
//   - nothing is sent to the page before it says it is ready ("assistantReady"); the page
//     asking gets the whole state at once
//   - after that the state goes out once per change of the session revision, not per tick
//   - every command from the page goes to the processor and is answered with the state,
//     whether the processor took it or not (the page re-renders from what is true)
//   - showing the panel again sends the state even if nothing changed (a hidden WebView drops
//     events)
//   - a page that never says it is ready within the load timeout counts as failed
//   - which panel the editor shows: the WebView, or a notice saying why there is none
//   - module 21d: the advanced info goes out with the state when the page is ready, then once
//     per change, and again when shown; the gear's "open the advanced view" waits for the
//     page to be ready and is sent once
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "assistant/Protocol.h"
#include "processor/Strat3Processor.h"
#include "ui/web/PanelLink.h"

using namespace strat3;
using processor::Strat3Processor;
namespace web = strat3::ui::web;

namespace {

constexpr double kFs = 48000.0;
constexpr int kBlock = 512;

struct JuceScope
{
    juce::ScopedJuceInitialiser_GUI init;
};

std::unique_ptr<Strat3Processor> makeProc()
{
    auto p = std::make_unique<Strat3Processor> (false);
    p->setRateAndBufferSizeDetails (kFs, kBlock);
    p->prepareToPlay (kFs, kBlock);
    return p;
}

juce::var json (const std::string& text)
{
    juce::var v;
    REQUIRE (juce::JSON::parse (juce::String (text), v).wasOk());
    return v;
}

// Marked stand-ins for the embedded bundle: the provider's job is the routing, not the bytes.
const std::map<std::string, std::string> kStubBundle {
    { "index_html", "<!-- stub index.html -->" },
    { "app_js", "/* stub app.js */" },
    { "style_css", "/* stub style.css */" },
};

web::PanelFile stubLookup (const char* name)
{
    const auto it = kStubBundle.find (name);
    if (it == kStubBundle.end())
        return {};
    return { it->second.data(), static_cast<int> (it->second.size()) };
}

std::string text (const web::PanelResource& r)
{
    return { reinterpret_cast<const char*> (r.data.data()), r.data.size() };
}

struct Event
{
    std::string id;
    juce::var payload;
};

/** A link bound to a real processor, recording every event it sends. */
struct Linked
{
    JuceScope juce;
    std::unique_ptr<Strat3Processor> proc = makeProc();
    std::vector<Event> events;
    std::vector<juce::var> sent;   // the states among them
    web::PanelLink link { web::portFor (*proc), [this] (const char* id, const juce::var& payload)
                          {
                              events.push_back ({ id, payload });
                              if (std::string (id) == web::kStateEvent)
                                  sent.push_back (payload);
                          } };

    std::vector<std::string> ids() const
    {
        std::vector<std::string> out;
        for (const auto& e : events)
            out.push_back (e.id);
        return out;
    }
};

void setParam (Strat3Processor& p, const char* id, float value)
{
    auto* param = p.apvts.getParameter (id);
    REQUIRE (param != nullptr);
    param->setValueNotifyingHost (param->convertTo0to1 (value));
}

} // namespace

TEST_CASE ("panel link: the provider serves the three bundle files and nothing else", "[ui][web]")
{
    const auto page = web::panelResource ("/", stubLookup);
    REQUIRE (page.has_value());
    CHECK (text (*page) == "<!-- stub index.html -->");
    CHECK (page->mimeType == "text/html");

    const auto index = web::panelResource ("/index.html", stubLookup);
    REQUIRE (index.has_value());
    CHECK (text (*index) == text (*page));

    const auto js = web::panelResource ("/assets/app.js", stubLookup);
    REQUIRE (js.has_value());
    CHECK (text (*js) == "/* stub app.js */");
    CHECK (js->mimeType == "text/javascript");

    const auto css = web::panelResource ("/assets/style.css", stubLookup);
    REQUIRE (css.has_value());
    CHECK (text (*css) == "/* stub style.css */");
    CHECK (css->mimeType == "text/css");

    for (const char* path : { "", "/favicon.ico", "/assets/", "/assets/app.js.map", "/app.js", "/assets/../index.html",
                              "/../assets/app.js", "//assets/app.js", "/ASSETS/APP.JS", "/assets/app.js?x=1", "/index.html#target",
                              "https://example.com/assets/app.js" })
    {
        INFO ("path: " << path);
        CHECK_FALSE (web::panelResource (path, stubLookup).has_value());
    }
}

TEST_CASE ("panel link: a bundle file missing from the binary is not served", "[ui][web]")
{
    const auto noCss = [] (const char* name) { return std::string (name) == "style_css" ? web::PanelFile {} : stubLookup (name); };
    CHECK_FALSE (web::panelResource ("/assets/style.css", noCss).has_value());
    CHECK (web::panelResource ("/assets/app.js", noCss).has_value());

    // Present but empty (a bundle built wrong): a blank page would look like a hang, so it is
    // refused and the load timeout turns it into the notice.
    const auto emptyCss = [] (const char* name) { return std::string (name) == "style_css" ? web::PanelFile { "", 0 } : stubLookup (name); };
    CHECK_FALSE (web::panelResource ("/assets/style.css", emptyCss).has_value());
}

TEST_CASE ("panel link: nothing goes to the page before it is ready; asking gets the state", "[ui][web]")
{
    Linked l;
    l.link.tick (0.0);
    l.link.shown();
    REQUIRE (l.proc->startAssistantCapture (assistant::CaptureKind::Analyse, 30.0));
    l.link.tick (0.1);
    CHECK (l.sent.empty());
    CHECK_FALSE (l.link.pageReady());

    l.link.ready();
    CHECK (l.link.pageReady());
    REQUIRE (l.sent.size() == 1);
    CHECK (juce::JSON::toString (l.sent[0]) == juce::JSON::toString (l.proc->assistantState()));
    CHECK (l.sent[0]["capture"]["kind"].toString() == "analyse");
}

TEST_CASE ("panel link: the state goes out once per change, not per tick", "[ui][web]")
{
    Linked l;
    l.link.ready();
    REQUIRE (l.sent.size() == 1);
    for (int i = 0; i < 5; ++i)
        l.link.tick (0.1 * i);
    CHECK (l.sent.size() == 1);

    REQUIRE (l.proc->startAssistantCapture (assistant::CaptureKind::Reference, 30.0));
    l.link.tick (1.0);
    l.link.tick (1.1);
    REQUIRE (l.sent.size() == 2);
    CHECK (l.sent[1]["capture"]["kind"].toString() == "reference");

    l.link.ready();   // the page asked again (it reloaded): it gets the state even unchanged
    CHECK (l.sent.size() == 3);
}

TEST_CASE ("panel link: every command reaches the processor and is answered with the state", "[ui][web]")
{
    Linked l;
    l.link.ready();
    l.sent.clear();

    l.link.command (json (R"({ "type": "analyse", "seconds": 60 })"));
    REQUIRE (l.sent.size() == 1);
    CHECK (l.sent[0]["capture"]["kind"].toString() == "analyse");
    CHECK (static_cast<double> (l.sent[0]["capture"]["seconds"]) == 60.0);
    l.link.tick (0.1);
    CHECK (l.sent.size() == 1);   // already sent: the tick does not repeat it

    // Refused (a second capture while one runs, and a malformed command): still answered, so
    // the page drops whatever it showed in between.
    l.link.command (json (R"({ "type": "captureReference", "seconds": 30 })"));
    l.link.command (json (R"({ "type": "depth", "id": 1 })"));
    l.link.command (juce::var ("analyse"));
    REQUIRE (l.sent.size() == 4);
    for (const auto& s : l.sent)
        CHECK (s["capture"]["kind"].toString() == "analyse");

    l.link.command (json (R"({ "type": "cancel" })"));
    REQUIRE (l.sent.size() == 5);
    CHECK (l.sent[4]["capture"]["kind"].toString() == "none");
    CHECK (l.sent[4]["result"].toString() == "cancelled");
}

TEST_CASE ("panel link: a command before the page is ready is still carried out", "[ui][web]")
{
    // The page only sends commands once it shows a state, so this is a page that reloaded and
    // whose ready event is still in flight. The command is not lost; the answer waits.
    Linked l;
    l.link.command (json (R"({ "type": "analyse", "seconds": 30 })"));
    CHECK (l.sent.empty());
    CHECK (l.proc->assistantState()["capture"]["kind"].toString() == "analyse");
    l.link.ready();
    REQUIRE (l.sent.size() == 1);
    CHECK (l.sent[0]["capture"]["kind"].toString() == "analyse");
}

TEST_CASE ("panel link: showing the panel again sends the state even if nothing changed", "[ui][web]")
{
    Linked l;
    l.link.ready();
    l.link.shown();
    CHECK (l.sent.size() == 2);
    l.link.tick (0.1);
    CHECK (l.sent.size() == 2);
}

TEST_CASE ("panel link: a page that never gets ready within the load timeout has failed", "[ui][web]")
{
    Linked l;
    l.link.tick (0.0);
    CHECK_FALSE (l.link.failed());
    l.link.tick (web::kLoadTimeoutSec - 0.01);
    CHECK_FALSE (l.link.failed());
    l.link.tick (web::kLoadTimeoutSec);
    CHECK (l.link.failed());
    CHECK (l.sent.empty());

    Linked ok;
    ok.link.tick (0.0);
    ok.link.ready();
    ok.link.tick (web::kLoadTimeoutSec * 10.0);
    CHECK_FALSE (ok.link.failed());

    // Ready after the timeout but before the editor swapped the panel: it works, so it has not
    // failed.
    Linked slow;
    slow.link.tick (0.0);
    slow.link.tick (web::kLoadTimeoutSec + 1.0);
    REQUIRE (slow.link.failed());
    slow.link.ready();
    CHECK_FALSE (slow.link.failed());

    // The clock starts at the first tick, not at some earlier time origin.
    Linked late;
    late.link.tick (1000.0);
    late.link.tick (1000.0 + web::kLoadTimeoutSec - 0.01);
    CHECK_FALSE (late.link.failed());
}

TEST_CASE ("panel link: which panel the editor shows", "[ui][web]")
{
    using web::PanelKind;
    CHECK (web::choosePanel (true, true, false) == PanelKind::Web);
    CHECK (web::choosePanel (false, true, false) == PanelKind::NotBuilt);
    CHECK (web::choosePanel (false, false, false) == PanelKind::NotBuilt);
    CHECK (web::choosePanel (true, false, false) == PanelKind::NoWebView);
    CHECK (web::choosePanel (true, true, true) == PanelKind::DidNotLoad);

    // Each notice says what is missing and that the rest of the plugin still works.
    for (auto kind : { PanelKind::NotBuilt, PanelKind::NoWebView, PanelKind::DidNotLoad })
    {
        const auto n = web::noticeFor (kind);
        INFO ("notice: " << n.body);
        CHECK (n.title == "Assistant unavailable");
        CHECK (n.body.contains ("The rest of STRAT3 works as usual."));
    }
    CHECK (web::noticeFor (PanelKind::NotBuilt).body != web::noticeFor (PanelKind::NoWebView).body);
    CHECK (web::noticeFor (PanelKind::NoWebView).body != web::noticeFor (PanelKind::DidNotLoad).body);
    CHECK (web::noticeFor (PanelKind::Web).title.isEmpty());
}

TEST_CASE ("panel link: the advanced info goes with the state when ready, then once per change", "[ui][web]")
{
    using V = std::vector<std::string>;
    Linked l;
    l.link.tick (0.0);
    CHECK (l.events.empty());   // nothing before the page listens

    l.link.ready();
    REQUIRE (l.ids() == V { web::kStateEvent, web::kInfoEvent });
    CHECK (juce::JSON::toString (l.events[1].payload) == juce::JSON::toString (web::advancedInfo (*l.proc)));
    CHECK (static_cast<double> (l.events[1].payload["crossovers"][0]) == Catch::Approx (120.0).margin (0.01));

    for (int i = 1; i <= 5; ++i)
        l.link.tick (0.1 * i);
    CHECK (l.events.size() == 2);   // nothing changed

    setParam (*l.proc, "xover.f1", 200.0f);
    l.link.tick (1.0);
    l.link.tick (1.1);
    REQUIRE (l.ids() == V { web::kStateEvent, web::kInfoEvent, web::kInfoEvent });
    CHECK (static_cast<double> (l.events[2].payload["crossovers"][0]) == Catch::Approx (200.0).margin (0.01));

    l.link.shown();   // a hidden WebView dropped whatever was sent meanwhile
    CHECK (l.ids() == V { web::kStateEvent, web::kInfoEvent, web::kInfoEvent, web::kStateEvent, web::kInfoEvent });
}

TEST_CASE ("panel link: the gear opens the advanced view once the page is ready", "[ui][web]")
{
    using V = std::vector<std::string>;
    Linked l;
    l.link.showAdvanced();   // the page is still loading: remembered, not sent
    CHECK (l.events.empty());

    l.link.ready();
    REQUIRE (l.ids() == V { web::kStateEvent, web::kInfoEvent, web::kViewEvent });
    CHECK (l.events[2].payload["view"].toString() == "advanced");

    l.link.ready();   // a reload later: the old press is not replayed
    CHECK (l.ids() == V { web::kStateEvent, web::kInfoEvent, web::kViewEvent, web::kStateEvent, web::kInfoEvent });

    l.link.showAdvanced();   // ready: at once, and only the view
    REQUIRE (l.events.size() == 6);
    CHECK (l.events[5].id == web::kViewEvent);
    CHECK (l.events[5].payload["view"].toString() == "advanced");
}
