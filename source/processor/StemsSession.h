#pragma once
// STEMS analysis and its cache (module 22b), between the processor and the engine.
//
// Analyse: the message thread arms the capture; the audio thread fills it with the STEMS
// input while the host plays (engine::process); a worker thread drains it into the streaming
// separator, so no whole-song buffer is ever held. The pass ends when the host stops, seeks
// or loops, when the mask budget is reached (kStemsBudgetFrames), or when it is cancelled.
// Whatever was analysed before the end becomes the new cache; a cancelled pass is dropped.
//
// Hand-over: the worker publishes a cache through an atomic pointer; the audio thread takes
// it at the start of a block (cacheForAudio) and acknowledges it. A replaced cache is freed
// on the worker only once the audio thread has acknowledged its successor, so the audio
// thread never reads freed masks and never frees anything.
//
// Saved: the cache goes to a compressed file in a per-user folder, named by a random id that
// the plugin state keeps (the masks run to ~46 MB per 4 minutes at 48 kHz: too big for a
// host's project file). A state whose file this machine lacks loads no cache and says so; the
// id is kept, so the project finds the file again where it exists. Files are never deleted
// here: another saved version of the project may still name them.
//
// Threads: prepare() on the message thread with the audio suspended; start(), cancel(),
// restore(), info() and cacheId() on any thread but audio; tick() on one worker;
// cacheForAudio() on the audio thread only.
#include <juce_core/juce_core.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "dsp/stems/Capture.h"
#include "dsp/stems/MaskCache.h"
#include "dsp/stems/Separator.h"

namespace strat3::processor {

/** The capture ring holds this much: the worker's slack before a pass overflows. */
inline constexpr double kStemsRingSeconds = 2.0;
/** Masks for one pass [INFERRED]: 128 MB, about 11 minutes at 48 kHz, 5.5 at 96 kHz. */
inline constexpr std::size_t kStemsMaskBudgetBytes = std::size_t { 128 } << 20;
inline constexpr int kStemsBudgetFrames = static_cast<int> (kStemsMaskBudgetBytes / (dsp::kStems * dsp::kMaskBins));
inline constexpr const char* kStemsFileExtension = ".s3stems";

enum class StemsPhase
{
    Idle,        // no cache, nothing running
    Waiting,     // armed: the pass starts when the host plays
    Analysing,   // capturing and separating
    Loading,     // a saved cache is being read
    Ready,       // a cache is handed to the audio thread
    Missing      // the project names a cache this machine does not have (or cannot read)
};

struct StemsInfo
{
    StemsPhase phase = StemsPhase::Idle;
    double analysedSeconds = 0.0;                            // the running pass, so far
    double budgetSeconds = 0.0;                              // the longest pass at this rate
    dsp::CaptureEnd lastEnd = dsp::CaptureEnd::None;         // why the last pass ended
    double cacheStartSeconds = 0.0, cacheEndSeconds = 0.0;   // the timeline stretch it covers
    double cacheSampleRate = 0.0;                            // the layer bypasses at another rate
    bool saveFailed = false;   // the cache works now but is not saved with the project
    std::uint32_t revision = 0;   // changes whenever any of the above does (polling)
};

struct StemsSession
{
    // ---- audio thread <-> worker (lock-free)
    dsp::StemsCapture ring {};
    std::atomic<const dsp::MaskCache*> next { nullptr }, acked { nullptr };

    // ---- everything below: under `lock`, never taken on the audio thread
    mutable std::mutex lock;
    double sampleRate = 0.0;                 // 0 until prepared
    int budgetFrames = kStemsBudgetFrames;   // tests make it small
    dsp::StreamSeparator separator {};
    bool running = false;                    // a pass is armed or capturing
    std::vector<float> drainL {}, drainR {};
    std::unique_ptr<dsp::MaskCache> current {};                 // what `next` points to
    std::vector<std::unique_ptr<dsp::MaskCache>> retired {};    // until the audio thread moves on
    std::string id {};                       // the cache the state names ("" none)
    bool loadPending = false, missing = false, saveFailed = false;
    dsp::CaptureEnd lastEnd = dsp::CaptureEnd::None;
    juce::File dir {};
    std::uint32_t revision = 0;
};

/** The per-user folder for cache files (created on the first save). */
juce::File defaultStemsDirectory();

/** True for an id this plugin makes (a lowercase dashed UUID). The state is untrusted input:
 *  nothing else is ever turned into a path. */
bool isValidStemsId (const std::string& id);

/** Writes the cache compressed, through a temporary file, so a crash never leaves half a
 *  file under the real name. False on any I/O error. */
bool writeCacheFile (const dsp::MaskCache& cache, const juce::File& file);

/** Reads a cache written by writeCacheFile. Refuses (nullopt) a foreign, truncated or
 *  damaged file, including one whose masks do not sum to 255 per bin and frame. */
std::optional<dsp::MaskCache> readCacheFile (const juce::File& file);

/** Message thread, audio suspended: the rate the engine now runs at. Sizes the capture and
 *  cancels a pass in progress (its timeline and rate no longer hold). Keeps the cache. */
void prepare (StemsSession& s, double sampleRate);

/** Arms a new pass. False while one is armed or running, or before prepare(). */
bool start (StemsSession& s);

/** Ends and drops a pass at once; a new one can start right after. No-op when none runs. */
void cancel (StemsSession& s);

/** Audio thread, once per block: the cache to hand to the engine (null for none). */
const dsp::MaskCache* cacheForAudio (StemsSession& s) noexcept;

/** Worker: drains the capture into the separator, finishes an ended pass into the new cache
 *  and saves it, loads a cache the state named, and frees what the audio thread let go of. */
void tick (StemsSession& s);

StemsInfo info (const StemsSession& s);

/** The id to save in the plugin state ("" for none). */
std::string cacheId (const StemsSession& s);

/** From a loaded plugin state: the cache it names ("" for none, an invalid id is none).
 *  Drops the current cache and cancels a pass unless it is the same id; tick() loads it. */
void restore (StemsSession& s, const std::string& id);

void setDirectory (StemsSession& s, const juce::File& dir);

/** Replaced caches still waiting for the audio thread to move on (tests). */
std::size_t retiredCount (const StemsSession& s);

} // namespace strat3::processor
