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
//   - the header gear opens the advanced view (module 21d), which shows the processor's
//     parameters through JUCE's relays: a drag on a crossover slider moves the parameter in
//     one host gesture, a choice and the high-pass switch reach their parameters, and a
//     parameter moved natively shows on the page
//   - a state that changed while the drawer was closed is on screen when it opens, and the
//     gear opens the closed drawer on the advanced view
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

// A pointer drag on a range input, as the browser reports one: pointerdown on the slider,
// input events while it moves (through the native setter, or React would not see them), and
// pointerup wherever the hand is let go (here the document, outside the slider).
juce::String dragScript (const juce::String& name, const juce::StringArray& positions)
{
    return "(() => { const s = document.querySelector('input[aria-label=' + " + juce::JSON::toString (juce::JSON::toString (name)) + " + ']');"
           " if (!s || s.disabled) return false;"
           " const set = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set;"
           " s.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true }));"
           " for (const p of [" + positions.joinIntoString (",") + "]) { set.call(s, String(p)); s.dispatchEvent(new Event('input', { bubbles: true })); }"
           " document.dispatchEvent(new PointerEvent('pointerup', { bubbles: true }));"
           " return true; })()";
}

juce::String sliderTextScript (const juce::String& name)
{
    return "(() => { const s = document.querySelector('input[aria-label=' + " + juce::JSON::toString (juce::JSON::toString (name)) + " + ']');"
           " return s && !s.disabled ? s.getAttribute('aria-valuetext') : ''; })()";
}

juce::String clickTabScript (const juce::String& label)
{
    return "(() => { const t = [...document.querySelectorAll('[role=tab]')].find(x => x.textContent === " + juce::JSON::toString (label)
           + "); if (!t) return false; t.click(); return true; })()";
}

juce::String selectedTabScript (const juce::String& label)
{
    return "(() => { const t = [...document.querySelectorAll('[role=tab]')].find(x => x.textContent === " + juce::JSON::toString (label)
           + "); return t ? t.getAttribute('aria-selected') : ''; })()";
}

/** Counts the host gestures one parameter gets, as a host would see them. */
struct GestureCount final : juce::AudioProcessorListener
{
    int index = -1, begun = 0, ended = 0;
    void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override {}
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override {}
    void audioProcessorParameterChangeGestureBegin (juce::AudioProcessor*, int i) override { begun += i == index; }
    void audioProcessorParameterChangeGestureEnd (juce::AudioProcessor*, int i) override { ended += i == index; }
};

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
        gestures.index = proc->apvts.getParameter ("xover.f1")->getParameterIndex();
        proc->addListener (&gestures);
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
        {
            proc->removeListener (&gestures);
            proc->releaseResources();
        }
        proc.reset();
    }

    void systemRequestedQuit() override { quit(); }

private:
    std::unique_ptr<Strat3Processor> proc;
    std::unique_ptr<Strat3Editor> editor;
    std::unique_ptr<juce::DocumentWindow> window;
    strat3::ui::web::WebPanelHost* host = nullptr;
    GestureCount gestures;

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

    float param (const char* id) const { return proc->apvts.getRawParameterValue (id)->load(); }

    void setParam (const char* id, float value)
    {
        auto* p = proc->apvts.getParameter (id);
        p->setValueNotifyingHost (p->convertTo0to1 (value));
    }

    /** Runs `script` once and is done when it has answered `expected`. */
    bool answered (const juce::var& expected)
    {
        return ! evalPending && lastEval == expected;
    }

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

        // The advanced view (module 21d).
        steps.push_back ({ "the header gear opens the advanced view", [this] { editor->advancedButton().triggerClick(); },
                           [this] { return pageShows ("Crossover") && pageShows ("Linkwitz-Riley (IIR), as in TRACK and MIX"); } });
        steps.push_back ({ "the relays bring the parameter values to the page", [this] { eval (sliderTextScript ("Crossover 1")); },
                           [this]
                           {
                               if (! evalPending && lastEval != juce::var ("120 Hz"))
                                   eval (sliderTextScript ("Crossover 1"));   // not there yet: ask again
                               return answered ("120 Hz");
                           } });
        steps.push_back ({ "a drag on Crossover 1 moves the parameter, on a log scale",
                           [this] { eval (dragScript ("Crossover 1", { "250", "500" })); },
                           [this] { return std::abs (param ("xover.f1") - 141.42f) < 0.5f; } });
        steps.push_back ({ "the drag was one host gesture", [] {}, [this] { return gestures.begun == 1 && gestures.ended == 1; }, 2.0 });
        steps.push_back ({ "a choice from the page reaches its parameter (4x)", [this] { eval (clickScript ("4x")); },
                           [this] { return param ("sat.os") == 2.0f; } });
        steps.push_back ({ "the high-pass switch reaches its parameter", [this] { eval (clickScript ("High-pass")); },
                           [this] { return param ("mb.scHpOn") > 0.5f; } });
        steps.push_back ({ "a parameter moved by the host shows on the page", [this] { setParam ("xover.f2", 2000.0f); },
                           [this]
                           {
                               if (! evalPending && lastEval != juce::var ("2.00 kHz"))
                                   eval (sliderTextScript ("Crossover 2"));
                               return answered ("2.00 kHz");
                           } });
        steps.push_back ({ "hold for screenshots (advanced)", [] {}, [this] { return now() - stepStart >= holdSec(); }, holdSec() + 5.0 });
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
        // React renders a click after the script that made it returns: ask again until it has.
        steps.push_back ({ "back on the Assistant tab", [this] { eval (clickTabScript ("Assistant")); },
                           [this]
                           {
                               if (! evalPending && lastEval != juce::var ("true"))
                                   eval (selectedTabScript ("Assistant"));
                               return answered ("true");
                           } });
        steps.push_back ({ "the drawer closed", [this] { editor->setDrawerOpen (false); }, [this] { return ! editor->panelShown(); } });
        steps.push_back ({ "the gear opens the drawer on the advanced view", [this] { editor->advancedButton().triggerClick(); },
                           [this]
                           {
                               if (! evalPending && lastEval != juce::var ("true"))
                                   eval (selectedTabScript ("Advanced"));
                               return editor->panelShown() && answered ("true");
                           } });
        steps.push_back ({ "and shows it", [] {}, [this] { return pageShows ("Crossover"); } });
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
