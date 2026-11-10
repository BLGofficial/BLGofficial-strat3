#pragma once
// STRAT3 native editor (module 19c): Phase 4 layout chosen by Blago 2026-11-04.
//   header | layer stack (signal flows top to bottom) | assistant column | delivery bar
// The assistant column (module 21c) holds the WebView panels, or a native notice saying why
// there are none. Below kAssistCollapseW it is a drawer over the right of the stack, opened
// from the header and closed whenever the window narrows. Threads: message thread only. It reads the processor through atomics
// (meters, takePeaks), readImpact (message thread) and the APVTS; it never blocks the audio.
#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>
#include <vector>

#include "analysis/ImpactAnalyser.h"
#include "engine/Engine.h"
#include "ui/model/UiModel.h"
#include "ui/native/AssistantPanel.h"
#include "ui/native/Bars.h"
#include "ui/native/LayerRow.h"
#include "ui/native/Theme.h"

namespace strat3::processor { class Strat3Processor; }

namespace strat3::ui {

namespace web { class WebPanelHost; }

class Strat3Editor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit Strat3Editor (processor::Strat3Processor&);
    ~Strat3Editor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    /** One UI frame (the 30 Hz timer calls it; tests call it directly). */
    void tick (float dtSec);

    juce::Rectangle<int> headerBounds() const { return header.getBounds(); }
    juce::Rectangle<int> stackBounds() const { return stackView.getBounds(); }
    juce::Rectangle<int> deliveryBounds() const { return delivery.getBounds(); }

    int rowCount() const noexcept { return static_cast<int> (rows.size()); }
    const RowInfo& row (int r) const { return rows[static_cast<std::size_t> (r)]->info(); }
    LayerRow& rowComponent (int r) { return *rows[static_cast<std::size_t> (r)]; }
    int selectedRow() const noexcept { return selected; }
    int expandedRow() const noexcept { return expanded; }
    void toggleExpanded (int r);
    /** Drag reorder result: layer at slot `from` goes to slot `to` (applied by the re-prepare). */
    void dropLayer (int fromSlot, int toSlot);

    juce::String deliveryText() const { return delivery.shown().line1 + " | " + delivery.shown().line2; }
    bool inClipShown() const noexcept { return delivery.shown().inClip; }
    bool outClipShown() const noexcept { return delivery.shown().outClip; }
    void clickClipLeds();
    /** ARCHON trim link (editor state, not saved with the session [INFERRED: fine until
     *  presets exist]). */
    void setTrimsLinked (bool);

    // ---- assistant column (module 21c)
    juce::Rectangle<int> panelBounds() const { return panel->getBounds(); }
    bool panelShown() const { return panel->isVisible(); }
    bool drawerMode() const noexcept { return inDrawerMode; }
    bool drawerOpen() const noexcept { return isDrawerOpen; }
    /** Opens or closes the drawer; does nothing while the column fits. */
    void setDrawerOpen (bool);
    juce::TextButton& drawerButton() noexcept { return header.drawerButton(); }
    /** The header's gear (module 21d): opens the column (or the drawer) on the advanced view. */
    juce::Button& advancedButton() noexcept { return header.advancedButton(); }
    web::PanelKind panelKind() const { return panel->kind(); }
    /** The notice's text, empty when the WebView panels are shown. */
    juce::String panelTitle() const;
    juce::String panelText() const;
    juce::Component* panelComponent() noexcept { return panel.get(); }
    juce::Component* stackComponent() noexcept { return &stackView; }
    /** The WebView host, or null when the column holds the notice. */
    web::WebPanelHost* webPanel() noexcept;
    /** Puts another panel in the column (the constructor's choice; tests give it one that
     *  fails to load, to see the editor swap in the notice). */
    void replacePanel (std::unique_ptr<AssistantPanel>);

private:
    void timerCallback() override { tick (1.0f / 30.0f); }
    void rebuildRows (const engine::LayerOrder&);
    void layoutRows();
    void select (int r);
    void toggleParam (const juce::String& id);
    void updateMeters (float dtSec, bool text);
    void layoutPanel (const Layout&);

    class StackContent;

    processor::Strat3Processor& proc;
    theme::LookAndFeel laf;
    Header header;
    juce::Viewport stackView;
    std::unique_ptr<StackContent> stackContent;
    DeliveryBar delivery;
    std::unique_ptr<AssistantPanel> panel;
    bool inDrawerMode = false, isDrawerOpen = false;
    std::vector<std::unique_ptr<LayerRow>> rows;
    engine::LayerOrder shownOrder = engine::kDefaultOrder;
    int selected = -1, expanded = -1;
    bool trimsLinked = false;

    analysis::ImpactFrame frame {};
    std::array<float, 2> inBar { kMeterFloorDb, kMeterFloorDb }, outBar { kMeterFloorDb, kMeterFloorDb };
    PeakHold tpHold {}, grHold {};
    bool inClip = false, outClip = false;
    int frameCount = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Strat3Editor)
};

} // namespace strat3::ui
