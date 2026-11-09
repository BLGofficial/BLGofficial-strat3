#pragma once
// EQ row editor (module 20c): spectrum canvas with one node per enabled band, and the band
// editor (value boxes for the selected band) below it. Replaces the generic LayerPanel for the
// EQ row only. Option A (Blago, 2026-11-09): the backdrop is the impact analyser's 1/6-octave
// level spectrum at the EQ's input (no extra audio-thread work).
// All interaction logic lives in ui/model/EqEditorModel.h; this class paints and forwards.
// Every write reaches the host inside a gesture. Message thread only.
#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <memory>
#include <vector>

#include "analysis/ImpactAnalyser.h"
#include "dsp/eq/EqTypes.h"
#include "ui/model/EqEditorModel.h"

namespace strat3::ui {

class EqEditor final : public juce::Component
{
public:
    explicit EqEditor (juce::AudioProcessorValueTreeState&);
    ~EqEditor() override;

    /** Re-reads the bands from the parameters (the editor's tick calls it). */
    void refresh();
    /** Level per 1/6-octave band at the EQ input, dB (ImpactFrame::levelDb). */
    void setSpectrum (const std::array<float, analysis::kImpactBands>& levelDb);
    const std::array<float, analysis::kImpactBands>& spectrum() const noexcept { return levels; }

    const dsp::EqSettings& settings() const noexcept { return bands; }
    int selectedBand() const noexcept { return selected; }
    void selectBand (int band);   // -1 = none; rebinds the band editor
    dsp::EqChannel view() const noexcept { return viewChannel; }
    void setView (dsp::EqChannel);

    EqPlot plot() const noexcept;                         // canvas size and current range
    juce::Rectangle<int> canvasBounds() const noexcept { return canvas; }
    juce::StringArray boundParamIds() const;              // the band editor's parameters
    int controlCount() const noexcept { return boundParamIds().size(); }

    // Pointer actions in canvas coordinates (the mouse overrides forward here; tests call them).
    void pointerDown (float x, float y);
    void pointerDrag (float dx, float dy, bool fine);    // from where the pointer went down
    void pointerUp();
    void pointerDoubleClick (float x, float y);
    void wheel (float x, float y, float notches, bool fine);

    bool keyPressed (const juce::KeyPress&) override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    static constexpr int kBandEditorH = 56;

private:
    struct Box;
    juce::RangedAudioParameter* param (int band, const char* name) const;
    void setAsGesture (juce::RangedAudioParameter*, float value);
    void rebind();
    void paintCanvas (juce::Graphics&);

    juce::AudioProcessorValueTreeState& state;
    dsp::EqSettings bands {};
    std::array<float, analysis::kImpactBands> levels {};
    int selected = -1;
    dsp::EqChannel viewChannel = dsp::EqChannel::Stereo;
    float rangeDb = 6.0f;
    juce::Rectangle<int> canvas;

    // Drag state: the band as it was when the pointer went down (drags are absolute from there).
    int dragBand = -1;
    dsp::EqBand dragStart {};
    juce::Point<float> downAt;
    bool gestureFreq = false, gestureGain = false;

    std::array<std::unique_ptr<juce::TextButton>, 3> viewButtons;
    juce::Label bandLabel;
    std::vector<std::unique_ptr<Box>> boxes;
    std::unique_ptr<Box> phaseBox;

    JUCE_DECLARE_NON_COPYABLE (EqEditor)
};

} // namespace strat3::ui
