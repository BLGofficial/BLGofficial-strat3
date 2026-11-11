#include "processor/StemsSession.h"

#include <algorithm>
#include <cmath>

namespace strat3::processor {
namespace {

// "S3ST" as a little-endian int, then a version: a file is either exactly this format or refused.
constexpr int kMagic = 0x54533353;
constexpr int kVersion = 1;
constexpr int kPrintBytes = dsp::kPrintBands + 1;
// A file may come from a build with a larger budget; anything beyond this is not a cache.
constexpr int kMaxFileFrames = 8 * kStemsBudgetFrames;
constexpr int kDrainChunk = 4096;

std::int64_t maskBytes (int frames) noexcept
{
    return static_cast<std::int64_t> (frames) * dsp::kStems * dsp::kMaskBins;
}

/** The longest pass the budget allows, in samples. */
std::int64_t budgetSamples (int frames) noexcept
{
    return frames <= 0 ? 0 : static_cast<std::int64_t> (frames - 1) * dsp::kMaskHop + dsp::kMaskFft;
}

juce::File fileFor (const juce::File& dir, const std::string& id)
{
    return dir.getChildFile (juce::String (id) + kStemsFileExtension);
}

bool masksSumToFull (const dsp::MaskCache& c) noexcept
{
    for (int f = 0; f < c.frames; ++f)
        for (int k = 0; k < dsp::kMaskBins; ++k)
        {
            int sum = 0;
            for (int st = 0; st < dsp::kStems; ++st)
                sum += c.masks[dsp::maskIndex (f, st, k)];
            if (sum != dsp::kMaskFull)
                return false;
        }
    return true;
}

// ---- everything below runs with s.lock held

void bump (StemsSession& s) noexcept { ++s.revision; }

/** Hands `cache` (null for none) to the audio thread; the one it replaces waits in retired. */
void publish (StemsSession& s, std::unique_ptr<dsp::MaskCache> cache)
{
    if (s.current != nullptr)
        s.retired.push_back (std::move (s.current));
    s.current = std::move (cache);
    s.next.store (s.current.get(), std::memory_order_release);
    bump (s);
}

void freeRetired (StemsSession& s)
{
    // The audio thread acknowledges what it loaded before it uses it: once it has the current
    // pointer, no block can still be reading a retired cache.
    if (! s.retired.empty() && s.acked.load (std::memory_order_acquire) == s.next.load (std::memory_order_relaxed))
        s.retired.clear();
}

/** Ends the pass now, discarding what it captured. The writer can no longer commit once the
 *  capture has ended, so the drain empties it for good and the reset cannot fail. */
void dropPass (StemsSession& s)
{
    if (! s.running)
        return;
    dsp::end (s.ring, dsp::CaptureEnd::Cancelled);
    while (dsp::read (s.ring, s.drainL.data(), s.drainR.data(), kDrainChunk) > 0) {}
    dsp::reset (s.ring);
    s.separator = {};
    s.running = false;
    s.lastEnd = dsp::CaptureEnd::Cancelled;
    bump (s);
}

/** Moves what the audio thread captured into the separator. True once the pass has ended
 *  and everything it captured has been taken. */
bool drain (StemsSession& s)
{
    for (;;)
    {
        const int n = dsp::read (s.ring, s.drainL.data(), s.drainR.data(), kDrainChunk);
        if (n <= 0)
            break;
        if (dsp::push (s.separator, s.drainL.data(), s.drainR.data(), n) < n || dsp::isFull (s.separator))
            dsp::end (s.ring, dsp::CaptureEnd::Full);   // the rest is read and dropped
    }
    return dsp::state (s.ring) == dsp::CaptureState::Ended && dsp::available (s.ring) == 0;
}

} // namespace

juce::File defaultStemsDirectory()
{
    // Per user and outside any project; on Windows the local (not roaming) profile, since
    // the files are large and only this machine's analysis.
#if JUCE_MAC
    const auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Application Support");
#elif JUCE_WINDOWS
    const auto base = juce::File::getSpecialLocation (juce::File::windowsLocalAppData);
#else
    const auto xdg = juce::SystemStats::getEnvironmentVariable ("XDG_DATA_HOME", {});
    const auto base = juce::File::isAbsolutePath (xdg) ? juce::File (xdg)
                                                        : juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile (".local/share");
#endif
    return base.getChildFile ("STRAT3").getChildFile ("Stems");
}

bool isValidStemsId (const std::string& id)
{
    // 8-4-4-4-12 lowercase hex, as juce::Uuid::toDashedString() writes it.
    if (id.size() != 36)
        return false;
    for (std::size_t i = 0; i < id.size(); ++i)
    {
        const char c = id[i];
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? c != '-' : ! ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    }
    return true;
}

bool writeCacheFile (const dsp::MaskCache& c, const juce::File& file)
{
    if (! file.getParentDirectory().createDirectory().wasOk())
        return false;
    juce::TemporaryFile temp (file);
    {
        juce::FileOutputStream raw (temp.getFile());
        if (! raw.openedOk())
            return false;
        juce::GZIPCompressorOutputStream out (raw, 6);
        bool ok = out.writeInt (kMagic) && out.writeInt (kVersion) && out.writeDouble (c.sampleRate)
               && out.writeInt64 (c.startSample) && out.writeInt (c.frames)
               && out.writeInt (static_cast<int> (c.prints.size()))
               && out.write (c.masks.data(), c.masks.size());
        for (const auto& p : c.prints)
        {
            std::array<std::int8_t, kPrintBytes> bytes {};
            std::copy (p.shapeDb.begin(), p.shapeDb.end(), bytes.begin());
            bytes.back() = p.levelDb;
            ok = ok && out.write (bytes.data(), bytes.size());
        }
        out.flush();
        raw.flush();
        if (! ok || raw.getStatus().failed())
            return false;
    }
    return temp.overwriteTargetFileWithTemporary();
}

std::optional<dsp::MaskCache> readCacheFile (const juce::File& file)
{
    juce::FileInputStream raw (file);
    if (! raw.openedOk())
        return std::nullopt;
    juce::GZIPDecompressorInputStream in (raw);
    if (in.readInt() != kMagic || in.readInt() != kVersion)
        return std::nullopt;
    dsp::MaskCache c;
    c.sampleRate = in.readDouble();
    c.startSample = in.readInt64();
    c.frames = in.readInt();
    const int prints = in.readInt();
    // Bounds before allocating: a damaged header must not ask for gigabytes.
    if (! std::isfinite (c.sampleRate) || c.sampleRate < 8000.0 || c.sampleRate > 768000.0
        || c.startSample < 0 || c.frames < 1 || c.frames > kMaxFileFrames
        || prints < 0 || prints > c.frames + 2)
        return std::nullopt;
    c.masks.resize (static_cast<std::size_t> (maskBytes (c.frames)));
    if (in.read (c.masks.data(), static_cast<int> (c.masks.size())) != static_cast<int> (c.masks.size()))
        return std::nullopt;
    c.prints.resize (static_cast<std::size_t> (prints));
    for (auto& p : c.prints)
    {
        std::array<std::int8_t, kPrintBytes> bytes {};
        if (in.read (bytes.data(), kPrintBytes) != kPrintBytes)
            return std::nullopt;
        std::copy (bytes.begin(), bytes.begin() + dsp::kPrintBands, p.shapeDb.begin());
        p.levelDb = bytes.back();
    }
    // Nothing may follow.
    char extra = 0;
    if (in.read (&extra, 1) != 0)
        return std::nullopt;
    if (! masksSumToFull (c))
        return std::nullopt;
    return c;
}

void prepare (StemsSession& s, double sampleRate)
{
    const std::lock_guard g (s.lock);
    dropPass (s);
    s.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    dsp::prepare (s.ring, static_cast<int> (std::ceil (kStemsRingSeconds * s.sampleRate)));
    s.drainL.assign (kDrainChunk, 0.0f);
    s.drainR.assign (kDrainChunk, 0.0f);
    if (s.dir == juce::File())
        s.dir = defaultStemsDirectory();
}

bool start (StemsSession& s)
{
    const std::lock_guard g (s.lock);
    if (s.running || s.sampleRate <= 0.0 || ! dsp::arm (s.ring))
        return false;
    dsp::begin (s.separator, s.sampleRate, s.budgetFrames);
    s.running = true;
    s.lastEnd = dsp::CaptureEnd::None;
    bump (s);
    return true;
}

void cancel (StemsSession& s)
{
    const std::lock_guard g (s.lock);
    dropPass (s);
}

const dsp::MaskCache* cacheForAudio (StemsSession& s) noexcept
{
    const auto* c = s.next.load (std::memory_order_acquire);
    s.acked.store (c, std::memory_order_release);
    return c;
}

void tick (StemsSession& s)
{
    // ---- a pass: drain, and once it has ended, finish it into the new cache
    const dsp::MaskCache* toSave = nullptr;
    std::string savedId;
    juce::File saveTo;
    {
        const std::lock_guard g (s.lock);
        freeRetired (s);
        if (s.running && drain (s))
        {
            s.lastEnd = dsp::endReason (s.ring);
            const auto startAt = dsp::startSample (s.ring);
            auto cache = std::make_unique<dsp::MaskCache> (dsp::finish (s.separator, startAt));
            s.separator = {};
            dsp::reset (s.ring);
            s.running = false;
            bump (s);
            if (cache->frames > 0)
            {
                // Played first, saved after: the cache works while the file is written.
                s.id = juce::Uuid().toDashedString().toStdString();
                s.loadPending = s.missing = s.saveFailed = false;
                savedId = s.id;
                saveTo = fileFor (s.dir, s.id);
                toSave = cache.get();
                publish (s, std::move (cache));
            }
        }
    }
    if (toSave != nullptr)
    {
        // Outside the lock. toSave stays alive even if a restore() drops it meanwhile: only
        // this thread frees caches (freeRetired), and it is busy here.
        const bool saved = writeCacheFile (*toSave, saveTo);
        const std::lock_guard g (s.lock);
        if (! saved && s.id == savedId)
        {
            s.saveFailed = true;
            s.id.clear();   // the state must not name a file that does not exist
            bump (s);
        }
    }

    // ---- a cache the plugin state named: read outside the lock (it can take a second)
    std::string loadId;
    juce::File loadFrom;
    {
        const std::lock_guard g (s.lock);
        if (s.loadPending && s.dir != juce::File())
        {
            s.loadPending = false;
            loadId = s.id;
            loadFrom = fileFor (s.dir, s.id);
        }
    }
    if (! loadId.empty())
    {
        auto loaded = readCacheFile (loadFrom);
        const std::lock_guard g (s.lock);
        if (s.id == loadId && ! s.loadPending)   // not replaced by another restore or a pass
        {
            if (loaded.has_value())
                publish (s, std::make_unique<dsp::MaskCache> (std::move (*loaded)));
            else
                s.missing = true;
            bump (s);
        }
    }
}

StemsInfo info (const StemsSession& s)
{
    const std::lock_guard g (s.lock);
    StemsInfo i;
    if (s.running)
        i.phase = dsp::state (s.ring) == dsp::CaptureState::Armed ? StemsPhase::Waiting : StemsPhase::Analysing;
    else if (s.loadPending)
        i.phase = StemsPhase::Loading;
    else if (s.current != nullptr)
        i.phase = StemsPhase::Ready;
    else if (s.missing)
        i.phase = StemsPhase::Missing;
    if (s.sampleRate > 0.0)
    {
        i.analysedSeconds = s.running ? static_cast<double> (dsp::pushedSamples (s.separator)) / s.sampleRate : 0.0;
        i.budgetSeconds = static_cast<double> (budgetSamples (s.budgetFrames)) / s.sampleRate;
    }
    i.lastEnd = s.lastEnd;
    if (s.current != nullptr && s.current->sampleRate > 0.0)
    {
        i.cacheSampleRate = s.current->sampleRate;
        i.cacheStartSeconds = static_cast<double> (s.current->startSample) / s.current->sampleRate;
        i.cacheEndSeconds = static_cast<double> (dsp::cacheEnd (*s.current)) / s.current->sampleRate;
    }
    i.saveFailed = s.saveFailed;
    i.revision = s.revision;
    return i;
}

std::string cacheId (const StemsSession& s)
{
    const std::lock_guard g (s.lock);
    return s.id;
}

void restore (StemsSession& s, const std::string& id)
{
    const std::lock_guard g (s.lock);
    const auto wanted = isValidStemsId (id) ? id : std::string {};
    // The same cache again: nothing to do, unless it was missing (the file may be back).
    if (wanted == s.id && (s.current != nullptr || s.loadPending || wanted.empty()))
        return;
    dropPass (s);
    publish (s, nullptr);
    s.id = wanted;
    s.loadPending = ! wanted.empty();
    s.missing = s.saveFailed = false;
    bump (s);
}

void setDirectory (StemsSession& s, const juce::File& dir)
{
    const std::lock_guard g (s.lock);
    s.dir = dir;
}

std::size_t retiredCount (const StemsSession& s)
{
    const std::lock_guard g (s.lock);
    return s.retired.size();
}

} // namespace strat3::processor
