#pragma once
// STRAT3 native editor (module 19c): Phase 4 layout chosen by Blago 2026-11-04.
//   header | layer stack (signal flows top to bottom) | delivery bar
// The assistant column (WebView, later module) is not shown until it exists: its width goes to
// the stack. Threads: message thread only. It reads the processor through atomics
// (meters, takePeaks), readImpact (message thread) and the APVTS; it never blocks the audio.
#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>
#include <vector>

#include "analysis/ImpactAnalyser.h"
#include "engine/Engine.h"
#include "ui/model/UiModel.h"
#include "ui/native/Bars.h"
#include "ui/native/LayerRow.h"
#include "ui/native/Theme.h"

namespace strat3::processor { class Strat3Processor; }

namespace strat3::ui {

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

private:
    void timerCallback() override { tick (1.0f / 30.0f); }
    void rebuildRows (const engine::LayerOrder&);
    void layoutRows();
    void select (int r);
    void toggleParam (const juce::String& id);
    void updateMeters (float dtSec, bool text);

    class StackContent;

    processor::Strat3Processor& proc;
    theme::LookAndFeel laf;
    Header header;
    juce::Viewport stackView;
    std::unique_ptr<StackContent> stackContent;
    DeliveryBar delivery;
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
