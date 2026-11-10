#pragma once
// The advanced view of the assistant column (module 21d), without the browser: which
// parameters the page binds through JUCE's control relays, and what else it is told about
// the engine. WebPanelHost builds the relays from kAdvancedRelays; the tests read it here.
//
// Events (page side: webui/src/protocol/advanced.ts):
//   native -> page  "advancedInfo"  what the engine runs with now (advancedInfo below)
//                   "showView"      { "view": "advanced" }: the header's gear was pressed
// Relays use JUCE's own events ("__juce__slider<id>", "__juce__toggle<id>",
// "__juce__comboBox<id>", JUCE 8.0.17 juce_WebControlRelays.cpp), one relay per parameter,
// named by the parameter id.
#include <juce_core/juce_core.h>

#include <array>

#include "processor/Strat3Processor.h"

namespace strat3::ui::web {

inline constexpr const char* kInfoEvent = "advancedInfo";
inline constexpr const char* kViewEvent = "showView";

enum class RelayKind { Slider, Toggle, ComboBox };

struct RelaySpec
{
    const char* id;
    RelayKind kind;
};

/** Phase 4's advanced slide-over, less what has a home elsewhere: target capture is the
 *  Target tab and the mask cache waits for STEMS. Oversampling, dither and the sidechain
 *  also have native controls in their rows; these are the same parameters. */
inline constexpr std::array<RelaySpec, 9> kAdvancedRelays { {
    { "xover.f1", RelayKind::Slider },
    { "xover.f2", RelayKind::Slider },
    { "xover.f3", RelayKind::Slider },
    { "sat.os", RelayKind::ComboBox },
    { "ceiling.dither", RelayKind::ComboBox },
    { "ceiling.bits", RelayKind::ComboBox },
    { "mb.scSource", RelayKind::ComboBox },
    { "mb.scHpOn", RelayKind::Toggle },
    { "mb.scHpFreq", RelayKind::Slider },
} };

/** "slider", "toggle" or "comboBox", as JUCE's page helper names them. */
const char* relayKindName (RelayKind) noexcept;

/**
 * The relays as the page's contract lists them, from the parameter specs.
 * @returns [{ id, kind, name, min, max, default, unit, taper ("lin"|"log") | choices }]
 */
juce::var relayContract();

/**
 * One relay's entry in that contract. Separate so a test can give it any parameter.
 * @returns the entry, or void if no parameter has the relay's id
 */
juce::var relayEntry (const RelaySpec&);

/**
 * What the advanced view shows besides the parameters: what runs, which can differ from
 * what the parameters ask for. Message thread.
 * @returns { topology: "iir"|"fir"|null, sampleRate, crossovers: [f1, f2, f3]|null,
 *            crossoversSet: [f1, f2, f3]|null (the parameters these came from),
 *            oversampling: { requested, active|null }, sidechain: { channels } }.
 *          null: the processor is not prepared, so nothing runs yet (sampleRate is 0).
 *          requested is the factor the engine was prepared with (the parameter's while not
 *          prepared), so active < requested means the rate cap and nothing else.
 */
juce::var advancedInfo (const processor::Strat3Processor&);

} // namespace strat3::ui::web
