// STEMS analysis pass, cache hand-over and the saved cache (module 22b). Contracts tested here:
//   - the cache file: what is written is read back exactly; a foreign, truncated or damaged
//     file is refused (masks that do not sum to 255 included); only ids the plugin makes ever
//     become a path (the plugin state is untrusted input)
//   - a pass: armed, it starts when the host plays, ends when the host stops (or at the mask
//     budget), and what was analysed becomes the cache, saved under a new id
//   - hand-over: a replaced cache is freed only after the audio thread has taken its successor
//   - cancel drops the pass and a new one can start at once; a re-prepare cancels a pass
//   - a saved cache is loaded by id; a missing file is reported and its id kept
//   - in the processor: Analyse while the host plays, then the stem gains apply on playback;
//     refused in TRACK; the cache reference travels with the plugin state
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstdint>
#include <chrono>
#include <numbers>
#include <thread>
#include <string>
#include <vector>

#include "processor/StemsSession.h"
#include "processor/Strat3Processor.h"

using namespace strat3;
using processor::StemsPhase;
using processor::StemsSession;
using processor::Strat3Processor;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double kFs = 48000.0;
constexpr int kBlock = 512;

int samples (double seconds) { return static_cast<int> (seconds * kFs); }

/** A folder of its own under the system temp folder, removed at the end of the test. */
struct TempDir
{
    juce::File dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile ("strat3-stems-test-" + juce::Uuid().toDashedString());
    TempDir() { dir.createDirectory(); }
    ~TempDir() { dir.deleteRecursively(); }
    int files() const { return dir.getNumberOfChildFiles (juce::File::findFiles); }
};

/** A low sine (bass) plus a quieter centred 440 Hz tone: material the separator splits. */
void program (std::vector<float>& l, std::vector<float>& r, int n)
{
    l.resize (static_cast<size_t> (n));
    r.resize (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
    {
        const double t = i / kFs;
        const auto v = static_cast<float> (0.3 * std::sin (2.0 * std::numbers::pi * 55.0 * t)
                                           + 0.05 * std::sin (2.0 * std::numbers::pi * 440.0 * t));
        l[static_cast<size_t> (i)] = v;
        r[static_cast<size_t> (i)] = v;
    }
}

dsp::Transport playingAt (std::int64_t at) { return { true, true, at }; }

/** Plays l/r into the session's capture as the engine would (the host playing from `start`),
 *  running the worker every 8 blocks. */
void play (StemsSession& s, const std::vector<float>& l, const std::vector<float>& r, std::int64_t start)
{
    const int n = static_cast<int> (l.size());
    int blocks = 0;
    for (int i = 0; i < n; i += kBlock)
    {
        const int len = std::min (kBlock, n - i);
        dsp::write (s.ring, l.data() + i, r.data() + i, len, playingAt (start + i));
        if (++blocks % 8 == 0)
            processor::tick (s);
    }
}

/** The host stops: the capture ends itself on the next block; the worker finishes the pass. */
void stop (StemsSession& s)
{
    const std::vector<float> z (kBlock, 0.0f);
    dsp::write (s.ring, z.data(), z.data(), kBlock, dsp::Transport {});
    processor::tick (s);
}

/** One whole pass over `seconds` of program from timeline position `start`. */
void analyse (StemsSession& s, double seconds, std::int64_t start = 0)
{
    std::vector<float> l, r;
    program (l, r, samples (seconds));
    REQUIRE (processor::start (s));
    play (s, l, r, start);
    stop (s);
}

void prepared (StemsSession& s, const juce::File& dir)
{
    processor::setDirectory (s, dir);
    processor::prepare (s, kFs);
}

dsp::MaskCache someCache()
{
    std::vector<float> l, r;
    program (l, r, samples (1.0));
    return dsp::separate (l.data(), r.data(), static_cast<int> (l.size()), kFs, 4800);
}

bool sameCache (const dsp::MaskCache& a, const dsp::MaskCache& b)
{
    return a.sampleRate == b.sampleRate && a.startSample == b.startSample && a.frames == b.frames
        && a.masks == b.masks && a.prints == b.prints;
}

} // namespace

// ------------------------------------------------------------------- the file

TEST_CASE ("stems file: a cache written and read back is the same cache", "[processor][stems][file]")
{
    const TempDir tmp;
    const auto cache = someCache();
    REQUIRE (cache.frames > 0);
    const auto file = tmp.dir.getChildFile ("a" + juce::String (processor::kStemsFileExtension));
    REQUIRE (processor::writeCacheFile (cache, file));
    // Compressed: the masks of a simple program shrink a lot.
    CHECK (file.getSize() < static_cast<juce::int64> (cache.masks.size() / 2));
    const auto back = processor::readCacheFile (file);
    REQUIRE (back.has_value());
    CHECK (sameCache (*back, cache));
    CHECK (tmp.files() == 1);   // no temporary file left behind
}

TEST_CASE ("stems file: a foreign, truncated or damaged file is refused", "[processor][stems][file]")
{
    const TempDir tmp;
    const auto cache = someCache();
    const auto good = tmp.dir.getChildFile ("good.s3stems");
    REQUIRE (processor::writeCacheFile (cache, good));
    juce::MemoryBlock packed;
    REQUIRE (good.loadFileAsData (packed));

    CHECK_FALSE (processor::readCacheFile (tmp.dir.getChildFile ("absent.s3stems")).has_value());

    const auto text = tmp.dir.getChildFile ("text.s3stems");
    REQUIRE (text.replaceWithText ("not a cache"));
    CHECK_FALSE (processor::readCacheFile (text).has_value());

    const auto cut = tmp.dir.getChildFile ("cut.s3stems");
    REQUIRE (cut.replaceWithData (packed.getData(), packed.getSize() / 2));
    CHECK_FALSE (processor::readCacheFile (cut).has_value());

    // Well-formed but wrong inside: one mask byte changed, so a bin no longer sums to 255.
    auto bad = cache;
    bad.masks[1000] = static_cast<std::uint8_t> (bad.masks[1000] ^ 0x10);
    const auto damaged = tmp.dir.getChildFile ("damaged.s3stems");
    REQUIRE (processor::writeCacheFile (bad, damaged));
    CHECK_FALSE (processor::readCacheFile (damaged).has_value());

    // Well-formed but claiming more frames than it holds.
    auto lying = cache;
    lying.frames += 10;
    const auto liar = tmp.dir.getChildFile ("liar.s3stems");
    REQUIRE (processor::writeCacheFile (lying, liar));
    CHECK_FALSE (processor::readCacheFile (liar).has_value());

    // ... or fewer: the bytes left over are refused too.
    auto modest = cache;
    modest.frames -= 1;
    const auto shorter = tmp.dir.getChildFile ("shorter.s3stems");
    REQUIRE (processor::writeCacheFile (modest, shorter));
    CHECK_FALSE (processor::readCacheFile (shorter).has_value());
}

TEST_CASE ("stems file: only ids the plugin makes ever become a path", "[processor][stems][file]")
{
    CHECK (processor::isValidStemsId (juce::Uuid().toDashedString().toStdString()));
    for (const std::string bad : { "", "../../secret", "a/b", "C:\\\\x",
                                   "0123456789abcdef0123456789abcdef0123",       // 36, no dashes
                                   "01234567-89AB-CDEF-0123-456789ABCDEF",       // upper case
                                   "0123456g-89ab-cdef-0123-456789abcdef",       // not hex
                                   "01234567-89ab-cdef-0123-456789abcdef.s3stems",
                                   "01234567-89ab-cdef-0123-456789abcde/" })
    {
        INFO (bad);
        CHECK_FALSE (processor::isValidStemsId (bad));
    }
    // A state naming a path is a state naming no cache.
    StemsSession s;
    const TempDir tmp;
    prepared (s, tmp.dir);
    processor::restore (s, "../../secret");
    CHECK (processor::cacheId (s).empty());
    CHECK (processor::info (s).phase == StemsPhase::Idle);
}

// ------------------------------------------------------------------- a pass

TEST_CASE ("stems session: a pass starts when the host plays and its result becomes the saved cache", "[processor][stems]")
{
    const TempDir tmp;
    StemsSession s;
    CHECK_FALSE (processor::start (s));   // not prepared
    prepared (s, tmp.dir);
    CHECK (processor::info (s).phase == StemsPhase::Idle);
    CHECK (processor::cacheForAudio (s) == nullptr);

    REQUIRE (processor::start (s));
    CHECK_FALSE (processor::start (s));   // one at a time
    CHECK (processor::info (s).phase == StemsPhase::Waiting);
    std::vector<float> l, r;
    program (l, r, samples (3.0));
    play (s, l, r, samples (10.0));
    processor::tick (s);
    const auto during = processor::info (s);
    CHECK (during.phase == StemsPhase::Analysing);
    CHECK_THAT (during.analysedSeconds, WithinAbs (3.0, 0.1));
    CHECK (during.budgetSeconds > 600.0);   // about 11 minutes at 48 kHz

    stop (s);
    const auto done = processor::info (s);
    CHECK (done.phase == StemsPhase::Ready);
    CHECK (done.lastEnd == dsp::CaptureEnd::Stopped);
    CHECK_THAT (done.cacheStartSeconds, WithinAbs (10.0, 1e-9));
    CHECK_THAT (done.cacheEndSeconds, WithinAbs (13.0, 0.05));
    CHECK (done.cacheSampleRate == kFs);
    CHECK_FALSE (done.saveFailed);
    CHECK (done.revision != during.revision);

    const auto* cache = processor::cacheForAudio (s);
    REQUIRE (cache != nullptr);
    CHECK (cache->startSample == samples (10.0));
    CHECK (cache->frames == dsp::maskFramesFor (samples (3.0)));
    // The bass is bass.
    const int bin55 = static_cast<int> (std::lround (55.0 * dsp::kMaskFft / kFs));
    CHECK (dsp::maskAt (*cache, cache->frames / 2, dsp::kStemBass, bin55) > 0.8f);

    const auto id = processor::cacheId (s);
    REQUIRE (processor::isValidStemsId (id));
    const auto file = tmp.dir.getChildFile (id + processor::kStemsFileExtension);
    REQUIRE (file.existsAsFile());
    const auto saved = processor::readCacheFile (file);
    REQUIRE (saved.has_value());
    CHECK (sameCache (*saved, *cache));
}

TEST_CASE ("stems session: a pass too short for one frame leaves things as they were", "[processor][stems]")
{
    const TempDir tmp;
    StemsSession s;
    prepared (s, tmp.dir);
    analyse (s, 0.01);   // 480 samples < 1024
    const auto i = processor::info (s);
    CHECK (i.phase == StemsPhase::Idle);
    CHECK (i.lastEnd == dsp::CaptureEnd::Stopped);
    CHECK (processor::cacheForAudio (s) == nullptr);
    CHECK (processor::cacheId (s).empty());
    CHECK (tmp.files() == 0);
}

TEST_CASE ("stems session: a cache that cannot be saved still plays, and the state does not name it", "[processor][stems]")
{
    const TempDir tmp;
    const auto notAFolder = tmp.dir.getChildFile ("occupied");
    REQUIRE (notAFolder.replaceWithText ("a file where the folder should be"));
    StemsSession s;
    prepared (s, notAFolder);
    analyse (s, 1.0);
    const auto i = processor::info (s);
    CHECK (i.phase == StemsPhase::Ready);
    CHECK (i.saveFailed);
    CHECK (processor::cacheForAudio (s) != nullptr);
    CHECK (processor::cacheId (s).empty());
}

TEST_CASE ("stems session: a replaced cache is freed only after the audio thread took its successor", "[processor][stems]")
{
    const TempDir tmp;
    StemsSession s;
    prepared (s, tmp.dir);
    analyse (s, 1.0);
    const auto* a = processor::cacheForAudio (s);
    REQUIRE (a != nullptr);

    analyse (s, 1.5);
    CHECK (processor::retiredCount (s) == 1);
    processor::tick (s);
    CHECK (processor::retiredCount (s) == 1);   // the audio thread may still be reading it
    const auto* b = processor::cacheForAudio (s);
    REQUIRE (b != nullptr);
    CHECK (b != a);
    processor::tick (s);
    CHECK (processor::retiredCount (s) == 0);
    CHECK (tmp.files() == 2);   // the first file stays: another version of the project may name it
}

TEST_CASE ("stems session: cancelling drops the pass, and a new one can start at once", "[processor][stems]")
{
    const TempDir tmp;
    StemsSession s;
    prepared (s, tmp.dir);
    analyse (s, 1.0);
    const auto id = processor::cacheId (s);

    REQUIRE (processor::start (s));
    std::vector<float> l, r;
    program (l, r, samples (1.0));
    play (s, l, r, 0);
    processor::cancel (s);
    processor::tick (s);
    const auto i = processor::info (s);
    CHECK (i.phase == StemsPhase::Ready);   // the cache from before stays
    CHECK (i.lastEnd == dsp::CaptureEnd::Cancelled);
    CHECK (processor::cacheId (s) == id);
    CHECK (tmp.files() == 1);
    CHECK (processor::start (s));
}

TEST_CASE ("stems session: a pass stops at the mask budget and keeps what fit", "[processor][stems]")
{
    const TempDir tmp;
    StemsSession s;
    prepared (s, tmp.dir);
    s.budgetFrames = 20;
    REQUIRE (processor::start (s));
    std::vector<float> l, r;
    program (l, r, samples (1.0));
    play (s, l, r, 0);
    processor::tick (s);
    const auto i = processor::info (s);
    CHECK (i.phase == StemsPhase::Ready);
    CHECK (i.lastEnd == dsp::CaptureEnd::Full);
    const auto* cache = processor::cacheForAudio (s);
    REQUIRE (cache != nullptr);
    CHECK (cache->frames == 20);
}

TEST_CASE ("stems session: a re-prepare cancels a pass in progress", "[processor][stems]")
{
    const TempDir tmp;
    StemsSession s;
    prepared (s, tmp.dir);
    REQUIRE (processor::start (s));
    std::vector<float> l, r;
    program (l, r, samples (0.5));
    play (s, l, r, 0);
    processor::prepare (s, 44100.0);
    processor::tick (s);
    CHECK (processor::info (s).phase == StemsPhase::Idle);
    CHECK (processor::info (s).lastEnd == dsp::CaptureEnd::Cancelled);
    CHECK (processor::cacheForAudio (s) == nullptr);
    CHECK (processor::start (s));
}

TEST_CASE ("stems session: a saved cache is loaded by id; a missing one is reported and its id kept", "[processor][stems]")
{
    const TempDir tmp, elsewhere;
    StemsSession first;
    prepared (first, tmp.dir);
    analyse (first, 1.0);
    const auto id = processor::cacheId (first);
    const auto* original = processor::cacheForAudio (first);
    REQUIRE (original != nullptr);

    StemsSession again;
    prepared (again, tmp.dir);
    processor::restore (again, id);
    CHECK (processor::info (again).phase == StemsPhase::Loading);
    CHECK (processor::cacheId (again) == id);   // saved again before the load finished: kept
    processor::tick (again);
    CHECK (processor::info (again).phase == StemsPhase::Ready);
    const auto* loaded = processor::cacheForAudio (again);
    REQUIRE (loaded != nullptr);
    CHECK (sameCache (*loaded, *original));
    processor::restore (again, id);   // the same id again: nothing to do
    CHECK (processor::info (again).phase == StemsPhase::Ready);

    StemsSession away;
    prepared (away, elsewhere.dir);
    processor::restore (away, id);
    processor::tick (away);
    CHECK (processor::info (away).phase == StemsPhase::Missing);
    CHECK (processor::cacheForAudio (away) == nullptr);
    CHECK (processor::cacheId (away) == id);

    processor::restore (again, "");   // a state without a cache
    CHECK (processor::info (again).phase == StemsPhase::Idle);
    CHECK (processor::cacheForAudio (again) == nullptr);
    processor::tick (again);
    CHECK (processor::retiredCount (again) == 0);
}

// ------------------------------------------------------------------- in the processor

namespace {

struct JuceScope
{
    juce::ScopedJuceInitialiser_GUI init;
};

struct TestPlayHead final : juce::AudioPlayHead
{
    bool playing = false;
    std::int64_t sample = 0;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setIsPlaying (playing);
        info.setTimeInSamples (sample);
        return info;
    }
};

void setParam (Strat3Processor& p, const std::string& id, float value)
{
    auto* param = p.apvts.getParameter (id);
    REQUIRE (param != nullptr);
    param->setValueNotifyingHost (param->convertTo0to1 (value));
}

/** The host plays l/r from timeline position 0 through the plugin; the STEMS worker runs
 *  every 8 blocks. Returns the output. */
std::pair<std::vector<float>, std::vector<float>> hostPlays (Strat3Processor& p, TestPlayHead& head,
                                                             const std::vector<float>& l, const std::vector<float>& r,
                                                             int block = kBlock)
{
    auto ol = l, or_ = r;
    juce::MidiBuffer midi;
    const int n = static_cast<int> (l.size());
    head.playing = true;
    int blocks = 0;
    for (int i = 0; i < n; i += block)
    {
        const int len = std::min (block, n - i);
        head.sample = i;
        float* ch[] { ol.data() + i, or_.data() + i };
        juce::AudioBuffer<float> buf (ch, 2, len);
        p.processBlock (buf, midi);
        if (++blocks % 8 == 0)
            p.stemsTick();
    }
    // The host stops.
    head.playing = false;
    std::vector<float> zl (kBlock, 0.0f), zr = zl;
    float* ch[] { zl.data(), zr.data() };
    juce::AudioBuffer<float> buf (ch, 2, kBlock);
    p.processBlock (buf, midi);
    p.stemsTick();
    return { ol, or_ };
}

double levelDb (const std::vector<float>& x, int from, int to)
{
    double e = 0.0;
    for (int i = from; i < to; ++i)
        e += static_cast<double> (x[static_cast<size_t> (i)]) * static_cast<double> (x[static_cast<size_t> (i)]);
    return 10.0 * std::log10 (e / (to - from));
}

} // namespace

TEST_CASE ("processor stems: Analyse while the host plays, then the bass gain applies on playback", "[processor][stems]")
{
    const JuceScope juce;
    const TempDir tmp;
    auto p = std::make_unique<Strat3Processor> (false);
    p->setStemsDirectory (tmp.dir);
    TestPlayHead head;
    p->setPlayHead (&head);
    setParam (*p, "stems.on", 1.0f);
    setParam (*p, "stems.bass.gain", -12.0f);
    p->prepareToPlay (kFs, kBlock);

    std::vector<float> l, r;
    program (l, r, samples (4.0));
    REQUIRE (p->startStemsAnalysis());
    CHECK (p->stemsInfo().phase == StemsPhase::Waiting);
    const auto [al, ar] = hostPlays (*p, head, l, r);
    CHECK (p->stemsInfo().phase == StemsPhase::Ready);
    CHECK (p->stemsInfo().lastEnd == dsp::CaptureEnd::Stopped);
    // While analysing there was no cache: STEMS only delayed.
    const int lat = p->getLatencySamples();
    CHECK_THAT (levelDb (al, samples (1.0) + lat, samples (3.0) + lat) - levelDb (l, samples (1.0), samples (3.0)), WithinAbs (0.0, 0.3));

    // Played again, in host blocks larger than prepared (processed in pieces, each at its own
    // position): the 55 Hz bass (most of the energy) comes down.
    const auto [pl, pr] = hostPlays (*p, head, l, r, 3 * kBlock);
    const double change = levelDb (pl, samples (1.0) + lat, samples (3.0) + lat) - levelDb (l, samples (1.0), samples (3.0));
    INFO ("change " << change << " dB");
    CHECK (change < -6.0);
    CHECK (tmp.files() == 1);
    p->setPlayHead (nullptr);
}

TEST_CASE ("processor stems: Analyse is refused in TRACK and before prepare", "[processor][stems]")
{
    const JuceScope juce;
    const TempDir tmp;
    auto p = std::make_unique<Strat3Processor> (false);
    p->setStemsDirectory (tmp.dir);
    CHECK_FALSE (p->startStemsAnalysis());
    setParam (*p, "global.latencyMode", 0.0f);
    p->prepareToPlay (kFs, kBlock);
    CHECK_FALSE (p->startStemsAnalysis());
    setParam (*p, "global.latencyMode", 1.0f);
    p->applyPendingConfig();
    CHECK (p->startStemsAnalysis());
}

TEST_CASE ("processor stems: the cache reference is saved with the plugin state and loaded back", "[processor][stems][state]")
{
    const JuceScope juce;
    const TempDir tmp, elsewhere;
    TestPlayHead head;
    juce::MemoryBlock state;
    processor::StemsInfo made;
    {
        auto p = std::make_unique<Strat3Processor> (false);
        p->setStemsDirectory (tmp.dir);
        p->setPlayHead (&head);
        p->prepareToPlay (kFs, kBlock);
        std::vector<float> l, r;
        program (l, r, samples (2.0));
        REQUIRE (p->startStemsAnalysis());
        hostPlays (*p, head, l, r);
        made = p->stemsInfo();
        REQUIRE (made.phase == StemsPhase::Ready);
        p->getStateInformation (state);
        p->setPlayHead (nullptr);
    }
    {
        auto q = std::make_unique<Strat3Processor> (false);
        q->setStemsDirectory (tmp.dir);
        q->prepareToPlay (kFs, kBlock);
        q->setStateInformation (state.getData(), static_cast<int> (state.getSize()));
        q->stemsTick();
        const auto loaded = q->stemsInfo();
        CHECK (loaded.phase == StemsPhase::Ready);
        CHECK (loaded.cacheStartSeconds == made.cacheStartSeconds);
        CHECK (loaded.cacheEndSeconds == made.cacheEndSeconds);
    }
    {
        // On a machine without the file: reported, and the reference survives the next save.
        auto q = std::make_unique<Strat3Processor> (false);
        q->setStemsDirectory (elsewhere.dir);
        q->prepareToPlay (kFs, kBlock);
        q->setStateInformation (state.getData(), static_cast<int> (state.getSize()));
        q->stemsTick();
        CHECK (q->stemsInfo().phase == StemsPhase::Missing);
        juce::MemoryBlock resaved;
        q->getStateInformation (resaved);
        auto back = std::make_unique<Strat3Processor> (false);
        back->setStemsDirectory (tmp.dir);
        back->prepareToPlay (kFs, kBlock);
        back->setStateInformation (resaved.getData(), static_cast<int> (resaved.getSize()));
        back->stemsTick();
        CHECK (back->stemsInfo().phase == StemsPhase::Ready);
    }
}

TEST_CASE ("processor stems: the worker thread analyses alongside the audio thread", "[processor][stems][threads]")
{
    // The real threads: the STEMS worker drains and finishes while this thread is the audio
    // thread. Paced at about 5x real time, well inside the 2 s capture ring.
    const JuceScope juce;
    const TempDir tmp;
    auto p = std::make_unique<Strat3Processor> (true);
    p->setStemsDirectory (tmp.dir);
    TestPlayHead head;
    p->setPlayHead (&head);
    setParam (*p, "stems.on", 1.0f);
    p->prepareToPlay (kFs, kBlock);
    std::vector<float> l, r;
    program (l, r, samples (3.0));
    REQUIRE (p->startStemsAnalysis());

    auto playOnce = [&]
    {
        juce::MidiBuffer midi;
        auto ol = l, or_ = r;
        head.playing = true;
        for (int i = 0; i + kBlock <= static_cast<int> (ol.size()); i += kBlock)
        {
            head.sample = i;
            float* ch[] { ol.data() + i, or_.data() + i };
            juce::AudioBuffer<float> buf (ch, 2, kBlock);
            p->processBlock (buf, midi);
            std::this_thread::sleep_for (std::chrono::milliseconds (2));
        }
        head.playing = false;
        float* ch[] { ol.data(), or_.data() };
        juce::AudioBuffer<float> buf (ch, 2, kBlock);
        p->processBlock (buf, midi);
    };
    auto waitFor = [&] (StemsPhase phase)
    {
        for (int i = 0; i < 400 && p->stemsInfo().phase != phase; ++i)
            std::this_thread::sleep_for (std::chrono::milliseconds (25));
        return p->stemsInfo().phase == phase;
    };

    playOnce();
    REQUIRE (waitFor (StemsPhase::Ready));
    CHECK (p->stemsInfo().lastEnd == dsp::CaptureEnd::Stopped);
    // A second pass replaces the first while the audio thread keeps using the cache.
    REQUIRE (p->startStemsAnalysis());
    playOnce();
    REQUIRE (waitFor (StemsPhase::Ready));
    playOnce();   // takes the new cache; the worker may now free the old one
    CHECK (p->stemsLayerStatus() != dsp::StemsStatus::NoCache);
    CHECK (tmp.files() == 2);
    p->setPlayHead (nullptr);
}
