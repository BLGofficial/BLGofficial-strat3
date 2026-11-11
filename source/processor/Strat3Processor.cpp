#include "processor/Strat3Processor.h"
#include "ui/Strat3Editor.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

#include "assistant/Protocol.h"
#include "assistant/SessionState.h"
#include "params/ParameterLayout.h"
#include "params/ParameterSpecs.h"
#include "params/StateMigration.h"

namespace strat3::processor {

using Set = juce::AudioChannelSet;

namespace {

void raise (std::atomic<float>& hold, float v) noexcept
{
    if (v > hold.load (std::memory_order_relaxed))   // single writer: no CAS needed
        hold.store (v, std::memory_order_relaxed);
}

float toMeterDb (float gain) noexcept
{
    return gain > 0.0f ? std::max (kMeterFloorDb, 20.0f * std::log10 (gain)) : kMeterFloorDb;
}


// Order as one int (3 bits per slot), so the audio-free threads can compare it atomically.
int encodeOrder (const engine::LayerOrder& o) noexcept
{
    int code = 0;
    for (std::size_t i = 0; i < o.size(); ++i)
        code |= static_cast<int> (o[i]) << (3 * i);
    return code;
}

engine::LayerOrder decodeOrder (int code) noexcept
{
    engine::LayerOrder o {};
    for (std::size_t i = 0; i < o.size(); ++i)
        o[i] = static_cast<engine::DepthLayer> ((code >> (3 * i)) & 7);
    return engine::isValidOrder (o) ? o : engine::kDefaultOrder;
}

} // namespace

struct Strat3Processor::AnalysisThread final : juce::Thread
{
    explicit AnalysisThread (Strat3Processor& o) : juce::Thread ("STRAT3 analysis"), owner (o) {}

    void run() override
    {
        while (! threadShouldExit())
        {
            owner.analysisTick();
            wait (1000 / kAnalysisHz);
        }
    }

    Strat3Processor& owner;
};

// STEMS work runs apart from the meters: finishing a pass and saving or loading a cache file
// can take a second or more, which would freeze the meters on the analysis thread.
struct Strat3Processor::StemsThread final : juce::Thread
{
    explicit StemsThread (Strat3Processor& o) : juce::Thread ("STRAT3 stems"), owner (o) {}

    void run() override
    {
        while (! threadShouldExit())
        {
            owner.stemsTick();
            wait (kStemsTickMs);
        }
    }

    Strat3Processor& owner;
};

Strat3Processor::Strat3Processor (bool runThread)
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", Set::stereo(), true)
                          .withOutput ("Output", Set::stereo(), true)
                          .withInput ("Sidechain", Set::stereo(), false)),
      apvts (*this, nullptr, params::kParamsType, params::createParameterLayout()),
      extraState (params::makeDefaultExtraState()),
      params (makeParamBridge (apvts)),
      engine (std::make_unique<engine::Engine>()),
      impact (std::make_unique<analysis::ImpactAnalyser>()),
      runAnalysisThread (runThread)
{
    requestedOrder.store (encodeOrder (engineOrder (params::decodeLayerOrder (extraState))));
    preparedOrderCode.store (requestedOrder.load());
    assistant::prepare (session);
    processor::setDirectory (stems, defaultStemsDirectory());
    if (runAnalysisThread)
    {
        analysisThread = std::make_unique<AnalysisThread> (*this);
        analysisThread->startThread (juce::Thread::Priority::low);
        stemsThread = std::make_unique<StemsThread> (*this);
        stemsThread->startThread (juce::Thread::Priority::low);
    }
}

Strat3Processor::~Strat3Processor()
{
    // The thread calls back into this object: stop it before any member goes.
    if (analysisThread != nullptr)
        analysisThread->stopThread (2000);
    if (stemsThread != nullptr)
        stemsThread->stopThread (10000);   // may be finishing a file write
    cancelPendingUpdate();
}

// ------------------------------------------------------------------- buses

bool Strat3Processor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    if (in != layouts.getMainOutputChannelSet() || (in != Set::mono() && in != Set::stereo()))
        return false;
    const auto sc = layouts.inputBuses.size() > 1 ? layouts.inputBuses[1] : Set::disabled();
    return sc.isDisabled() || sc == Set::mono() || sc == Set::stereo();
}

// ------------------------------------------------------------------- audio handshake

// Dekker: each side stores its own flag, then loads the other's (both seq_cst), so at least
// one of them sees the other. The audio side never waits; the message side waits for the
// block in flight only.
void Strat3Processor::suspendAudio() noexcept
{
    suspended.store (true);
    while (inProcess.load())
        std::this_thread::yield();
}

void Strat3Processor::resumeAudio() noexcept { suspended.store (false); }

bool Strat3Processor::enterAudio() noexcept
{
    inProcess.store (true);
    if (suspended.load() || ! prepared.load())
    {
        inProcess.store (false);
        return false;
    }
    return true;
}

void Strat3Processor::leaveAudio() noexcept { inProcess.store (false); }

// ------------------------------------------------------------------- prepare

void Strat3Processor::prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock)
{
    prepareEngine (sampleRate, maximumExpectedSamplesPerBlock);
}

void Strat3Processor::releaseResources()
{
    const std::lock_guard lock (analysisLock);
    suspendAudio();
    prepared.store (false);
    resumeAudio();
}

void Strat3Processor::prepareEngine (double sampleRate, int maxBlock)
{
    int latency = 0;
    {
        // Analysis first: a tick in flight finishes before the audio goes silent, so the
        // silent stretch is the prepare itself, not the prepare plus a tick.
        const std::lock_guard lock (analysisLock);   // the designer and the meters are reset too
        suspendAudio();
        const bool mono = getMainBusNumInputChannels() == 1;
        const auto* sc = getBus (true, 1);
        monoLayout.store (mono);
        sidechainChannels.store (sc != nullptr && sc->isEnabled() ? std::min (2, sc->getNumberOfChannels()) : 0);

        engine::EngineConfig cfg;
        cfg.sampleRate = sampleRate;
        cfg.maxBlock = maxBlock;
        cfg.mode = readLatencyMode (params);
        cfg.satOs = readSatOs (params);
        cfg.order = decodeOrder (requestedOrder.load());
        engine::prepare (*engine, cfg, readParams (params, mono));
        config = engine->config;   // sanitised rate and block size
        config.satOs = cfg.satOs;  // keep the requested factor: pending compares against it
        processor::prepare (stems, config.sampleRate);   // cancels a pass: its timeline is gone
        engine::setStemsCapture (*engine, &stems.ring);
        stemsLayerState.store (static_cast<int> (engine::stemsStatus (*engine)));

        latency = engine::latencySamples (*engine);
        for (auto& d : bypassDelay)
        {
            dsp::prepare (d, static_cast<std::size_t> (std::max (1, latency)));
            dsp::setDelay (d, static_cast<std::size_t> (latency));
        }
        scratchR.assign (static_cast<std::size_t> (config.maxBlock), 0.0f);

        analysis::prepare (*impact, config.sampleRate, engine::kRows);
        const auto rows = engine::rowLatencies (*engine);
        analysis::setLatencies (*impact, rows);
        analysis::prepare (loudIn);
        analysis::prepare (loudOut);
        appliedOffsetDb = 0.0f;
        engine::setGainMatchDb (*engine, 0.0f);
        for (auto* m : { &mLufs, &sLufs, &iLufs, &inSLufs })
            m->store (-100.0f);
        gainMatchOffset.store (0.0f);
        cpuLoad.store (0.0f);
        resetMeters();

        preparedModeIndex.store (static_cast<int> (cfg.mode));
        preparedOs.store (cfg.satOs);
        preparedOrderCode.store (encodeOrder (engine::layerOrder (*engine)));
        prepared.store (true);
        resumeAudio();
    }
    setLatencySamples (latency);   // outside the locks: hosts may call back synchronously
}

bool Strat3Processor::configPending() const noexcept
{
    return readLatencyMode (params) != preparedMode() || readSatOs (params) != preparedOs.load()
        || requestedOrder.load() != preparedOrderCode.load();
}

void Strat3Processor::applyPendingConfig()
{
    // Not prepared: the host's next prepareToPlay picks the config up.
    if (! prepared.load() || ! configPending())
        return;
    prepareEngine (config.sampleRate, config.maxBlock);
}

double Strat3Processor::getTailLengthSeconds() const
{
    // [INFERRED] The linear-phase FIRs ring for about as long after the delayed impulse as
    // before it, so the tail is the reported latency. IIR modes report a few samples.
    return prepared.load() ? getLatencySamples() / config.sampleRate : 0.0;
}

// ------------------------------------------------------------------- audio

void Strat3Processor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    const juce::ScopedNoDenormals noDenormals;
    if (! enterAudio())
    {
        buffer.clear();
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    const int n = buffer.getNumSamples();
    const bool mono = monoLayout.load (std::memory_order_relaxed);
    const int mainCh = mono ? 1 : 2;
    if (buffer.getNumChannels() < mainCh || n <= 0)
    {
        buffer.clear();
        leaveAudio();
        return;
    }

    float* l = buffer.getWritePointer (0);
    float* r = mono ? nullptr : buffer.getWritePointer (1);
    const float* scL = nullptr;
    const float* scR = nullptr;
    const int scCh = sidechainChannels.load (std::memory_order_relaxed);
    if (scCh > 0 && buffer.getNumChannels() >= mainCh + scCh)
    {
        scL = buffer.getReadPointer (mainCh);   // sidechain channels follow the main input
        scR = scCh == 2 ? buffer.getReadPointer (mainCh + 1) : scL;
    }

    engine::setParams (*engine, readParams (params, mono));
    engine::setStemsCache (*engine, processor::cacheForAudio (stems));

    // Where the host is: STEMS looks its masks up by timeline position, and its analysis
    // pass follows the transport.
    dsp::Transport transport;
    if (auto* head = getPlayHead())
        if (const auto pos = head->getPosition())
        {
            transport.playing = pos->getIsPlaying();
            if (const auto at = pos->getTimeInSamples())
            {
                transport.hasPosition = true;
                transport.sample = *at;
            }
        }

    // Hosts may exceed the prepared block size; the engine and scratchR are sized for it.
    const int maxBlock = config.maxBlock;
    for (int off = 0; off < n; off += maxBlock)
    {
        const int len = std::min (maxBlock, n - off);
        float* cl = l + off;
        float* cr = mono ? scratchR.data() : r + off;
        if (mono)
            std::copy (cl, cl + len, cr);
        // Bypass delay stays warm, so switching to bypass plays the right (delayed) input.
        for (int i = 0; i < len; ++i)
            dsp::process (bypassDelay[0], cl[i]);
        if (! mono)
            for (int i = 0; i < len; ++i)
                dsp::process (bypassDelay[1], cr[i]);
        auto t = transport;
        t.sample += off;
        engine::process (*engine, cl, cr, len,
                         scL != nullptr ? scL + off : nullptr,
                         scR != nullptr ? scR + off : nullptr, t);
        publishMeters (engine::blockMeters (*engine));
    }
    stemsLayerState.store (static_cast<int> (engine::stemsStatus (*engine)), std::memory_order_relaxed);

    if (engine::takeNonFiniteFlag (*engine))
        nonFiniteLatched.store (true, std::memory_order_relaxed);

    const double seconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();
    const auto load = static_cast<float> (seconds * config.sampleRate / n);
    const float old = cpuLoad.load (std::memory_order_relaxed);
    cpuLoad.store (old + kCpuSmoothing * (load - old), std::memory_order_relaxed);
    leaveAudio();
}

void Strat3Processor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    const juce::ScopedNoDenormals noDenormals;
    if (! enterAudio())
    {
        buffer.clear();
        return;
    }
    const int mainCh = monoLayout.load (std::memory_order_relaxed) ? 1 : 2;
    for (int ch = 0; ch < std::min (mainCh, buffer.getNumChannels()); ++ch)
        dsp::processBlock (bypassDelay[static_cast<std::size_t> (ch)], buffer.getWritePointer (ch), buffer.getNumSamples());
    leaveAudio();
}

// ------------------------------------------------------------------- analysis

void Strat3Processor::analysisTick()
{
    const std::lock_guard lock (analysisLock);
    if (! prepared.load())
        return;
    if (configPending())
        triggerAsyncUpdate();   // the re-prepare runs on the message thread

    if (deltaHeard())
        deltaHoldTicks = kAssistantDeltaHoldTicks;
    else if (deltaHoldTicks > 0)
        --deltaHoldTicks;
    const bool capturePause = deltaHoldTicks > 0;

    auto& e = *engine;
    analysis::drain (loudIn, e.loudIn);
    {
        // Not analysis::drain: a running capture measures its own LUFS-I from the same blocks.
        const std::lock_guard assistantGuard (assistantLock);
        double block = 0.0;
        while (analysis::pop (e.loudOut, block))
        {
            analysis::addBlock (loudOut, block);
            if (! capturePause)
                assistant::addLoudnessBlock (session, block);
        }
    }
    const float inS = analysis::shortTermLufs (loudIn);
    const float outS = analysis::shortTermLufs (loudOut);
    mLufs.store (analysis::momentaryLufs (loudOut));
    sLufs.store (outS);
    iLufs.store (analysis::integratedLufs (loudOut));
    inSLufs.store (inS);

    // Output is measured before gain match, so the offset never feeds back into itself.
    // Silence on either side keeps the last offset instead of jumping to +-24 dB.
    if (params.gainMatch->load() < 0.5f)
        appliedOffsetDb = 0.0f;
    else if (inS > kGainMatchGateLufs && outS > kGainMatchGateLufs)
        appliedOffsetDb = std::clamp (inS - outS, -engine::kGainMatchMaxDb, engine::kGainMatchMaxDb);
    engine::setGainMatchDb (e, appliedOffsetDb);
    gainMatchOffset.store (appliedOffsetDb);

    analysis::pull (*impact, e.capture);
    analysis::computeFrame (*impact);
    feedAssistant (capturePause);

    engine::requestDesigns (e, readParams (params, monoLayout.load()));
}

Meters Strat3Processor::meters() const noexcept
{
    Meters m;
    m.momentaryLufs = mLufs.load();
    m.shortTermLufs = sLufs.load();
    m.integratedLufs = iLufs.load();
    m.inputShortTermLufs = inSLufs.load();
    m.gainMatchDb = gainMatchOffset.load();
    m.cpuLoad = cpuLoad.load();
    m.nonFinite = nonFiniteLatched.load();
    m.correlation = outCorrelation.load();
    m.truePeakMaxDb = toMeterDb (tpProgramMax.load());
    if (m.truePeakMaxDb > kMeterFloorDb && m.integratedLufs > kMeterFloorDb)
        m.plr = analysis::peakToLoudnessRatio (m.truePeakMaxDb, m.integratedLufs);
    return m;
}


void Strat3Processor::publishMeters (const engine::BlockMeters& b) noexcept
{
    for (std::size_t c = 0; c < 2; ++c)
    {
        raise (inPeakHold[c], b.inPeak[c]);
        raise (outPeakHold[c], b.outPeak[c]);
    }
    raise (outTpHold, b.outTruePeak);
    raise (tpProgramMax, b.outTruePeak);
    raise (grHold, -b.grDb);   // held as a positive amount of reduction
    if (std::isfinite (b.correlation))
        outCorrelation.store (b.correlation, std::memory_order_relaxed);
    if (std::max (b.inPeak[0], b.inPeak[1]) >= 1.0f)
        inClipLatched.store (true, std::memory_order_relaxed);
    if (b.outTruePeak > 1.0f)
        outClipLatched.store (true, std::memory_order_relaxed);
}

void Strat3Processor::resetMeters() noexcept
{
    for (auto* h : { &inPeakHold[0], &inPeakHold[1], &outPeakHold[0], &outPeakHold[1], &outTpHold, &grHold, &tpProgramMax })
        h->store (0.0f);
    outCorrelation.store (1.0f);
    inClipLatched.store (false);
    outClipLatched.store (false);
}

PeakMeters Strat3Processor::takePeaks() noexcept
{
    PeakMeters m;
    for (std::size_t c = 0; c < 2; ++c)
    {
        m.inPeakDb[c] = toMeterDb (inPeakHold[c].exchange (0.0f));
        m.outPeakDb[c] = toMeterDb (outPeakHold[c].exchange (0.0f));
    }
    m.outTruePeakDb = toMeterDb (outTpHold.exchange (0.0f));
    const float gr = grHold.exchange (0.0f);
    m.grDb = gr > 0.0f ? -gr : 0.0f;
    m.inClip = inClipLatched.load();
    m.outClip = outClipLatched.load();
    return m;
}

void Strat3Processor::clearClips() noexcept
{
    inClipLatched.store (false);
    outClipLatched.store (false);
}

bool Strat3Processor::readImpact (analysis::ImpactFrame& out)
{
    // Message thread only: it is the triple buffer's single reader, and the only thread that
    // re-prepares the analyser, so no lock is needed against either.
    return prepared.load() && analysis::readLatest (*impact, out);
}

// ------------------------------------------------------------------- assistant

assistant::ParamValues Strat3Processor::currentParamValues() const
{
    assistant::ParamValues values;
    for (const auto& spec : params::allSpecs())
        if (const auto* raw = apvts.getRawParameterValue (spec.id))
            values[spec.id] = raw->load();
    return values;
}

bool Strat3Processor::deltaHeard() const noexcept
{
    // Same rule as engine::setMonitorTargets: any layer's delta (STEMS only where it runs),
    // or the global one.
    if (params.globalDelta->load (std::memory_order_relaxed) >= 0.5f)
        return true;
    if (engine::stemsInChain (preparedMode()) && params.stems.delta->load (std::memory_order_relaxed) >= 0.5f)
        return true;
    for (const auto& layer : params.layer)
        if (layer.delta->load (std::memory_order_relaxed) >= 0.5f)
            return true;
    return false;
}

void Strat3Processor::feedAssistant (bool paused)
{
    // Always taken, capture or not: a window analysed before a capture starts (or during a
    // pause) must not become a frame later.
    assistant::BandEnergies mid {}, side {};
    const bool fresh = analysis::takeOutputEnergies (*impact, mid, side);
    const bool wasPaused = capturePaused.exchange (paused);
    const std::lock_guard lock (assistantLock);
    if (session.capturing == assistant::CaptureKind::None)
        return;
    if (wasPaused != paused)
        ++session.revision;   // the panel shows why the progress stopped
    if (fresh && ! paused)
        assistant::addFrame (session, mid, side, outCorrelation.load (std::memory_order_relaxed));
    if (assistant::captureComplete (session))
        assistant::completeCapture (session, currentParamValues());
    assistantRev.store (session.revision);
}

bool Strat3Processor::startAssistantCapture (assistant::CaptureKind kind, double seconds)
{
    const std::lock_guard lock (assistantLock);
    const bool ok = assistant::startCapture (session, kind, seconds);
    assistantRev.store (session.revision);
    return ok;
}

void Strat3Processor::applyParamSet (const assistant::ParamSet& set, std::optional<double> dragAt)
{
    for (const auto& [id, value] : set)
    {
        auto* param = apvts.getParameter (id);
        if (param == nullptr || ! std::isfinite (value))
            continue;   // a move saved by another build may name a parameter this one lacks
        const auto open = dragGestures.find (id);
        if (dragAt.has_value())
        {
            // The page sends a depth command every 50 ms while the slider is dragged. One
            // gesture for the whole drag, so the host records one undo step, not twenty a second.
            if (open == dragGestures.end())
            {
                param->beginChangeGesture();
                dragGestures.emplace (id, *dragAt);
            }
            else
            {
                open->second = *dragAt;
            }
            param->setValueNotifyingHost (param->convertTo0to1 (value));
            continue;
        }
        if (open != dragGestures.end())
        {
            // Accept or reject during a drag: the drag ends here, before this command's own step.
            param->endChangeGesture();
            dragGestures.erase (open);
        }
        // One gesture per parameter: that is the unit hosts record for undo and automation.
        param->beginChangeGesture();
        param->setValueNotifyingHost (param->convertTo0to1 (value));
        param->endChangeGesture();
    }
}

void Strat3Processor::closeIdleAssistantGestures (double nowSec)
{
    JUCE_ASSERT_MESSAGE_THREAD
    for (auto it = dragGestures.begin(); it != dragGestures.end();)
    {
        if (nowSec - it->second < kDepthGestureIdleSec)
        {
            ++it;
            continue;
        }
        if (auto* param = apvts.getParameter (it->first))
            param->endChangeGesture();
        it = dragGestures.erase (it);
    }
}

void Strat3Processor::closeAssistantGestures()
{
    closeIdleAssistantGestures (std::numeric_limits<double>::infinity());
}

bool Strat3Processor::assistantCommand (const juce::var& command)
{
    return assistantCommand (command, juce::Time::getMillisecondCounterHiRes() / 1000.0);
}

bool Strat3Processor::assistantCommand (const juce::var& command, double nowSec)
{
    JUCE_ASSERT_MESSAGE_THREAD
    const auto c = assistant::parseCommand (command);
    if (! c.has_value())
        return false;

    assistant::ParamSet set;
    bool ok = true;
    {
        const std::lock_guard lock (assistantLock);
        using assistant::CommandType;
        switch (c->type)
        {
            case CommandType::Analyse:
                ok = assistant::startCapture (session, assistant::CaptureKind::Analyse, c->seconds);
                break;
            case CommandType::CaptureReference:
                ok = assistant::startCapture (session, assistant::CaptureKind::Reference, c->seconds);
                break;
            case CommandType::Cancel:
                ok = session.capturing != assistant::CaptureKind::None;
                assistant::cancelCapture (session);
                break;
            case CommandType::ClearTarget:
                assistant::clearTarget (session);
                break;
            case CommandType::ApplyAll:
                set = assistant::applyAll (session);
                break;
            case CommandType::Accept:
            case CommandType::Reject:
            case CommandType::Depth:
            {
                const auto* move = assistant::findMove (session, c->id);
                ok = move != nullptr && ! move->locked;   // locked: history from before the last analysis
                if (! ok)
                    break;
                if (c->type == CommandType::Accept)
                    set = assistant::accept (session, c->id, c->hasDepth ? c->depth : move->depth);
                else if (c->type == CommandType::Reject)
                    set = assistant::reject (session, c->id);
                else
                    set = assistant::setDepth (session, c->id, c->depth);
                break;
            }
        }
        assistantRev.store (session.revision);
    }
    // Outside the lock: parameter listeners run in here.
    applyParamSet (set, c->type == assistant::CommandType::Depth ? std::optional<double> (nowSec) : std::nullopt);
    return ok;
}

juce::var Strat3Processor::assistantState()
{
    const std::lock_guard lock (assistantLock);
    return assistant::stateToVar (session, capturePaused.load());
}

// ------------------------------------------------------------------- STEMS

bool Strat3Processor::startStemsAnalysis()
{
    // TRACK has no STEMS, so nothing would ever be captured.
    if (! prepared.load() || ! engine::stemsInChain (preparedMode()))
        return false;
    return processor::start (stems);
}

void Strat3Processor::cancelStemsAnalysis() { processor::cancel (stems); }

StemsInfo Strat3Processor::stemsInfo() const { return processor::info (stems); }

dsp::StemsStatus Strat3Processor::stemsLayerStatus() const noexcept
{
    return static_cast<dsp::StemsStatus> (stemsLayerState.load (std::memory_order_relaxed));
}

void Strat3Processor::stemsTick() { processor::tick (stems); }

void Strat3Processor::setStemsDirectory (const juce::File& dir) { processor::setDirectory (stems, dir); }

// ------------------------------------------------------------------- editor and state

juce::AudioProcessorEditor* Strat3Processor::createEditor()
{
    return new ui::Strat3Editor (*this);   // native part of the hybrid UI (module 19c)
}

void Strat3Processor::getStateInformation (juce::MemoryBlock& dest)
{
    auto extra = extraState.createCopy();
    {
        const std::lock_guard lock (assistantLock);
        extra.appendChild (assistant::toTree (session), nullptr);
    }
    // The cache itself lives in a file (tens of MB); the state names it.
    juce::ValueTree stemsTree (kStemsStateType);
    stemsTree.setProperty (kStemsCacheProperty, juce::String (processor::cacheId (stems)), nullptr);
    extra.appendChild (stemsTree, nullptr);
    const auto envelope = params::wrapState (apvts.copyState(), extra);
    if (const auto xml = envelope.createXml())
        copyXmlToBinary (*xml, dest);
}

void Strat3Processor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr)
        return;
    const auto migrated = params::migrateState (juce::ValueTree::fromXml (*xml));
    if (! migrated.has_value())
        return;   // wrong root or a newer schema: keep what we have
    const auto tree = params::unwrapParameters (*migrated);
    if (! tree.isValid())   // unwrapParameters only returns a child of the APVTS type
        return;
    apvts.replaceState (tree);
    // The session is loaded into the live object and taken out of the tree, so there is one
    // copy of it: the next save writes the session as it is then.
    auto extra = params::unwrapExtra (*migrated).createCopy();
    const auto saved = extra.getChildWithName (assistant::kAssistantType);
    {
        const std::lock_guard lock (assistantLock);
        assistant::fromTree (session, saved);
        assistantRev.store (session.revision);
    }
    extra.removeChild (saved, nullptr);
    // No STEMS child (an older state, or none saved): no cache.
    const auto stemsTree = extra.getChildWithName (kStemsStateType);
    processor::restore (stems, stemsTree.getProperty (kStemsCacheProperty).toString().toStdString());
    extra.removeChild (stemsTree, nullptr);
    extraState = extra;
    requestedOrder.store (encodeOrder (engineOrder (params::decodeLayerOrder (extraState))));
    if (prepared.load() && configPending())
        triggerAsyncUpdate();
}

// ------------------------------------------------------------------- layer order

engine::LayerOrder engineOrder (const std::vector<std::string>& ids)
{
    // Same all-or-nothing rule as EXTRA: a partly valid order would silently reroute the chain.
    if (params::sanitiseLayerOrder (ids) != ids)
        return engine::kDefaultOrder;
    engine::LayerOrder out = engine::kDefaultOrder;
    std::size_t slot = 0;
    for (const auto& id : ids)
    {
        static const std::array<std::pair<const char*, engine::DepthLayer>, engine::kDepthLayers> names {
            { { "eq", engine::kEq }, { "low", engine::kLow }, { "mb", engine::kMb },
              { "width", engine::kWidth }, { "sat", engine::kSat }, { "limit", engine::kLimit } } };
        for (const auto& [name, layer] : names)
            if (id == name && slot < out.size())
                out[slot++] = layer;   // "stems" matches nothing: skipped
    }
    return engine::isValidOrder (out) ? out : engine::kDefaultOrder;
}


void Strat3Processor::setLayerOrder (const std::vector<std::string>& ids)
{
    if (params::sanitiseLayerOrder (ids) != ids)
        return;   // not a permutation: keep the current order
    extraState = params::withLayerOrder (extraState, ids);
    requestedOrder.store (encodeOrder (engineOrder (ids)));
    if (prepared.load() && configPending())
        triggerAsyncUpdate();
}

std::vector<std::string> Strat3Processor::layerOrder() const { return params::decodeLayerOrder (extraState); }

engine::LayerOrder Strat3Processor::preparedOrder() const noexcept { return decodeOrder (preparedOrderCode.load()); }

} // namespace strat3::processor
