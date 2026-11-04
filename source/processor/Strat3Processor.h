#pragma once
// The AudioProcessor the host loads: APVTS -> engine, saved state, latency reporting, and the
// analysis thread (impact map, loudness meters, gain-match offset, MASTER FIR redesigns).
//
// Threads:
//   message   prepareToPlay / releaseResources / applyPendingConfig (re-prepare when
//             global.latencyMode or sat.os changed; both change latency, so neither is
//             automatable and neither is applied on the audio thread) / readImpact
//   audio     processBlock / processBlockBypassed: atomics and the engine only
//   analysis  analysisTick() at ~30 Hz; asks the message thread for a re-prepare when the
//             config params changed
//   any       setStateInformation (some hosts call it off the message thread): it only sets
//             parameters and asks for a re-prepare
//
// Re-prepare while the host keeps calling processBlock: a Dekker handshake on two seq_cst
// atomics (suspended / inProcess). The audio thread never waits: while suspended it outputs
// silence. The message thread waits for at most the block in flight.
#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "analysis/ImpactAnalyser.h"
#include "analysis/Loudness1770.h"
#include "engine/Engine.h"
#include "processor/ParamBridge.h"

namespace strat3::processor {

inline constexpr int kAnalysisHz = 30;
inline constexpr float kCpuSmoothing = 0.1f;          // per block, one-pole on the load reading
inline constexpr float kGainMatchGateLufs = -70.0f;   // below this (either side) keep the last offset

struct Meters
{
    float momentaryLufs = -100.0f, shortTermLufs = -100.0f, integratedLufs = -100.0f;   // output
    float inputShortTermLufs = -100.0f;
    float gainMatchDb = 0.0f;
    float cpuLoad = 0.0f;    // processBlock time / block duration
    bool nonFinite = false;  // latched until clearNonFiniteFlag()
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
    /** True when global.latencyMode or the requested sat.os differ from the prepared ones. */
    bool configPending() const noexcept;
    /** Message thread: re-prepare for a new mode / oversampling and report the new latency. */
    void applyPendingConfig();
    /** True while a re-prepare has been asked of the message thread and not yet run. */
    bool reprepareRequested() const noexcept { return isUpdatePending(); }
    /** Analysis thread (or a test with the thread off). */
    void analysisTick();

    Meters meters() const noexcept;
    void clearNonFiniteFlag() noexcept { nonFiniteLatched.store (false); }
    /** Message thread. Latest impact map (rows = engine::kRows). False if nothing new. */
    bool readImpact (analysis::ImpactFrame& out);

    engine::LatencyMode preparedMode() const noexcept { return static_cast<engine::LatencyMode> (preparedModeIndex.load()); }
    int preparedSatOs() const noexcept { return preparedOs.load(); }
    const engine::Engine& getEngine() const noexcept { return *engine; }

    /** Non-automatable state (layer order, later targets and the assistant log). Message thread. */
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

    const bool runAnalysisThread;
    std::unique_ptr<AnalysisThread> analysisThread;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Strat3Processor)
};

} // namespace strat3::processor
