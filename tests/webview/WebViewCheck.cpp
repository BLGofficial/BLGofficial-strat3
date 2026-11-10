// The panels in a real WebView (module 21c): the plugin's own editor, with the embedded
// bundle, in a window. Run under a display (xvfb-run on a server). Exit code 0 when every
// check passed, 1 when one failed, 77 when no WebView could be created (ctest: skipped).
//
// What it checks, end to end through JUCE's WebBrowserComponent and the system WebView:
//   - the page loads from the embedded bundle and says it is ready
//   - it renders the processor's real state (tabs, the Analyse buttons)
//   - a click in the page reaches the processor as a command, and the new state comes back
//     to the page (Analyse, then Cancel)
//   - the page asked for the three bundle files and nothing else
//   - a state that changed while the drawer was closed is on screen when it opens
//   - the editor closes with the page loaded
// Screenshots of the window can be taken from outside while it waits (STRAT3_WEBVIEW_HOLD_MS).
#include <juce_gui_extra/juce_gui_extra.h>

#include <functional>
#include <iostream>
#include <memory>
#include <set>
#include <vector>

#include "processor/Strat3Processor.h"
#include "ui/Strat3Editor.h"
#include "ui/web/WebPanelHost.h"

namespace {

using strat3::processor::Strat3Processor;
using strat3::ui::Strat3Editor;

/** One step: start it, then wait until `done` or the timeout. */
struct Step
{
    juce::String name;
    std::function<void()> start;
    std::function<bool()> done;
    double timeoutSec = 10.0;
};

// Finds a button by its visible text (or label) and clicks it: what a user's click does.
juce::String clickScript (const juce::String& label)
{
    return "(() => { const b = [...document.querySelectorAll('button')].find(x => (x.getAttribute('aria-label') || x.textContent).trim() === "
           + juce::JSON::toString (label) + "); if (!b || b.disabled) return false; b.click(); return true; })()";
}

class Check final : public juce::JUCEApplication, private juce::Timer
{
public:
    const juce::String getApplicationName() override { return "strat3_webview_check"; }
    const juce::String getApplicationVersion() override { return "1"; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String&) override
    {
        proc = std::make_unique<Strat3Processor> (false);
        proc->setRateAndBufferSizeDetails (48000.0, 512);
        proc->prepareToPlay (48000.0, 512);
        editor.reset (static_cast<Strat3Editor*> (proc->createEditor()));

        window = std::make_unique<juce::DocumentWindow> ("STRAT3", juce::Colours::black, 0);
        window->setUsingNativeTitleBar (false);
        window->setContentNonOwned (editor.get(), true);
        window->setTopLeftPosition (0, 0);
        window->setVisible (true);

        host = editor->webPanel();
        if (host == nullptr)
        {
            std::cout << "SKIP  no WebView panel in this editor: " << editor->panelText() << std::endl;
            setApplicationReturnValue (77);
            quit();
            return;
        }
        buildSteps();
        startTimer (20);
    }

    void shutdown() override
    {
        stopTimer();
        if (window != nullptr)
            window->clearContentComponent();
        editor.reset();   // before the processor, as a host does
        window.reset();
        if (proc != nullptr)
            proc->releaseResources();
        proc.reset();
    }

    void systemRequestedQuit() override { quit(); }

private:
    std::unique_ptr<Strat3Processor> proc;
    std::unique_ptr<Strat3Editor> editor;
    std::unique_ptr<juce::DocumentWindow> window;
    strat3::ui::web::WebPanelHost* host = nullptr;

    std::vector<Step> steps;
    std::size_t current = 0;
    bool started = false;
    double stepStart = 0.0;
    int failures = 0;
    juce::var lastEval;          // result of the last page script
    bool evalPending = false;
    juce::String pageText;       // document.body.innerText, refreshed while a step waits

    static double now() { return juce::Time::getMillisecondCounterHiRes() / 1000.0; }

    void eval (const juce::String& script)
    {
        evalPending = true;
        host->evaluate (script, [this] (const juce::var& v) { lastEval = v; evalPending = false; });
    }

    /** Re-reads the page text whenever no script is in flight. innerText is what is rendered,
     *  so it carries the CSS upper-casing of labels: the match ignores case. */
    bool pageShows (const juce::String& text)
    {
        if (! evalPending)
        {
            pageText = lastEval.toString();
            eval ("document.body.innerText");
        }
        return pageText.containsIgnoreCase (text);
    }

    juce::String captureKind() const { return proc->assistantState()["capture"]["kind"].toString(); }

    void buildSteps()
    {
        steps.push_back ({ "the page loads from the bundle and says it is ready", [] {}, [this] { return host->pageReady(); }, 30.0 });
        steps.push_back ({ "it renders the processor's state: two tabs and the Analyse buttons", [] {},
                           [this] { return pageShows ("Assistant") && pageShows ("Target") && pageShows ("Analyse 30"); } });
        steps.push_back ({ "a click on Analyse 30 s reaches the processor", [this] { eval (clickScript ("Analyse 30 s")); },
                           [this] { return captureKind() == "analyse"; } });
        steps.push_back ({ "the new state comes back to the page", [] {}, [this] { return pageShows ("Analysing"); } });
        steps.push_back ({ "a click on Cancel ends the capture", [this] { eval (clickScript ("Cancel")); },
                           [this] { return captureKind() == "none"; } });
        steps.push_back ({ "the page says the capture was cancelled", [] {}, [this] { return pageShows ("Capture cancelled."); } });
        steps.push_back ({ "the page asked for the three bundle files and nothing else", [] {},
                           [this]
                           {
                               const std::set<juce::String> expected { "/", "/assets/app.js", "/assets/style.css" };
                               const auto& asked = host->requestedPaths();
                               return std::set<juce::String> (asked.begin(), asked.end()) == expected;
                           },
                           2.0 });
        steps.push_back ({ "the page fits the column: no horizontal scroll", [this] { eval ("document.documentElement.scrollWidth <= window.innerWidth && window.innerWidth > 0"); },
                           [this] { return ! evalPending && static_cast<bool> (lastEval); }, 3.0 });
        steps.push_back ({ "hold for screenshots (column)", [] {}, [this] { return now() - stepStart >= holdSec(); }, holdSec() + 5.0 });
        steps.push_back ({ "a narrow window puts the panel in a closed drawer",
                           [this]
                           {
                               editor->setSize (1000, 700);
                               proc->startAssistantCapture (strat3::assistant::CaptureKind::Reference, 30.0);
                           },
                           [this] { return editor->drawerMode() && ! editor->panelShown() && now() - stepStart > 0.5; } });
        steps.push_back ({ "opening the drawer shows the state that changed while it was closed",
                           [this] { editor->setDrawerOpen (true); },
                           [this] { return editor->panelShown() && pageShows ("Capturing reference"); } });
        steps.push_back ({ "hold for screenshots (drawer)", [] {}, [this] { return now() - stepStart >= holdSec(); }, holdSec() + 5.0 });
    }

    static double holdSec()
    {
        return juce::SystemStats::getEnvironmentVariable ("STRAT3_WEBVIEW_HOLD_MS", "0").getDoubleValue() / 1000.0;
    }

    void timerCallback() override
    {
        editor->tick (0.02f);   // what the editor's own 30 Hz timer does, at the check's pace
        if (current >= steps.size())
            return finish();

        auto& step = steps[current];
        if (! started)
        {
            started = true;
            stepStart = now();
            step.start();
            return;
        }
        if (step.done())
        {
            std::cout << "PASS  " << step.name << std::endl;
            next();
        }
        else if (now() - stepStart > step.timeoutSec)
        {
            std::cout << "FAIL  " << step.name << "  (page text: " << pageText.replace ("\n", " | ").substring (0, 300) << ")" << std::endl;
            ++failures;
            next();
        }
    }

    void next()
    {
        ++current;
        started = false;
        lastEval = juce::var();
        pageText = {};
    }

    void finish()
    {
        stopTimer();
        // Closing with the page loaded: the editor and its WebView go before the processor.
        window->clearContentComponent();
        editor.reset();
        std::cout << "PASS  the editor closes with the page loaded" << std::endl;
        std::cout << (failures == 0 ? "ALL PASS" : juce::String (failures) + " FAILED") << std::endl;
        setApplicationReturnValue (failures == 0 ? 0 : 1);
        quit();
    }
};

} // namespace

START_JUCE_APPLICATION (Check)
