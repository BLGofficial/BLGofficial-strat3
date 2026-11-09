#pragma once
// One stage of the stack (Phase 4): label column (marker, name, handle, ON / delta or LINK),
// the impact strip (20 Hz..20 kHz heat line from the impact map), the stats column, and, when
// expanded, the stage's full parameter set in place. Message thread only.
#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <functional>
#include <memory>
#include <vector>

#include "analysis/ImpactAnalyser.h"
#include "ui/model/UiModel.h"
#include "ui/native/EqEditor.h"
#include "ui/native/ValueHandle.h"

namespace strat3::ui {

/** Generic in-place editor: one control per parameter with the given prefix. The EQ row uses
 *  EqEditor instead (module 20); later bespoke editors replace it per layer the same way. */
class LayerPanel final : public juce::Component
{
public:
    LayerPanel (juce::AudioProcessorValueTreeState&, const juce::String& prefix, const juce::StringArray& exclude);
    int controlCount() const noexcept { return static_cast<int> (cells.size()); }
    void resized() override;
    void paint (juce::Graphics&) override;

private:
    struct Cell;
    juce::Viewport view;
    juce::Component content;
    std::vector<std::unique_ptr<Cell>> cells;
};

class LayerRow final : public juce::Component
{
public:
    LayerRow (juce::AudioProcessorValueTreeState&, RowInfo);
    ~LayerRow() override;

    const RowInfo& info() const noexcept { return rowInfo; }
    ValueHandle& handle() noexcept { return *valueHandle; }
    bool expandable() const noexcept { return ! panelPrefix.isEmpty(); }

    void setExpanded (bool);
    int panelControlCount() const noexcept
    {
        return panel != nullptr ? panel->controlCount() : (eqEd != nullptr ? eqEd->controlCount() : 0);
    }
    /** The EQ row's spectrum editor while expanded (module 20), else nullptr. */
    EqEditor* eqEditor() noexcept { return eqEd.get(); }
    /** EQ input level per band (ImpactFrame::levelDb); kept while collapsed for the next open. */
    void setSpectrum (const std::array<float, analysis::kImpactBands>& levelDb);
    void setSelected (bool);
    /** Impact row of the latest frame (gain / width per band, dB). */
    void setImpact (const std::array<float, analysis::kImpactBands>& gainDb, const std::array<float, analysis::kImpactBands>& widthDb);
    const std::array<float, analysis::kImpactBands>& impact() const noexcept { return cells; }
    void setStats (const juce::String&);
    /** Message thread, from the editor's tick: accessible title, active marker. */
    void refresh();

    std::function<void()> onToggleExpand, onSelect;
    std::function<void (int fromSlot, int contentY)> onReorderDrag, onReorderDrop;
    juce::ToggleButton* linkButton() noexcept { return link.get(); }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    juce::String name() const;
    bool active() const;

    juce::AudioProcessorValueTreeState& state;
    RowInfo rowInfo;
    juce::String panelPrefix, handleId;
    juce::RangedAudioParameter* onParam = nullptr;
    std::unique_ptr<ValueHandle> valueHandle;
    std::unique_ptr<juce::ToggleButton> on, delta, link;
    std::unique_ptr<juce::ButtonParameterAttachment> onAttach, deltaAttach;
    std::unique_ptr<LayerPanel> panel;
    std::unique_ptr<EqEditor> eqEd;   // replaces `panel` on the EQ row (module 20)
    std::array<float, analysis::kImpactBands> levels {};
    std::array<float, analysis::kImpactBands> cells {};
    juce::String stats;
    bool selected = false, reordering = false;
};

} // namespace strat3::ui
