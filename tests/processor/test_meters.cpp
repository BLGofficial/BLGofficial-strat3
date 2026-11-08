// Delivery metering (module 19b): the numbers the delivery bar and the I/O meters show that
// the loudness meters do not (Phase 4 delivery bar + the ARCHON loudness box, Blago 2026-11-04).
// Contracts tested here:
//   - sample peaks per channel, input and output, held until the editor takes them
//   - output peaks are those of the printed audio (checked against the output buffer); the
//     output true peak (BS.1770 4x) reads the crest wherever the chain's phase puts the samples
// Every measurement skips the first 0.5 s: an abrupt sine onset through the band split's
// all-pass overshoots by ~1 dB at the output, and the meters (correctly) see that.
//   - the program true-peak max survives takes, resets on re-prepare, and gives PLR
//   - correlation of the output: +1 mono, -1 inverted, ~0 unrelated
//   - clip LEDs latch (input >= 0 dBFS, output true peak > 0 dBTP) until cleared
//   - gain reduction: 0 when nothing reduces, negative when the ceiling or limiter works
//   - a non-finite block does not leave the meters non-finite
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <memory>
#include <numbers>
#include <random>
#include <string>
#include <vector>

#include "analysis/TruePeak.h"
#include "processor/Strat3Processor.h"

using namespace strat3;
using processor::Strat3Processor;
using Catch::Approx;

namespace {

constexpr double kFs = 48000.0;
constexpr int kBlock = 512;

struct JuceScope
{
    juce::ScopedJuceInitialiser_GUI init;   // APVTS Timer + AsyncUpdater need a message manager
};

std::unique_ptr<Strat3Processor> makeProc() { return std::make_unique<Strat3Processor> (false); }

void setParam (Strat3Processor& p, const std::string& id, float value)
{
    auto* param = p.apvts.getParameter (id);
    REQUIRE (param != nullptr);
    param->setValueNotifyingHost (param->convertTo0to1 (value));
}

std::vector<float> sine (int n, double hz, float amp, double phase = 0.0)
{
    std::vector<float> x (static_cast<std::size_t> (n));
    for (int i = 0; i < n; ++i)
        x[static_cast<std::size_t> (i)] = amp * static_cast<float> (std::sin (2.0 * std::numbers::pi * hz * i / kFs + phase));
    return x;
}

std::vector<float> noise (int n, float rms, unsigned seed)
{
    std::mt19937 rng (seed);
    const float a = rms * std::sqrt (3.0f);
    std::uniform_real_distribution<float> d (-a, a);
    std::vector<float> x (static_cast<std::size_t> (n));
    for (auto& v : x)
        v = d (rng);
    return x;
}

/** Runs l / r in place through processBlock. */
void process (Strat3Processor& p, std::vector<float>& l, std::vector<float>& r, int tickEvery = 0)
{
    juce::MidiBuffer midi;
    const int n = static_cast<int> (l.size());
    int count = 0;
    for (int off = 0; off < n; off += kBlock)
    {
        const int len = std::min (kBlock, n - off);
        float* ptrs[] { l.data() + off, r.data() + off };
        juce::AudioBuffer<float> buf (ptrs, 2, len);
        p.processBlock (buf, midi);
        if (tickEvery > 0 && ++count % tickEvery == 0)
            p.analysisTick();
    }
}

void runStereo (Strat3Processor& p, std::vector<float> l, std::vector<float> r, int tickEvery = 0)
{
    process (p, l, r, tickEvery);
}

/** Runs 0.5 s of the signal and drops what the meters held (the onset). */
void warmUp (Strat3Processor& p, const std::vector<float>& l, const std::vector<float>& r)
{
    runStereo (p, std::vector<float> (l.begin(), l.begin() + 24000), std::vector<float> (r.begin(), r.begin() + 24000));
    (void) p.takePeaks();
}

float bufferPeak (const std::vector<float>& x)
{
    float m = 0.0f;
    for (float v : x)
        m = std::max (m, std::abs (v));
    return m;
}

/** Reference true peak of a steady buffer. The detector's first kTruePeakTaps outputs are
 *  skipped: its empty history would read the buffer start as an abrupt onset. */
float bufferTruePeak (const std::vector<float>& x)
{
    analysis::TruePeakDetector d;
    float m = 0.0f;
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        const float v = analysis::push (d, x[i]);
        if (i >= static_cast<std::size_t> (analysis::kTruePeakTaps))
            m = std::max (m, v);
    }
    return m;
}

double db (double g) { return 20.0 * std::log10 (g); }

// 12 kHz at 48 kHz, 45 degrees: every sample sits at +-amp/sqrt 2, the crest falls between.
std::vector<float> interSample (int n, float amp) { return sine (n, kFs / 4.0, amp, std::numbers::pi / 4.0); }

} // namespace

TEST_CASE ("meters: sample peaks per channel, held until taken", "[processor][meters]")
{
    const JuceScope juce;
    auto p = makeProc();
    p->prepareToPlay (kFs, kBlock);
    const auto a = sine (48000, 1000.0, 0.5f), b = sine (48000, 1000.0, 0.25f);
    warmUp (*p, a, b);
    runStereo (*p, a, b);

    const auto pk = p->takePeaks();
    CHECK (pk.inPeakDb[0] == Approx (db (0.5)).margin (0.05));
    CHECK (pk.inPeakDb[1] == Approx (db (0.25)).margin (0.05));
    CHECK (pk.outPeakDb[0] == Approx (db (0.5)).margin (0.1));    // all layers off: transparent
    CHECK (pk.outPeakDb[1] == Approx (db (0.25)).margin (0.1));

    const auto empty = p->takePeaks();   // nothing since the last take
    CHECK (empty.inPeakDb[0] == processor::kMeterFloorDb);
    CHECK (empty.outPeakDb[1] == processor::kMeterFloorDb);
    CHECK (empty.outTruePeakDb == processor::kMeterFloorDb);
}

TEST_CASE ("meters: output peaks are those of the printed audio; true peak reads the crest", "[processor][meters]")
{
    const JuceScope juce;
    auto p = makeProc();
    p->prepareToPlay (kFs, kBlock);
    const auto x = interSample (48000, 0.5f);
    warmUp (*p, x, x);
    auto l = x, r = x;
    process (*p, l, r);

    const auto pk = p->takePeaks();
    CHECK (pk.inPeakDb[0] == Approx (db (0.5 / std::sqrt (2.0))).margin (0.05));   // input samples: -9.0
    CHECK (pk.outPeakDb[0] == Approx (db (bufferPeak (l))).margin (0.01));
    CHECK (pk.outPeakDb[1] == Approx (db (bufferPeak (r))).margin (0.01));
    CHECK (pk.outTruePeakDb == Approx (db (std::max (bufferTruePeak (l), bufferTruePeak (r)))).margin (0.05));
    CHECK (pk.outTruePeakDb == Approx (db (0.5)).margin (0.3));                    // the crest: -6.0
    CHECK (pk.outTruePeakDb >= pk.outPeakDb[0] - 0.01f);

    // Input at 0 degrees: the default chain's phase at 12 kHz then puts the output samples
    // 45 degrees off the crest, so the sample and true peaks part by 3 dB.
    const auto y = sine (48000, kFs / 4.0, 0.5f);
    warmUp (*p, y, y);
    auto l2 = y, r2 = y;
    process (*p, l2, r2);
    const auto q = p->takePeaks();
    CHECK (q.outPeakDb[0] == Approx (db (bufferPeak (l2))).margin (0.01));
    CHECK (q.outTruePeakDb == Approx (db (std::max (bufferTruePeak (l2), bufferTruePeak (r2)))).margin (0.05));
    CHECK (q.outTruePeakDb - q.outPeakDb[0] > 2.5f);
}

TEST_CASE ("meters: program true-peak max holds across takes, resets on re-prepare, gives PLR", "[processor][meters]")
{
    const JuceScope juce;
    auto p = makeProc();
    p->prepareToPlay (kFs, kBlock);
    CHECK (p->meters().truePeakMaxDb == processor::kMeterFloorDb);
    CHECK (std::isnan (p->meters().plr));

    // The program max is the largest true peak any take saw (the onset overshoot included).
    float maxTaken = processor::kMeterFloorDb;
    const auto loud = sine (24000, 1000.0, 0.5f);
    for (int i = 0; i < 8; ++i)
    {
        runStereo (*p, loud, loud, 3);
        maxTaken = std::max (maxTaken, p->takePeaks().outTruePeakDb);
    }
    const auto quiet = sine (24000, 1000.0, 0.05f);
    runStereo (*p, quiet, quiet, 3);
    maxTaken = std::max (maxTaken, p->takePeaks().outTruePeakDb);   // the loud tail
    runStereo (*p, quiet, quiet, 3);
    p->analysisTick();

    const auto m = p->meters();
    CHECK (m.truePeakMaxDb == Approx (maxTaken).margin (1e-4));
    CHECK (static_cast<double> (m.truePeakMaxDb) > db (0.5) - 0.1);
    CHECK (p->takePeaks().outTruePeakDb < -20.0f);   // the take only covers the quiet part
    REQUIRE (std::isfinite (m.plr));
    CHECK (m.plr == Approx (m.truePeakMaxDb - m.integratedLufs).margin (1e-3));

    p->prepareToPlay (kFs, kBlock);
    CHECK (p->meters().truePeakMaxDb == processor::kMeterFloorDb);
}

TEST_CASE ("meters: output correlation", "[processor][meters]")
{
    const JuceScope juce;
    auto p = makeProc();
    p->prepareToPlay (kFs, kBlock);

    // 2 s each: the 300 ms window keeps e^-6.7 of the previous segment
    const auto a = noise (96000, 0.1f, 1), b = noise (96000, 0.1f, 2);
    runStereo (*p, a, a);
    CHECK (p->meters().correlation > 0.99f);

    auto inv = a;
    for (auto& v : inv)
        v = -v;
    runStereo (*p, a, inv);
    CHECK (p->meters().correlation < -0.99f);

    runStereo (*p, a, b);
    CHECK (std::abs (p->meters().correlation) < 0.15f);
}

TEST_CASE ("meters: clip LEDs latch until cleared; the ceiling guards the output in MIX", "[processor][meters]")
{
    const JuceScope juce;
    {
        auto p = makeProc();
        p->prepareToPlay (kFs, kBlock);
        const auto hot = interSample (24000, 1.3f);
        runStereo (*p, hot, hot);
        auto pk = p->takePeaks();
        CHECK_FALSE (pk.inClip);           // samples at 0.92: below 0 dBFS
        CHECK_FALSE (pk.outClip);          // MIX: true-peak ceiling at -1 dBTP

        const auto over = sine (24000, 1000.0, 1.2f);
        runStereo (*p, over, over);
        runStereo (*p, sine (24000, 1000.0, 0.1f), sine (24000, 1000.0, 0.1f));
        pk = p->takePeaks();
        CHECK (pk.inClip);                 // latched through the quiet part
        CHECK_FALSE (pk.outClip);
        CHECK (p->takePeaks().inClip);     // and through takes

        p->clearClips();
        CHECK_FALSE (p->takePeaks().inClip);
    }
    {
        // CEILING off: nothing guards the output. The input samples stay below 0 dBFS, the
        // crest between them (and so the output true peak, whatever the chain's phase) does not.
        auto p = makeProc();
        setParam (*p, "ceiling.on", 0.0f);
        p->prepareToPlay (kFs, kBlock);
        const auto hot = interSample (48000, 1.3f);
        warmUp (*p, hot, hot);
        p->clearClips();
        runStereo (*p, hot, hot);
        const auto pk = p->takePeaks();
        CHECK_FALSE (pk.inClip);
        CHECK (pk.outClip);
        CHECK (pk.outTruePeakDb == Approx (db (1.3)).margin (0.3));

        // Output samples below 0 dBFS, crest above: the output LED follows the true peak.
        const auto offCrest = sine (48000, kFs / 4.0, 1.3f);
        warmUp (*p, offCrest, offCrest);
        p->clearClips();
        runStereo (*p, offCrest, offCrest);
        const auto q = p->takePeaks();
        CHECK (q.outPeakDb[0] < 0.0f);
        CHECK (q.outClip);
    }
    {
        // Exactly full scale is a clip (1 kHz at 48 kHz hits sin = 1 every 48 samples).
        auto p = makeProc();
        p->prepareToPlay (kFs, kBlock);
        runStereo (*p, sine (4800, 1000.0, 1.0f), sine (4800, 1000.0, 0.5f));
        CHECK (p->takePeaks().inClip);
    }
}

TEST_CASE ("meters: gain reduction from the ceiling and the limiter", "[processor][meters]")
{
    const JuceScope juce;
    auto p = makeProc();
    p->prepareToPlay (kFs, kBlock);

    runStereo (*p, noise (24000, 0.05f, 3), noise (24000, 0.05f, 4));
    CHECK (p->takePeaks().grDb == 0.0f);   // nothing reduces

    runStereo (*p, noise (24000, 0.6f, 5), noise (24000, 0.6f, 6));
    const float ceilingGr = p->takePeaks().grDb;
    CHECK (ceilingGr < -1.0f);

    // Held: a burst followed by quiet still reads the burst's reduction ...
    runStereo (*p, noise (12000, 0.6f, 11), noise (12000, 0.6f, 12));
    runStereo (*p, noise (12000, 0.05f, 13), noise (12000, 0.05f, 14));
    CHECK (p->takePeaks().grDb < -1.0f);
    // ... and once taken (and the ceiling's ~50 ms release is over), quiet reads (close to)
    // none: each block starts from no reduction.
    runStereo (*p, noise (24000, 0.05f, 15), noise (24000, 0.05f, 16));
    (void) p->takePeaks();
    runStereo (*p, noise (12000, 0.05f, 21), noise (12000, 0.05f, 22));
    CHECK (p->takePeaks().grDb > -0.05f);

    setParam (*p, "limit.on", 1.0f);
    setParam (*p, "limit.thresh", -12.0f);
    runStereo (*p, noise (24000, 0.2f, 7), noise (24000, 0.2f, 8));
    (void) p->takePeaks();   // let the limiter's ramps settle
    runStereo (*p, noise (24000, 0.2f, 9), noise (24000, 0.2f, 10));
    CHECK (p->takePeaks().grDb < -3.0f);
    CHECK (p->takePeaks().grDb == 0.0f);   // held value was taken

    // An off limiter reduces nothing, whatever its detector last held.
    setParam (*p, "limit.on", 0.0f);
    runStereo (*p, noise (24000, 0.2f, 17), noise (24000, 0.2f, 18));
    (void) p->takePeaks();
    runStereo (*p, noise (24000, 0.2f, 19), noise (24000, 0.2f, 20));
    CHECK (p->takePeaks().grDb > -0.05f);
}

TEST_CASE ("meters: a non-finite block does not leave the meters non-finite", "[processor][meters]")
{
    const JuceScope juce;
    auto p = makeProc();
    p->prepareToPlay (kFs, kBlock);
    auto bad = sine (kBlock, 1000.0, 0.5f);
    bad[100] = std::numeric_limits<float>::quiet_NaN();
    runStereo (*p, bad, bad);
    const auto x = sine (48000, 1000.0, 0.5f);
    warmUp (*p, x, x);
    runStereo (*p, x, x);

    const auto pk = p->takePeaks();
    CHECK (std::isfinite (p->meters().correlation));
    CHECK (std::isfinite (pk.outTruePeakDb));
    CHECK (pk.outPeakDb[0] == Approx (db (0.5)).margin (0.2));
    CHECK (std::isfinite (p->meters().truePeakMaxDb));
}

TEST_CASE ("meters: an overflow inside the chain is metered as the muted block it prints", "[processor][meters]")
{
    const JuceScope juce;
    auto p = makeProc();
    setParam (*p, "global.inTrim", 24.0f);
    p->prepareToPlay (kFs, kBlock);
    const std::vector<float> big (4 * kBlock, 3.0e38f);   // finite input, +24 dB: inf inside
    runStereo (*p, big, big);
    auto pk = p->takePeaks();
    CHECK (pk.inPeakDb[0] > 700.0f);                        // the input really was that big
    CHECK (pk.outPeakDb[0] == processor::kMeterFloorDb);    // the output was muted
    CHECK (pk.outTruePeakDb == processor::kMeterFloorDb);
    CHECK (std::isfinite (p->meters().truePeakMaxDb));
    CHECK (std::isfinite (p->meters().correlation));

    setParam (*p, "global.inTrim", 0.0f);
    const auto x = sine (48000, 1000.0, 0.5f);
    warmUp (*p, x, x);
    runStereo (*p, x, x);
    pk = p->takePeaks();
    CHECK (pk.outPeakDb[0] == Approx (db (0.5)).margin (0.2));
    CHECK (std::isfinite (pk.outTruePeakDb));
    CHECK (p->meters().correlation > 0.99f);

    // The correlation state recovered too: it follows new audio instead of holding a value.
    auto inv = noise (96000, 0.3f, 61);   // louder than the sine: the window is energy weighted
    auto neg = inv;
    for (auto& v : neg)
        v = -v;
    runStereo (*p, inv, neg);
    CHECK (p->meters().correlation < -0.99f);
}
