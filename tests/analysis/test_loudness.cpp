// Loudness (ITU-R BS.1770-4 / EBU R128 style metering).
//   Audio thread: K-weighting (shelf + high-pass per channel), mean square per 100 ms block,
//   pushed to a lock-free SPSC ring (full ring drops and counts, never blocks).
//   Analysis thread: momentary (4 blocks), short-term (30 blocks), integrated (400 ms gating
//   blocks at 75% overlap, absolute gate -70 LUFS, relative gate -10 LU).
//   Delivery maths: PLR = true peak - integrated; platform penalty = how far a platform turns
//   the track down (never up).
// Expected values for the gating cases are derived analytically from the BS.1770-4 formulas
// in each test (they match the EBU Tech 3341 minimum-requirement cases as far as I recall
// them [INFERRED]; the arithmetic, not the recollection, is what the tests check).
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <numbers>
#include <thread>
#include <vector>

#include "analysis/Loudness1770.h"
#include "support/Analysis.h"

using namespace strat3::analysis;
using namespace strat3::test;

namespace {

constexpr double kTwoPi = 2.0 * std::numbers::pi;

struct Meter
{
    std::unique_ptr<LoudnessTap> tap = std::make_unique<LoudnessTap>();
    std::unique_ptr<LoudnessMeter> meter = std::make_unique<LoudnessMeter>();
    double fs = 48000.0;

    explicit Meter (double sampleRate = 48000.0) : fs (sampleRate)
    {
        prepare (*tap, fs);
        prepare (*meter);
    }

    /** Feeds a stereo signal, draining after every sample so the ring never fills. */
    void feed (const std::vector<float>& l, const std::vector<float>& r)
    {
        for (size_t i = 0; i < l.size(); ++i)
        {
            push (*tap, l[i], r[i]);
            drain (*meter, *tap);
        }
    }

    void tone (double hz, double dbfs, double seconds, bool right = true)
    {
        const auto n = static_cast<size_t> (std::llround (seconds * fs));
        const double a = std::pow (10.0, dbfs / 20.0);
        std::vector<float> x (n), silent (n, 0.0f);
        for (size_t i = 0; i < n; ++i)
            x[i] = static_cast<float> (a * std::sin (kTwoPi * hz * static_cast<double> (phase++) / fs));
        feed (x, right ? x : silent);
    }

    void silence (double seconds)
    {
        const std::vector<float> z (static_cast<size_t> (std::llround (seconds * fs)), 0.0f);
        feed (z, z);
    }

    std::uint64_t phase = 0;
};

/** Exact BS.1770-4 integrated loudness from the full list of 100 ms block energies. */
double referenceIntegrated (const std::vector<double>& blocks)
{
    std::vector<double> gating;
    for (size_t j = 3; j < blocks.size(); ++j)
        gating.push_back (0.25 * (blocks[j] + blocks[j - 1] + blocks[j - 2] + blocks[j - 3]));
    auto lufs = [] (double e) { return -0.691 + 10.0 * std::log10 (e); };
    double sum = 0.0;
    int n = 0;
    for (double e : gating)
        if (e > 0.0 && lufs (e) > -70.0) { sum += e; ++n; }
    if (n == 0)
        return -std::numeric_limits<double>::infinity();
    const double rel = lufs (sum / n) - 10.0;
    sum = 0.0;
    n = 0;
    for (double e : gating)
        if (e > 0.0 && lufs (e) > -70.0 && lufs (e) > rel) { sum += e; ++n; }
    return lufs (sum / n);
}

double lufsOfMean (std::initializer_list<std::pair<double, double>> secondsAndLufs)
{
    double e = 0.0, t = 0.0;
    for (auto [s, l] : secondsAndLufs)
    {
        e += s * std::pow (10.0, (l + 0.691) / 10.0);
        t += s;
    }
    return -0.691 + 10.0 * std::log10 (e / t);
}

} // namespace

// ------------------------------------------------------------------- K-weighting

TEST_CASE ("K-weighting at 48 kHz matches the BS.1770-4 coefficient tables", "[analysis][loudness][kweight]")
{
    const auto k = designKWeighting (48000.0);
    // Stage 1 (high shelf) and stage 2 (RLB high-pass), BS.1770-4 Tables 1 and 2.
    CHECK (std::abs (k.shelf.b0 - 1.53512485958697) < 1e-6);
    CHECK (std::abs (k.shelf.b1 - -2.69169618940638) < 1e-6);
    CHECK (std::abs (k.shelf.b2 - 1.19839281085285) < 1e-6);
    CHECK (std::abs (k.shelf.a1 - -1.69065929318241) < 1e-6);
    CHECK (std::abs (k.shelf.a2 - 0.73248077421585) < 1e-6);
    CHECK (k.highpass.b0 == 1.0);
    CHECK (k.highpass.b1 == -2.0);
    CHECK (k.highpass.b2 == 1.0);
    CHECK (std::abs (k.highpass.a1 - -1.99004745483398) < 1e-6);
    CHECK (std::abs (k.highpass.a2 - 0.99007225036621) < 1e-6);
}

TEST_CASE ("K-weighting has the same response at every supported sample rate", "[analysis][loudness][kweight]")
{
    const double fs = GENERATE (44100.0, 88200.0, 96000.0, 192000.0);
    CAPTURE (fs);
    const auto ref = designKWeighting (48000.0), k = designKWeighting (fs);
    for (double hz : { 20.0, 40.0, 100.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
    {
        const double a = kWeightingGainDb (k, hz, fs), b = kWeightingGainDb (ref, hz, 48000.0);
        CAPTURE (hz, a, b);
        CHECK (std::abs (a - b) < 0.02);
    }
    // Shape anchors: about +4 dB at high frequencies, and the RLB roll-off at the bottom.
    CHECK (kWeightingGainDb (k, 10000.0, fs) > 3.9);
    CHECK (kWeightingGainDb (k, 20.0, fs) < -10.0);
}

// ------------------------------------------------------------------- absolute calibration

TEST_CASE ("a stereo 1 kHz sine at -23 dBFS reads -23.0 LUFS on every window", "[analysis][loudness][calibration]")
{
    const double fs = GENERATE (44100.0, 48000.0, 96000.0, 192000.0);
    CAPTURE (fs);
    Meter m (fs);
    m.tone (1000.0, -23.0, 20.0);
    CHECK (std::abs (momentaryLufs (*m.meter) - -23.0f) < 0.05f);
    CHECK (std::abs (shortTermLufs (*m.meter) - -23.0f) < 0.05f);
    CHECK (std::abs (integratedLufs (*m.meter) - -23.0f) < 0.05f);
}

TEST_CASE ("block energy 1.0 is -0.691 LUFS: the BS.1770 offset", "[analysis][loudness][calibration]")
{
    CHECK (energyToLufs (1.0) == static_cast<float> (-0.691));
}

TEST_CASE ("the audio path applies the designed K-curve at low and high frequencies", "[analysis][loudness][calibration]")
{
    const auto k = designKWeighting (48000.0);
    for (double hz : { 40.0, 100.0, 5000.0 })
    {
        Meter m;
        m.tone (hz, -23.0, 10.0);
        const double expected = -23.0 + kWeightingGainDb (k, hz, 48000.0) - kWeightingGainDb (k, 1000.0, 48000.0);
        CAPTURE (hz, expected, integratedLufs (*m.meter));
        CHECK (std::abs (static_cast<double> (integratedLufs (*m.meter)) - expected) < 0.02);
    }
}

TEST_CASE ("level scales 1:1: -33 dBFS reads -33.0 LUFS", "[analysis][loudness][calibration]")
{
    Meter m;
    m.tone (1000.0, -33.0, 20.0);
    CHECK (std::abs (integratedLufs (*m.meter) - -33.0f) < 0.05f);
}

TEST_CASE ("one channel only reads 3.01 dB lower: channels sum in power", "[analysis][loudness][calibration]")
{
    Meter m;
    m.tone (1000.0, 0.0, 5.0, false);
    CHECK (std::abs (integratedLufs (*m.meter) - -3.01f) < 0.05f);
}

// ------------------------------------------------------------------- gating

TEST_CASE ("relative gate drops the quiet passages: -36 / -23 / -36 reads -23.0", "[analysis][loudness][gating]")
{
    Meter m;
    m.tone (1000.0, -36.0, 10.0);
    m.tone (1000.0, -23.0, 60.0);
    m.tone (1000.0, -36.0, 10.0);
    // Ungated mean ~ -24.2, gate ~ -34.2: every -36 block is out; only transition blocks blur.
    CHECK (std::abs (integratedLufs (*m.meter) - -23.0f) < 0.1f);
}

TEST_CASE ("absolute gate drops blocks at -72 before the relative gate is computed", "[analysis][loudness][gating]")
{
    Meter m;
    m.tone (1000.0, -72.0, 10.0);
    m.tone (1000.0, -36.0, 10.0);
    m.tone (1000.0, -23.0, 60.0);
    m.tone (1000.0, -36.0, 10.0);
    m.tone (1000.0, -72.0, 10.0);
    CHECK (std::abs (integratedLufs (*m.meter) - -23.0f) < 0.1f);
}

TEST_CASE ("blocks above the relative gate are energy-averaged, not dB-averaged", "[analysis][loudness][gating]")
{
    Meter m;
    m.tone (1000.0, -26.0, 20.0);
    m.tone (1000.0, -20.0, 20.1);
    m.tone (1000.0, -26.0, 20.0);
    const double expected = lufsOfMean ({ { 40.0, -26.0 }, { 20.1, -20.0 } });   // = -23.0
    CHECK (std::abs (expected - -23.0) < 0.01);
    CHECK (std::abs (integratedLufs (*m.meter) - static_cast<float> (expected)) < 0.1f);
}

TEST_CASE ("silence and sub-gate material read minus infinity, never a number", "[analysis][loudness][gating]")
{
    Meter m;
    m.silence (5.0);
    CHECK (momentaryLufs (*m.meter) == -std::numeric_limits<float>::infinity());
    CHECK (shortTermLufs (*m.meter) == -std::numeric_limits<float>::infinity());
    CHECK (integratedLufs (*m.meter) == -std::numeric_limits<float>::infinity());
    m.tone (1000.0, -75.0, 5.0);
    CHECK (std::abs (momentaryLufs (*m.meter) - -75.0f) < 0.1f);   // momentary is ungated
    CHECK (integratedLufs (*m.meter) == -std::numeric_limits<float>::infinity());
}

TEST_CASE ("integrated matches an exact reference within 0.001 LU on varying program", "[analysis][loudness][gating]")
{
    // 10 minutes of 100 ms blocks whose loudness wanders between -80 and -5 LUFS. The
    // histogram sums energies exactly; only the bin straddling the relative gate is decided
    // as a whole, so the error stays near float precision.
    const auto seed = GENERATE (range (1, 21));
    auto meter = std::make_unique<LoudnessMeter>();
    prepare (*meter);
    WhiteNoise w;
    w.counter = static_cast<std::uint64_t> (seed);
    std::vector<double> blocks;
    double level = -20.0;
    for (int j = 0; j < 6000; ++j)
    {
        level = std::clamp (level + 3.0 * static_cast<double> (w.next()), -80.0, -5.0);
        const double e = std::pow (10.0, (level + 0.691) / 10.0);
        blocks.push_back (e);
        addBlock (*meter, e);
    }
    const double ref = referenceIntegrated (blocks);
    CAPTURE (ref, integratedLufs (*meter));
    CHECK (std::abs (static_cast<double> (integratedLufs (*meter)) - ref) < 1e-3);
}

// ------------------------------------------------------------------- windows and timing

TEST_CASE ("blocks are exactly 100 ms and independent of how the host chunks audio", "[analysis][loudness][timing]")
{
    for (double fs : { 44100.0, 48000.0, 96000.0 })
    {
        auto tap = std::make_unique<LoudnessTap>();
        prepare (*tap, fs);
        CHECK (blockLength (*tap) == static_cast<int> (std::lround (0.1 * fs)));
    }
    // One sample at a time vs. odd chunks drained between: same blocks, bit for bit.
    auto a = std::make_unique<LoudnessTap>(), b = std::make_unique<LoudnessTap>();
    prepare (*a, 48000.0);
    prepare (*b, 48000.0);
    WhiteNoise w;
    std::vector<double> ea, eb;
    double e = 0.0;
    for (int i = 0; i < 48000 * 3 + 123; ++i)
    {
        const float l = 0.3f * w.next(), r = 0.3f * w.next();
        push (*a, l, r);
        while (pop (*a, e)) ea.push_back (e);
        push (*b, l, r);
        if (i % 977 == 0)
            while (pop (*b, e)) eb.push_back (e);
    }
    while (pop (*b, e)) eb.push_back (e);
    CHECK (ea.size() == 30);
    CHECK (ea == eb);
}

TEST_CASE ("momentary settles in exactly 400 ms, short-term in exactly 3 s", "[analysis][loudness][timing]")
{
    Meter m;
    m.silence (5.0);
    m.tone (1000.0, -20.0, 0.2);   // 2 of 4 momentary blocks: half the energy
    CHECK (std::abs (momentaryLufs (*m.meter) - -23.01f) < 0.05f);
    m.tone (1000.0, -20.0, 0.2);
    CHECK (std::abs (momentaryLufs (*m.meter) - -20.0f) < 0.05f);
    m.tone (1000.0, -20.0, 1.1);   // 15 of 30 short-term blocks
    CHECK (std::abs (shortTermLufs (*m.meter) - -23.01f) < 0.05f);
    m.tone (1000.0, -20.0, 1.5);
    CHECK (std::abs (shortTermLufs (*m.meter) - -20.0f) < 0.05f);
}

TEST_CASE ("integrated uses only full 400 ms gating blocks", "[analysis][loudness][timing]")
{
    // Three 100 ms blocks make no gating block yet: integrated stays empty.
    auto meter = std::make_unique<LoudnessMeter>();
    prepare (*meter);
    const double e = std::pow (10.0, (-20.0 + 0.691) / 10.0);
    for (int j = 0; j < 3; ++j)
        addBlock (*meter, e);
    CHECK (integratedLufs (*meter) == -std::numeric_limits<float>::infinity());
    addBlock (*meter, e);
    CHECK (std::abs (integratedLufs (*meter) - -20.0f) < 0.01f);
}

TEST_CASE ("reset clears every window", "[analysis][loudness]")
{
    Meter m;
    m.tone (1000.0, -10.0, 5.0);
    reset (*m.meter);
    CHECK (momentaryLufs (*m.meter) == -std::numeric_limits<float>::infinity());
    CHECK (shortTermLufs (*m.meter) == -std::numeric_limits<float>::infinity());
    CHECK (integratedLufs (*m.meter) == -std::numeric_limits<float>::infinity());
    m.tone (1000.0, -30.0, 10.0);
    CHECK (std::abs (integratedLufs (*m.meter) - -30.0f) < 0.05f);
}

// ------------------------------------------------------------------- FIFO

TEST_CASE ("a full ring drops new blocks, counts them, and never blocks", "[analysis][loudness][fifo]")
{
    auto tap = std::make_unique<LoudnessTap>();
    prepare (*tap, 48000.0);
    const int blocks = kLoudnessRingSize + 10;
    for (int i = 0; i < blocks * blockLength (*tap); ++i)
        push (*tap, 0.1f, 0.1f);
    CHECK (droppedBlocks (*tap) >= 10u);
    double e = 0.0;
    int popped = 0;
    while (pop (*tap, e)) ++popped;
    CHECK (popped == kLoudnessRingSize);   // the ring holds exactly its size
    CHECK (popped + static_cast<int> (droppedBlocks (*tap)) == blocks);
    for (int i = 0; i < blockLength (*tap); ++i)   // room again: the next block arrives
        push (*tap, 0.1f, 0.1f);
    CHECK (pop (*tap, e));
}

TEST_CASE ("SPSC ring: a concurrent consumer receives every block in order", "[analysis][loudness][fifo][threads]")
{
    LoudnessRing ring;
    constexpr int n = 200000;
    std::atomic<bool> ok { true };
    std::thread consumer ([&] {
        double v = 0.0;
        for (int expected = 0; expected < n;)
            if (pop (ring, v))
            {
                if (v != static_cast<double> (expected))
                    ok = false;
                ++expected;
            }
    });
    for (int i = 0; i < n;)
        if (push (ring, static_cast<double> (i)))
            ++i;
    consumer.join();
    CHECK (ok);
}

// ------------------------------------------------------------------- delivery maths

TEST_CASE ("PLR is true peak minus integrated loudness", "[analysis][loudness][delivery]")
{
    CHECK (std::abs (peakToLoudnessRatio (-1.0f, -9.8f) - 8.8f) < 1e-5f);
    CHECK (std::isnan (peakToLoudnessRatio (-1.0f, -std::numeric_limits<float>::infinity())));
}

TEST_CASE ("platform penalty: how far a platform turns the track down, never up", "[analysis][loudness][delivery]")
{
    // Platform reference levels are [INFERRED] public figures, kept in one table.
    CHECK (std::abs (platformPenaltyDb (Platform::Spotify, -9.8f) - -4.2f) < 1e-5f);
    CHECK (std::abs (platformPenaltyDb (Platform::AppleMusic, -9.8f) - -6.2f) < 1e-5f);
    CHECK (std::abs (platformPenaltyDb (Platform::YouTube, -9.8f) - -4.2f) < 1e-5f);
    CHECK (platformPenaltyDb (Platform::Spotify, -18.0f) == 0.0f);
    CHECK (platformPenaltyDb (Platform::Spotify, -std::numeric_limits<float>::infinity()) == 0.0f);
}

// ------------------------------------------------------------------- stability

TEST_CASE ("a NaN input is caught and the meter recovers", "[analysis][loudness][stability]")
{
    Meter m;
    m.tone (1000.0, -20.0, 1.0);
    push (*m.tap, std::numeric_limits<float>::quiet_NaN(), 0.0f);
    CHECK (resetIfNotFinite (*m.tap));
    CHECK_FALSE (resetIfNotFinite (*m.tap));
    const float before = momentaryLufs (*m.meter);
    addBlock (*m.meter, std::numeric_limits<double>::quiet_NaN());   // ignored, not stored
    addBlock (*m.meter, std::numeric_limits<double>::infinity());
    addBlock (*m.meter, -1.0);
    CHECK (momentaryLufs (*m.meter) == before);
    m.tone (1000.0, -20.0, 5.0);
    CHECK (std::isfinite (integratedLufs (*m.meter)));
    CHECK (std::abs (integratedLufs (*m.meter) - -20.0f) < 0.05f);
}

TEST_CASE ("filter state never goes subnormal in silence when flushed per block", "[analysis][loudness][stability]")
{
    auto tap = std::make_unique<LoudnessTap>();
    prepare (*tap, 48000.0);
    for (int i = 0; i < 4800; ++i)
        push (*tap, 0.5f, -0.5f);
    for (int i = 0; i < 48000 * 30; ++i)
    {
        push (*tap, 0.0f, 0.0f);
        if (i % 256 == 0)
            flushDenormals (*tap);
    }
    bool sub = false;
    forEachState (*tap, [&] (double v) { sub |= std::fpclassify (v) == FP_SUBNORMAL || (v != 0.0 && std::abs (v) < 1e-30); });
    CHECK_FALSE (sub);
}
