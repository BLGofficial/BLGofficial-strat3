#pragma once
// Header (Phase 4 hero controls) and delivery bar (Phase 4 metering + the ARCHON loudness
// readouts and clip LEDs). Message thread only; the delivery bar shows what the editor's tick
// hands it and never reads the processor itself.
#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <functional>
#include <memory>

namespace strat3::ui {

/** A square button with a gear drawn in the theme (Phase 4 wireframe: [⚙]). No glyph: the
 *  label font may not have one. */
class GearButton final : public juce::Button
{
public:
    GearButton() : juce::Button ("Advanced") {}
    void paintButton (juce::Graphics&, bool over, bool down) override;
};

class Header final : public juce::Component
{
public:
    explicit Header (juce::AudioProcessorValueTreeState&);
    ~Header() override;
    void paint (juce::Graphics&) override;
    void resized() override;

    /** The assistant drawer's toggle: shown only while the window is too narrow for the
     *  column (module 21c). */
    juce::TextButton& drawerButton() noexcept { return assistant; }
    void setDrawerMode (bool);
    /** The gear (module 21d): opens the advanced view. Shown only while the column holds
     *  the WebView, which is the only panel that has one. */
    juce::Button& advancedButton() noexcept { return gear; }
    void setAdvancedAvailable (bool);

private:
    juce::RangedAudioParameter& modeParam;
    juce::ParameterAttachment modeAttach;
    std::array<juce::TextButton, 3> modes;
    juce::ComboBox target;
    juce::ToggleButton gainMatch { "GAIN-MATCH" }, delta;
    juce::TextButton assistant { "ASSISTANT" };
    GearButton gear;
    std::unique_ptr<juce::ComboBoxParameterAttachment> targetAttach;
    std::unique_ptr<juce::ButtonParameterAttachment> gainMatchAttach, deltaAttach;
};

struct DeliveryReadout
{
    juce::String line1, line2;              // fixed-decimal readouts (15 Hz)
    std::array<float, 2> inDb {}, outDb {}; // bar meters (30 Hz, with fall-back)
    bool inClip = false, outClip = false, nonFinite = false;
};

class DeliveryBar final : public juce::Component
{
public:
    DeliveryBar();
    void show (const DeliveryReadout&);
    const DeliveryReadout& shown() const noexcept { return r; }
    std::function<void()> onClearClips, onClearNonFinite;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    juce::Rectangle<int> meterArea() const;
    DeliveryReadout r;
};

} // namespace strat3::ui
