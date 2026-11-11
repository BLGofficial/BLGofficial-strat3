#pragma once
// STEMS layer (full band, first in the chain): applies the cached stem masks at playback.
//   STFT 1024 / hop 256, sqrt-Hann in and out (the product is Hann, which sums to 2 at hop
//   N/4, hence the 0.5). Latency: exactly 1024 samples, cache or not, so the host's delay
//   compensation never changes.
//   per bin: g = sum over stems of mask * gain * tilt, tilt = tiltDbPerOct * octaves from
//   1 kHz, capped at +-12 dB; then g' = 1 + e (g - 1), e the engage amount
//   e moves toward 1 by hop / 50 ms per frame while the layer knows where it is, toward 0
//   otherwise; at e = 0 the FFT is skipped (same output, the OLA alone)
// Where it is: the host's timeline position of the newest sample in the frame. Masks come
// from the cache frame whose start is nearest this frame's start. The layer bypasses while
// the host is stopped, gives no position, plays outside the cached stretch, or runs at a
// sample rate other than the cache's (the bins would be other frequencies).
// Stale: every 512 samples on the cache's grid the input's fingerprint is compared with the
// cache's; 3 misses in a row mark the cache stale (bypass), 3 matches in a row clear it. A
// position jump restarts the count [INFERRED counts].
// The masks change per frame without smoothing: the 4-fold overlap already crossfades them.
#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <memory>
#include <vector>

#include "dsp/stems/Fingerprint.h"
#include "dsp/stems/MaskCache.h"

namespace strat3::dsp {

inline constexpr int kStemsFft = 1024;
inline constexpr int kStemsHop = 256;
inline constexpr float kStemsFadeMs = 50.0f;
inline constexpr float kStemsMaxGainDb = 12.0f;
inline constexpr float kStemsMaxTilt = 6.0f;
inline constexpr float kStemsTiltLimitDb = 12.0f;
inline constexpr float kStemsTiltPivotHz = 1000.0f;
inline constexpr int kStaleBlocks = 3;

// The playback bins must be the mask bins.
static_assert (kStemsFft == kMaskFft);

struct StemsSettings
{
    std::array<float, kStems> gainDb {};         // -12..12
    std::array<float, kStems> tiltDbPerOct {};   // -6..6
    bool operator== (const StemsSettings&) const = default;
};

/** What the host says about the block: from its play head. */
struct Transport
{
    bool playing = false;
    bool hasPosition = false;
    std::int64_t sample = 0;   // timeline position of the block's first sample
};

enum class StemsStatus { NoCache, Waiting, Active, Stale };

struct StemsLayer
{
    double sampleRate = 48000.0;
    StemsSettings settings {};
    const MaskCache* cache = nullptr;

    // per stem and bin: gain * tilt, from the settings
    std::array<std::array<float, kMaskBins>, kStems> stemGain {};
    std::array<float, kMaskBins> octaves {};   // log2 (bin Hz / 1 kHz)

    std::unique_ptr<juce::dsp::FFT> fft {};
    std::vector<float> window {};                        // sqrt-Hann
    std::array<std::vector<float>, 2> input {}, overlap {}, ready {};
    std::vector<float> work {};                          // 2 * N for JUCE's in-place real FFT
    std::vector<float> gains {};                         // per bin, this frame
    int hopFill = 0;

    float engage = 0.0f, engageStep = 0.0f;
    int lastFrame = -1;
    StemsStatus state = StemsStatus::NoCache;

    // fingerprint check
    PrintMeter meter {};
    bool positionKnown = false, aligned = false, stale = false;
    std::int64_t expected = 0;
    int matchRun = 0, missRun = 0;
};

namespace stemsdetail {

inline float dbToGain (float db) noexcept { return std::exp (db * 0.11512925f); }   // ln 10 / 20
inline float finiteOr (float v, float keep) noexcept { return std::isfinite (v) ? v : keep; }

inline bool cacheUsable (const StemsLayer& s) noexcept
{
    return s.cache != nullptr && s.cache->frames > 0 && s.cache->sampleRate == s.sampleRate;
}

inline void updateGains (StemsLayer& s) noexcept
{
    for (std::size_t st = 0; st < kStems; ++st)
    {
        const float gain = dbToGain (s.settings.gainDb[st]);
        const float tilt = s.settings.tiltDbPerOct[st];
        for (std::size_t k = 0; k < kMaskBins; ++k)
            s.stemGain[st][k] = gain * dbToGain (std::clamp (tilt * s.octaves[k], -kStemsTiltLimitDb, kStemsTiltLimitDb));
    }
}

/** Notes position jumps: the fingerprint blocks must line up with the cache's again. */
inline void trackTransport (StemsLayer& s, const Transport& t, int n) noexcept
{
    if (! (t.playing && t.hasPosition))
    {
        s.positionKnown = false;
        return;
    }
    if (! s.positionKnown || t.sample != s.expected)
    {
        s.aligned = false;
        s.matchRun = s.missRun = 0;
    }
    s.positionKnown = true;
    s.expected = t.sample + n;
}

inline void judge (StemsLayer& s, bool match) noexcept
{
    if (match)
    {
        s.missRun = 0;
        if (++s.matchRun >= kStaleBlocks)
            s.stale = false;
    }
    else
    {
        s.matchRun = 0;
        if (++s.missRun >= kStaleBlocks)
            s.stale = true;
    }
}

/** Fingerprints input samples at timeline positions [first, first + n). */
inline void meterInput (StemsLayer& s, const float* l, const float* r, int n, std::int64_t first) noexcept
{
    const auto& c = *s.cache;
    for (int i = 0; i < n; ++i)
    {
        const std::int64_t pos = first + i;
        if (! s.aligned && (pos - c.startSample) % kPrintBlock == 0)
        {
            restartBlock (s.meter);
            s.aligned = true;
        }
        if (push (s.meter, 0.5f * (l[i] + r[i])) && s.aligned)
        {
            const std::int64_t block = (pos + 1 - kPrintBlock - c.startSample) / kPrintBlock;
            if (block >= 0 && block < static_cast<std::int64_t> (c.prints.size()))
                judge (s, matches (lastPrint (s.meter), c.prints[static_cast<std::size_t> (block)]));
        }
    }
}

/** One STFT frame; `newest` is the timeline position of its last sample. */
inline void processFrame (StemsLayer& s, const Transport& t, std::int64_t newest) noexcept
{
    constexpr int N = kStemsFft, H = kStemsHop;
    const bool usable = cacheUsable (s);
    const bool located = t.playing && t.hasPosition;
    const bool inRange = usable && located && newest >= s.cache->startSample && newest < cacheEnd (*s.cache);

    if (inRange)
    {
        const std::int64_t start = std::clamp (newest - (N - 1), s.cache->startSample,
                                               s.cache->startSample + static_cast<std::int64_t> (s.cache->frames - 1) * kMaskHop);
        s.lastFrame = cacheFrameAt (*s.cache, start);
    }
    s.state = ! usable ? StemsStatus::NoCache : s.stale ? StemsStatus::Stale : inRange ? StemsStatus::Active : StemsStatus::Waiting;
    const bool active = s.state == StemsStatus::Active;
    // engage > 0 only after an Active frame set lastFrame; prepare and setCache zero both, so a
    // fade-out always has the masks it started with.
    s.engage = active ? std::min (1.0f, s.engage + s.engageStep) : std::max (0.0f, s.engage - s.engageStep);

    if (s.engage > 0.0f)
    {
        for (std::size_t k = 0; k < kMaskBins; ++k)
        {
            float g = 0.0f;
            for (int st = 0; st < kStems; ++st)
                g += maskAt (*s.cache, s.lastFrame, st, static_cast<int> (k)) * s.stemGain[static_cast<std::size_t> (st)][k];
            s.gains[k] = 1.0f + s.engage * (g - 1.0f);
        }
    }

    for (std::size_t ch = 0; ch < 2; ++ch)
    {
        auto& in = s.input[ch];
        auto& acc = s.overlap[ch];
        if (s.engage > 0.0f)
        {
            std::fill (s.work.begin(), s.work.end(), 0.0f);
            for (std::size_t n = 0; n < static_cast<std::size_t> (N); ++n)
                s.work[n] = in[n] * s.window[n];
            s.fft->performRealOnlyForwardTransform (s.work.data(), true);
            auto* bins = reinterpret_cast<std::complex<float>*> (s.work.data());
            for (std::size_t k = 0; k < kMaskBins; ++k)
                bins[k] *= s.gains[k];
            s.fft->performRealOnlyInverseTransform (s.work.data());
            for (std::size_t n = 0; n < static_cast<std::size_t> (N); ++n)
                acc[n] += s.work[n] * s.window[n];
        }
        else
        {
            for (std::size_t n = 0; n < static_cast<std::size_t> (N); ++n)
                acc[n] += in[n] * (s.window[n] * s.window[n]);
        }

        for (std::size_t n = 0; n < static_cast<std::size_t> (H); ++n)
            s.ready[ch][n] = 0.5f * acc[n];
        std::copy (acc.begin() + H, acc.end(), acc.begin());
        std::fill (acc.end() - H, acc.end(), 0.0f);
        std::copy (in.begin() + H, in.end(), in.begin());
    }
}

} // namespace stemsdetail

/** prepareToPlay: allocates and resets all state; keeps the settings and the cache. */
inline void prepare (StemsLayer& s, double sampleRate)
{
    constexpr int N = kStemsFft;
    s.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    s.fft = std::make_unique<juce::dsp::FFT> (juce::roundToInt (std::log2 (N)));
    s.window.assign (N, 0.0f);
    for (int n = 0; n < N; ++n)   // periodic, so the overlap sums exactly
        s.window[static_cast<std::size_t> (n)] = static_cast<float> (std::sqrt (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * n / N)));
    for (std::size_t ch = 0; ch < 2; ++ch)
    {
        s.input[ch].assign (N, 0.0f);
        s.overlap[ch].assign (N, 0.0f);
        s.ready[ch].assign (kStemsHop, 0.0f);
    }
    s.work.assign (2 * N, 0.0f);
    s.gains.assign (kMaskBins, 1.0f);
    s.hopFill = 0;

    // DC has no octave: it takes half a bin's.
    for (std::size_t k = 0; k < kMaskBins; ++k)
        s.octaves[k] = static_cast<float> (std::log2 (std::max (0.5, static_cast<double> (k)) * s.sampleRate / N / static_cast<double> (kStemsTiltPivotHz)));
    stemsdetail::updateGains (s);

    s.engageStep = static_cast<float> (kStemsHop / (static_cast<double> (kStemsFadeMs) * 0.001 * s.sampleRate));
    s.engage = 0.0f;
    s.lastFrame = -1;
    prepare (s.meter, s.sampleRate);
    s.positionKnown = s.aligned = s.stale = false;
    s.matchRun = s.missRun = 0;
    s.state = stemsdetail::cacheUsable (s) ? StemsStatus::Waiting : StemsStatus::NoCache;
}

/** Sets the cache the layer reads; null for none. The layer does not own it: the caller keeps
 *  it alive until another is set. A new cache starts bypassed and fades in. */
inline void setCache (StemsLayer& s, const MaskCache* cache) noexcept
{
    if (cache == s.cache)
        return;
    s.cache = cache;
    s.engage = 0.0f;
    s.lastFrame = -1;
    s.stale = s.aligned = false;
    s.matchRun = s.missRun = 0;
    reset (s.meter);
    s.state = stemsdetail::cacheUsable (s) ? StemsStatus::Waiting : StemsStatus::NoCache;
}

/** Audio thread, once per block. Clamped; a non-finite field keeps its previous value. */
inline void setSettings (StemsLayer& s, const StemsSettings& in) noexcept
{
    using stemsdetail::finiteOr;
    StemsSettings next = s.settings;
    for (std::size_t st = 0; st < kStems; ++st)
    {
        next.gainDb[st] = std::clamp (finiteOr (in.gainDb[st], next.gainDb[st]), -kStemsMaxGainDb, kStemsMaxGainDb);
        next.tiltDbPerOct[st] = std::clamp (finiteOr (in.tiltDbPerOct[st], next.tiltDbPerOct[st]), -kStemsMaxTilt, kStemsMaxTilt);
    }
    if (next == s.settings)
        return;
    s.settings = next;
    stemsdetail::updateGains (s);
}

/** Processes a block in place. The output is the input 1024 samples late. */
inline void process (StemsLayer& s, float* l, float* r, int n, const Transport& t) noexcept
{
    stemsdetail::trackTransport (s, t, n);
    const bool metering = s.positionKnown && stemsdetail::cacheUsable (s);
    const auto tail = static_cast<std::size_t> (kStemsFft - kStemsHop);

    int i = 0;
    while (i < n)
    {
        const int len = std::min (n - i, kStemsHop - s.hopFill);
        if (metering)
            stemsdetail::meterInput (s, l + i, r + i, len, t.sample + i);
        for (int j = 0; j < len; ++j)
        {
            const auto slot = static_cast<std::size_t> (s.hopFill + j);
            const auto io = static_cast<std::size_t> (i + j);
            s.input[0][tail + slot] = l[io];
            s.input[1][tail + slot] = r[io];
            l[io] = s.ready[0][slot];
            r[io] = s.ready[1][slot];
        }
        s.hopFill += len;
        i += len;
        if (s.hopFill == kStemsHop)
        {
            s.hopFill = 0;
            stemsdetail::processFrame (s, t, t.sample + i - 1);
        }
    }
}

inline int latencySamples (const StemsLayer&) noexcept { return kStemsFft; }
inline StemsStatus status (const StemsLayer& s) noexcept { return s.state; }
inline float engaged (const StemsLayer& s) noexcept { return s.engage; }

/** Returns true if any state had to be cleared (raise the UI flag; never log here). The STFT
 *  has no feedback, but a NaN in a frame spreads to every bin of 4 frames: clear it now. */
inline bool resetIfNotFinite (StemsLayer& s) noexcept
{
    const auto finite = [] (const std::vector<float>& v) { return std::all_of (v.begin(), v.end(), [] (float x) { return std::isfinite (x); }); };
    bool ok = std::isfinite (s.engage);
    for (std::size_t ch = 0; ch < 2; ++ch)
        ok = ok && finite (s.input[ch]) && finite (s.overlap[ch]) && finite (s.ready[ch]);
    if (ok)
        return false;
    for (std::size_t ch = 0; ch < 2; ++ch)
    {
        std::fill (s.input[ch].begin(), s.input[ch].end(), 0.0f);
        std::fill (s.overlap[ch].begin(), s.overlap[ch].end(), 0.0f);
        std::fill (s.ready[ch].begin(), s.ready[ch].end(), 0.0f);
    }
    s.engage = 0.0f;
    return true;
}

} // namespace strat3::dsp
