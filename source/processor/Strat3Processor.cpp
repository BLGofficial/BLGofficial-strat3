#include "processor/Strat3Processor.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include "params/ParameterLayout.h"
#include "params/StateMigration.h"

namespace strat3::processor {

using Set = juce::AudioChannelSet;

namespace {

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
    if (runAnalysisThread)
    {
        analysisThread = std::make_unique<AnalysisThread> (*this);
        analysisThread->startThread (juce::Thread::Priority::low);
    }
}

Strat3Processor::~Strat3Processor()
{
    // The thread calls back into this object: stop it before any member goes.
    if (analysisThread != nullptr)
        analysisThread->stopThread (2000);
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
        engine::process (*engine, cl, cr, len,
                         scL != nullptr ? scL + off : nullptr,
                         scR != nullptr ? scR + off : nullptr);
    }

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

    auto& e = *engine;
    analysis::drain (loudIn, e.loudIn);
    analysis::drain (loudOut, e.loudOut);
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
    return m;
}

bool Strat3Processor::readImpact (analysis::ImpactFrame& out)
{
    // Message thread only: it is the triple buffer's single reader, and the only thread that
    // re-prepares the analyser, so no lock is needed against either.
    return prepared.load() && analysis::readLatest (*impact, out);
}

// ------------------------------------------------------------------- editor and state

juce::AudioProcessorEditor* Strat3Processor::createEditor()
{
    return new juce::GenericAudioProcessorEditor (*this);   // the hybrid UI replaces it later
}

void Strat3Processor::getStateInformation (juce::MemoryBlock& dest)
{
    const auto envelope = params::wrapState (apvts.copyState(), extraState);
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
    extraState = params::unwrapExtra (*migrated);
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
