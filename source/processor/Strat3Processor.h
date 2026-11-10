#pragma once
// The AudioProcessor the host loads: APVTS -> engine, saved state, latency reporting, and the
// analysis thread (impact map, loudness meters, gain-match offset, MASTER FIR redesigns).
//
// Threads:
//   message   prepareToPlay / releaseResources / applyPendingConfig (re-prepare when
//             global.latencyMode, sat.os or the layer order changed; all change latency, so none is
//             automatable and none is applied on the audio thread) / readImpact / setLayerOrder
//   audio     processBlock / processBlockBypassed: atomics and the engine only
//   analysis  analysisTick() at ~30 Hz; asks the message thread for a re-prepare when the
//             config params changed
//   any       setStateInformation (some hosts call it off the message thread): it only sets
//             parameters and asks for a re-prepare
//
// Assistant session (module 21a): one Session behind assistantLock, shared by the analysis
// thread (capture feed, proposals) and the message thread (commands, state for the panel).
// Lock order is analysisLock -> assistantLock; parameters are never set while holding either
// (a parameter listener may call back into the processor). The audio thread takes neither.
// While a delta is heard the output taps carry the difference signal, not the mix, so the
// capture is paused for that time plus kAssistantDeltaHoldTicks.
//
// Re-prepare while the host keeps calling processBlock: a Dekker handshake on two seq_cst
// atomics (suspended / inProcess). The audio thread never waits: while suspended it outputs
// silence. The message thread waits for at most the block in flight.
#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "analysis/ImpactAnalyser.h"
#include "analysis/Loudness1770.h"
#include "assistant/Session.h"
#include "engine/Engine.h"
#include "processor/ParamBridge.h"

namespace strat3::processor {

/** Layer IDs (EXTRA's layerOrder, stems included) -> engine order. Stems is skipped; anything
 *  but an exact permutation of the processing layers gives the default. */
engine::LayerOrder engineOrder (const std::vector<std::string>& ids);

inline constexpr int kAnalysisHz = 30;
inline constexpr float kCpuSmoothing = 0.1f;          // per block, one-pole on the load reading
inline constexpr float kGainMatchGateLufs = -70.0f;   // below this (either side) keep the last offset
inline constexpr float kMeterFloorDb = -100.0f;       // peaks of silence; also "nothing measured yet"
// After a delta listen ends, its audio is still inside the analysis window (43 ms), the
// chain's latency (up to ~150 ms in MASTER) and the 400 ms loudness gate: hold the assistant
// capture for half a second more. Own tuning [INFERRED].
inline constexpr int kAssistantDeltaHoldTicks = kAnalysisHz / 2;

struct Meters
{
    float momentaryLufs = -100.0f, shortTermLufs = -100.0f, integratedLufs = -100.0f;   // output
    float inputShortTermLufs = -100.0f;
    float gainMatchDb = 0.0f;
    float cpuLoad = 0.0f;    // processBlock time / block duration
    bool nonFinite = false;  // latched until clearNonFiniteFlag()
    float correlation = 1.0f;                         // output, 300 ms; +1 for silence
    float truePeakMaxDb = kMeterFloorDb;              // output program max since the last prepare
    float plr = std::numeric_limits<float>::quiet_NaN();   // truePeakMaxDb - integrated; NaN while empty
};

/** Held since the last takePeaks() (one consumer: the editor). dBFS / dBTP, floor kMeterFloorDb. */
struct PeakMeters
{
    std::array<float, 2> inPeakDb { kMeterFloorDb, kMeterFloorDb }, outPeakDb { kMeterFloorDb, kMeterFloorDb };
    float outTruePeakDb = kMeterFloorDb;
    float grDb = 0.0f;                  // most reduction (<= 0): CEILING + LIMIT detector gain
    bool inClip = false, outClip = false;   // latched until clearClips()
};

class Strat3Processor final : public juce::AudioProcessor, private juce::AsyncUpdater
{
public:
    /** runAnalysisThread = false: tests drive analysisTick() themselves (one consumer only). */
    explicit Strat3Processor (bool runAnalysisThread = true);
    ~Strat3Processor() override;

    juce::AudioProcessorValueTreeState apvts;

    // ---- juce::AudioProcessor
    const juce::String getName() const override { return "STRAT3"; }
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    double getTailLengthSeconds() const override;
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock& dest) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // ---- STRAT3
    /** True when global.latencyMode, the requested sat.os or the layer order differ from the prepared ones. */
    bool configPending() const noexcept;
    /** Message thread: re-prepare for a new mode / oversampling and report the new latency. */
    void applyPendingConfig();
    /** True while a re-prepare has been asked of the message thread and not yet run. */
    bool reprepareRequested() const noexcept { return isUpdatePending(); }
    /** Analysis thread (or a test with the thread off). */
    void analysisTick();

    Meters meters() const noexcept;
    void clearNonFiniteFlag() noexcept { nonFiniteLatched.store (false); }
    /** Editor (one consumer). Returns the peaks held since the last call and restarts the hold. */
    PeakMeters takePeaks() noexcept;
    void clearClips() noexcept;
    /** Message thread. Latest impact map (rows = engine::kRows). False if nothing new. */
    bool readImpact (analysis::ImpactFrame& out);

    /** Message thread. Sanitised (all-or-nothing); stored in EXTRA; re-prepare requested. */
    void setLayerOrder (const std::vector<std::string>& ids);
    /** Message thread. The order in EXTRA (stems included). */
    std::vector<std::string> layerOrder() const;
    engine::LayerOrder preparedOrder() const noexcept;

    engine::LatencyMode preparedMode() const noexcept { return static_cast<engine::LatencyMode> (preparedModeIndex.load()); }
    int preparedSatOs() const noexcept { return preparedOs.load(); }
    const engine::Engine& getEngine() const noexcept { return *engine; }

    // ---- assistant (module 21a)
    /** Message thread. Starts a capture of the output: ANALYSE ends in proposals, REFERENCE
     *  adds one reference to the target. False if one is running, `seconds` is outside
     *  (0, 60], or the reference list is full. */
    bool startAssistantCapture (assistant::CaptureKind kind, double seconds);
    /** Message thread. One JSON command from the panel (assistant/Protocol.h). Accepted moves
     *  are set on the parameters inside begin/endChangeGesture, so host undo and automation
     *  see them. False if the command is malformed or does not apply; nothing changes then. */
    bool assistantCommand (const juce::var& command);
    /** Any thread but audio. The whole session as JSON for the panel. */
    juce::var assistantState();
    /** Changes whenever assistantState() would: the panel polls this, not the JSON. */
    std::uint32_t assistantRevision() const noexcept { return assistantRev.load(); }

    /** Non-automatable state (layer order). The assistant session is saved next to it, from
     *  the live session, not from this tree. Message thread. */
    juce::ValueTree extraState;

private:
    void handleAsyncUpdate() override { applyPendingConfig(); }
    void prepareEngine (double sampleRate, int maxBlock);   // message thread
    void suspendAudio() noexcept;                            // message thread; waits for the block in flight
    void resumeAudio() noexcept;
    bool enterAudio() noexcept;                              // audio thread; false = output silence
    void leaveAudio() noexcept;

    struct AnalysisThread;

    ParamBridge params;
    std::unique_ptr<engine::Engine> engine;
    engine::EngineConfig config {};                     // written only while the audio is suspended

    std::atomic<bool> prepared { false }, suspended { false }, inProcess { false }, monoLayout { false };
    std::atomic<int> preparedModeIndex { static_cast<int> (engine::LatencyMode::Mix) }, preparedOs { 2 };
    std::atomic<int> requestedOrder { 0 }, preparedOrderCode { 0 };   // encodeOrder(): 3 bits per slot
    std::atomic<int> sidechainChannels { 0 };          // 0 (no bus), 1 or 2; fixed between prepares

    std::vector<float> scratchR;                        // mono: the engine's right channel
    std::array<dsp::DelayLine, 2> bypassDelay {};       // keeps bypass aligned with PDC; fed every block

    std::mutex analysisLock;                            // analysis tick vs re-prepare
    std::unique_ptr<analysis::ImpactAnalyser> impact;
    analysis::LoudnessMeter loudIn {}, loudOut {};
    float appliedOffsetDb = 0.0f;                       // analysis thread
    std::atomic<float> mLufs { -100.0f }, sLufs { -100.0f }, iLufs { -100.0f }, inSLufs { -100.0f };
    std::atomic<float> gainMatchOffset { 0.0f }, cpuLoad { 0.0f };
    std::atomic<bool> nonFiniteLatched { false };
    // Delivery metering: linear holds. One writer (audio) raises them; one reader (the editor)
    // exchanges them back to 0, so no value is lost: a raise that lands after the exchange
    // belongs to the next take.
    std::array<std::atomic<float>, 2> inPeakHold {}, outPeakHold {};
    std::atomic<float> outTpHold { 0.0f }, grHold { 0.0f }, tpProgramMax { 0.0f }, outCorrelation { 1.0f };
    std::atomic<bool> inClipLatched { false }, outClipLatched { false };
    void publishMeters (const engine::BlockMeters&) noexcept;   // audio thread
    void resetMeters() noexcept;

    std::mutex assistantLock;                           // analysis tick vs message thread; never audio
    assistant::Session session {};
    std::atomic<std::uint32_t> assistantRev { 0 };
    int deltaHoldTicks = 0;                              // analysis thread
    std::atomic<bool> capturePaused { false };
    bool deltaHeard() const noexcept;                    // any thread: reads parameter atomics
    void feedAssistant (bool paused);                    // analysis thread, inside analysisTick
    assistant::ParamValues currentParamValues() const;   // any thread but audio (allocates)
    void applyParamSet (const assistant::ParamSet&);     // message thread, no lock held

    const bool runAnalysisThread;
    std::unique_ptr<AnalysisThread> analysisThread;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Strat3Processor)
};

} // namespace strat3::processor
