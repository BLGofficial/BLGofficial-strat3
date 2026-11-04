#pragma once
// MULTIBAND layer: one feedforward dynamics processor per band of the shared band domain.
//   detector (linked: loudest channel) -> level dB -> gain computer -> attack/release (dB)
//   -> per-band mix -> one gain for both channels of the band.
// Gain computer: log-domain soft knee (Giannoulis, Massberg and Reiss, JAES 2012).
//   Down: compression above threshold. Up: upward compression below threshold, boost capped
//   at +12 dB so a noise floor is never lifted without bound.
// Detectors: Peak (|x|), RMS (exact 10 ms moving average), Envelope (|x| through the attack/
// release follower, gain then only de-zippered). All three run every sample so switching
// detector mid-playback starts from a warm state.
// Release is program dependent: nominal for dense material (crest <= 3 dB), down to a
// quarter of it for sparse material (crest >= 12 dB), so transients do not leave holes
// while sustained material does not pump. The 3/12 dB points and the 4x range are our own
// tuning [INFERRED].
// Sidechain: internal (the bands themselves) or external (bus 2, split by SidechainSplit
// with the same crossover); the optional high-pass filters the detector only. A high-pass
// on each band's detector equals one before the split, because both are linear.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

#include "dsp/core/DelayLine.h"
#include "dsp/core/Smoother.h"
#include "dsp/core/SvfTpt.h"
#include "dsp/crossover/BandDomain.h"
#include "dsp/crossover/Lr4Crossover.h"

namespace strat3::dsp {

enum class MbDir { Down, Up };                     // param order: append only
enum class MbDetector { Peak, Rms, Envelope };     // param order: append only
enum class MbSidechain { Internal, External };     // param order: append only

inline constexpr float kMbUpwardMaxDb = 12.0f;
inline constexpr float kMbRmsWindowMs = 10.0f;
inline constexpr float kMbCrestMs = 300.0f;
inline constexpr float kMbCrestLowDb = 3.0f;       // sine: nominal release
inline constexpr float kMbCrestHighDb = 12.0f;     // and above: fastest release
inline constexpr float kMbReleaseSpeedup = 4.0f;
inline constexpr float kMbDezipMs = 1.0f;
inline constexpr float kMbMixMs = 10.0f;
inline constexpr int   kMbControlInterval = 16;
inline constexpr float kMbLevelFloorDb = -140.0f;
inline constexpr float kMbLevelCeilDb = 200.0f;    // keeps +inf input finite in the computer
inline constexpr float kMbHpQ = 0.70710678f;

struct MbBand
{
    float threshDb = 0.0f;
    float ratio = 1.0f;          // 1 = neutral
    MbDir dir = MbDir::Down;
    float kneeDb = 6.0f;
    float attackMs = 10.0f;
    float releaseMs = 120.0f;
    MbDetector detector = MbDetector::Rms;
    float mixPercent = 100.0f;

    bool operator== (const MbBand&) const = default;
};

struct MbSettings
{
    std::array<MbBand, 4> bands {};
    MbSidechain sidechain = MbSidechain::Internal;
    bool sidechainHpOn = false;
    float sidechainHpHz = 80.0f;

    bool operator== (const MbSettings&) const = default;
};

/** Static curve: gain in dB for a detector level in dB. Pure; NaN level gives 0 dB. */
inline float gainComputerDb (const MbBand& b, float levelDb) noexcept
{
    if (std::isnan (levelDb))
        return 0.0f;
    const float x = std::clamp (levelDb, kMbLevelFloorDb, kMbLevelCeilDb);
    const float slope = 1.0f / b.ratio - 1.0f;   // exactly 0 at ratio 1
    const float w = b.kneeDb;

    // Downward curve as a function of the distance above threshold.
    auto curve = [&] (float d)
    {
        if (2.0f * d < -w)
            return 0.0f;
        if (w > 0.0f && 2.0f * std::abs (d) <= w)
        {
            const float t = d + 0.5f * w;
            return slope * t * t / (2.0f * w);
        }
        return slope * d;
    };

    // Upward = the downward curve mirrored about the threshold.
    return b.dir == MbDir::Down ? curve (x - b.threshDb)
                                : std::min (-curve (b.threshDb - x), kMbUpwardMaxDb);
}

/** Effective release: nominal at crest <= 3 dB, nominal / 4 at crest >= 12 dB, log-spaced. */
inline float programReleaseMs (float releaseMs, float crestDb) noexcept
{
    if (! std::isfinite (crestDb))
        return releaseMs;
    const float w = std::clamp ((crestDb - kMbCrestLowDb) / (kMbCrestHighDb - kMbCrestLowDb), 0.0f, 1.0f);
    return releaseMs * std::exp2 (-std::log2 (kMbReleaseSpeedup) * w);
}

struct MbBandState
{
    std::vector<float> ring {};        // squared detector samples, RMS window
    std::size_t ringPos = 0;
    double ringSum = 0.0;              // double: running add/subtract must not drift
    float env = 0.0f;                  // envelope detector
    float gainDb = 0.0f;               // smoothed gain (metering + audio)
    float attackCoeff = 0.0f, releaseCoeff = 0.0f;
    float crestPeak = 0.0f, crestPower = 0.0f;
    OnePoleSmoother mix {};
    std::array<SvfState, 2> hp {};
};

struct MultibandLayer
{
    double sampleRate = 48000.0;
    MbSettings settings {};            // sanitised
    std::array<MbBandState, 4> bands {};
    SvfCoeffs hpCoeffs = passthroughCoeffs();
    float crestPeakRelease = 0.0f, crestPowerCoeff = 0.0f, dezipCoeff = 0.0f;
    int controlCounter = 0;
};

namespace mbdetail {

inline float coeffFor (float timeMs, double sampleRate) noexcept { return makeOnePole (timeMs, sampleRate, 0.0f).coeff; }

inline float finiteOr (float v, float keep) noexcept { return std::isfinite (v) ? v : keep; }

template <typename E>
E enumOr (E v, E last, E keep) noexcept
{
    const int i = static_cast<int> (v);
    return (i >= 0 && i <= static_cast<int> (last)) ? v : keep;
}

inline MbBand sanitise (const MbBand& in, const MbBand& keep) noexcept
{
    MbBand b;
    b.threshDb   = std::clamp (finiteOr (in.threshDb, keep.threshDb), -60.0f, 0.0f);
    b.ratio      = std::clamp (finiteOr (in.ratio, keep.ratio), 1.0f, 20.0f);
    b.dir        = enumOr (in.dir, MbDir::Up, keep.dir);
    b.kneeDb     = std::clamp (finiteOr (in.kneeDb, keep.kneeDb), 0.0f, 24.0f);
    b.attackMs   = std::clamp (finiteOr (in.attackMs, keep.attackMs), 0.1f, 100.0f);
    b.releaseMs  = std::clamp (finiteOr (in.releaseMs, keep.releaseMs), 5.0f, 1000.0f);
    b.detector   = enumOr (in.detector, MbDetector::Envelope, keep.detector);
    b.mixPercent = std::clamp (finiteOr (in.mixPercent, keep.mixPercent), 0.0f, 100.0f);
    return b;
}

inline float crestDb (const MbBandState& st) noexcept
{
    constexpr float tiny = 1e-14f;   // -140 dB power: silence reads as 0 dB crest
    return 10.0f * std::log10 ((st.crestPeak * st.crestPeak + tiny) / (st.crestPower + tiny));
}

inline void controlTick (MultibandLayer& mb) noexcept
{
    for (std::size_t b = 0; b < 4; ++b)
    {
        auto& st = mb.bands[b];
        st.releaseCoeff = coeffFor (programReleaseMs (mb.settings.bands[b].releaseMs, crestDb (st)), mb.sampleRate);
    }
}

/** One-pole follower for moving targets (detectors, gain). Unlike OnePoleSmoother it has no
 *  stall snap: with a target that moves every sample, "next == state" happens whenever the
 *  target passes close to the state, and snapping there biased a 300 ms power average of a
 *  sine 1.3 dB low. Only values near zero are snapped, which is all subnormal safety needs. */
inline float follow (float state, float target, float coeff) noexcept
{
    const float next = target + coeff * (state - target);
    return std::abs (next) <= kSnapEpsilon ? 0.0f : next;
}

inline float ampToDb (float a) noexcept
{
    return 20.0f * std::log10 (std::max (a, 1e-7f));   // floor = kMbLevelFloorDb
}

} // namespace mbdetail

inline float bandGainDb (const MultibandLayer& mb, int band) noexcept { return mb.bands[static_cast<std::size_t> (band)].gainDb; }
inline float bandCrestDb (const MultibandLayer& mb, int band) noexcept { return mbdetail::crestDb (mb.bands[static_cast<std::size_t> (band)]); }
inline int latencySamples (const MultibandLayer&) noexcept { return 0; }

/** Audio thread, once per block; unchanged settings are a no-op. Non-finite fields keep
 *  their previous value, the rest are clamped to the parameter ranges. */
inline void setSettings (MultibandLayer& mb, const MbSettings& in) noexcept
{
    MbSettings s;
    for (std::size_t b = 0; b < 4; ++b)
        s.bands[b] = mbdetail::sanitise (in.bands[b], mb.settings.bands[b]);
    s.sidechain = mbdetail::enumOr (in.sidechain, MbSidechain::External, mb.settings.sidechain);
    s.sidechainHpOn = in.sidechainHpOn;
    s.sidechainHpHz = std::clamp (mbdetail::finiteOr (in.sidechainHpHz, mb.settings.sidechainHpHz), 20.0f, 500.0f);
    if (s == mb.settings)
        return;

    if (s.sidechainHpOn && ! mb.settings.sidechainHpOn)
        for (auto& st : mb.bands)
            st.hp = {};   // stale state from the last time the filter ran
    mb.settings = s;
    mb.hpCoeffs = makeSvfCoeffs (SvfType::HighPass, s.sidechainHpHz, kMbHpQ, 0.0f, mb.sampleRate);
    for (std::size_t b = 0; b < 4; ++b)
        mb.bands[b].attackCoeff = mbdetail::coeffFor (s.bands[b].attackMs, mb.sampleRate);
    mbdetail::controlTick (mb);
}

/** Jump mix glides to target: after prepare / state load only. */
inline void snapToTargets (MultibandLayer& mb) noexcept
{
    for (std::size_t b = 0; b < 4; ++b)
        mb.bands[b].mix.state = mb.settings.bands[b].mixPercent * 0.01f;
}

/** prepareToPlay: allocates the RMS windows, resets all state, keeps the settings. */
inline void prepare (MultibandLayer& mb, double sampleRate)
{
    mb.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    const auto window = static_cast<std::size_t> (std::max (1L, std::lround (static_cast<double> (kMbRmsWindowMs) * 0.001 * mb.sampleRate)));
    for (std::size_t b = 0; b < 4; ++b)
    {
        auto& st = mb.bands[b];
        st = {};
        st.ring.assign (window, 0.0f);
        st.mix = makeOnePole (kMbMixMs, mb.sampleRate, mb.settings.bands[b].mixPercent * 0.01f);
        st.attackCoeff = mbdetail::coeffFor (mb.settings.bands[b].attackMs, mb.sampleRate);
    }
    mb.crestPeakRelease = static_cast<float> (std::exp (-1.0 / (static_cast<double> (kMbCrestMs) * 0.001 * mb.sampleRate)));
    mb.crestPowerCoeff = mbdetail::coeffFor (kMbCrestMs, mb.sampleRate);
    mb.dezipCoeff = mbdetail::coeffFor (kMbDezipMs, mb.sampleRate);
    mb.hpCoeffs = makeSvfCoeffs (SvfType::HighPass, mb.settings.sidechainHpHz, kMbHpQ, 0.0f, mb.sampleRate);
    mb.controlCounter = 0;
    mbdetail::controlTick (mb);
}

/** In place on the bands. `sidechain` is the external bus split into the same bands, or
 *  nullptr when the bus is disabled (then the internal signal is used). */
inline void process (MultibandLayer& mb, StereoBands& io, const StereoBands* sidechain) noexcept
{
    if (++mb.controlCounter >= kMbControlInterval)
    {
        mb.controlCounter = 0;
        mbdetail::controlTick (mb);
    }

    const bool external = mb.settings.sidechain == MbSidechain::External && sidechain != nullptr;
    const StereoBands& det = external ? *sidechain : io;

    for (std::size_t b = 0; b < 4; ++b)
    {
        auto& st = mb.bands[b];
        const auto& p = mb.settings.bands[b];

        float dl = det.l[b], dr = det.r[b];
        if (mb.settings.sidechainHpOn)
        {
            dl = processSample (st.hp[0], mb.hpCoeffs, dl);
            dr = processSample (st.hp[1], mb.hpCoeffs, dr);
        }
        // Linked: the loudest channel drives one gain for both, so the image never shifts.
        const float a = std::max (std::abs (dl), std::abs (dr));
        const float pw = std::max (dl * dl, dr * dr);

        st.crestPeak = std::max (a, st.crestPeak * mb.crestPeakRelease);
        if (st.crestPeak < kSnapEpsilon)
            st.crestPeak = 0.0f;   // the bare release multiply would decay into subnormals
        st.crestPower = mbdetail::follow (st.crestPower, pw, mb.crestPowerCoeff);

        st.ringSum += static_cast<double> (pw) - static_cast<double> (st.ring[st.ringPos]);
        st.ring[st.ringPos] = pw;
        if (++st.ringPos == st.ring.size())
            st.ringPos = 0;
        st.env = mbdetail::follow (st.env, a, a > st.env ? st.attackCoeff : st.releaseCoeff);

        float levelDb = 0.0f;
        switch (p.detector)
        {
            case MbDetector::Peak:     levelDb = mbdetail::ampToDb (a); break;
            case MbDetector::Rms:      levelDb = 10.0f * std::log10 (std::max (static_cast<float> (std::max (st.ringSum, 0.0) / static_cast<double> (st.ring.size())), 1e-14f)); break;
            case MbDetector::Envelope: levelDb = mbdetail::ampToDb (st.env); break;
        }

        const float target = gainComputerDb (p, levelDb);
        // Attack whenever the gain falls: louder input means more cut (down) or less boost
        // (up), so one rule serves both directions.
        const float coeff = p.detector == MbDetector::Envelope ? mb.dezipCoeff
                          : (target < st.gainDb ? st.attackCoeff : st.releaseCoeff);
        st.gainDb = mbdetail::follow (st.gainDb, target, coeff);

        const float g = std::exp (st.gainDb * static_cast<float> (std::numbers::ln10 / 20.0));
        const float mix = step (st.mix, p.mixPercent * 0.01f);
        const float eff = (1.0f - mix) + mix * g;   // endpoint form: mix 0 and 1 are bit-exact
        io.l[b] *= eff;
        io.r[b] *= eff;
    }
}

/** Visits every float of running state (detectors, gains, filters, RMS window). */
template <typename Fn>
void forEachFloatState (MultibandLayer& mb, Fn&& fn)
{
    for (auto& st : mb.bands)
    {
        fn (st.env);
        fn (st.gainDb);
        fn (st.crestPeak);
        fn (st.crestPower);
        fn (st.mix.state);
        for (auto& h : st.hp) { fn (h.ic1eq); fn (h.ic2eq); }
        for (const float v : st.ring)
            fn (v);
    }
}

/** Returns true if any state had to be reset (raise the UI flag; never log here). */
inline bool resetIfNotFinite (MultibandLayer& mb) noexcept
{
    bool bad = false;
    forEachFloatState (mb, [&] (float v) { bad |= ! std::isfinite (v); });
    for (const auto& st : mb.bands)
        bad |= ! std::isfinite (st.ringSum);
    if (! bad)
        return false;

    for (auto& st : mb.bands)
    {
        std::fill (st.ring.begin(), st.ring.end(), 0.0f);
        st.ringSum = 0.0;
        st.ringPos = 0;
        st.env = st.gainDb = st.crestPeak = st.crestPower = 0.0f;
        st.hp = {};
        if (! std::isfinite (st.mix.state))
            st.mix.state = 0.0f;
    }
    return true;
}

inline void flushDenormals (MultibandLayer& mb) noexcept
{
    for (auto& st : mb.bands)
        for (auto& h : st.hp)
            flushDenormals (h);
}

// ------------------------------------------------------------------------- sidechain split

/** External sidechain -> 4 bands at the band domain's crossover points. Always IIR: the
 *  detector needs magnitude, not phase. In FIR mode the main bands are late by the FIR
 *  latency, so the sidechain is delayed by the same amount to keep gain and audio aligned. */
struct SidechainSplit
{
    std::array<DelayLine, 2> delay {};
    std::array<Lr4Split4, 2> split {};
};

inline void prepare (SidechainSplit& sc, CrossoverPoints points, double sampleRate, int maxDelaySamples)
{
    for (auto& d : sc.delay)
    {
        prepare (d, static_cast<std::size_t> (std::max (0, maxDelaySamples)));
        setDelay (d, std::size_t { 0 });
    }
    for (auto& s : sc.split)
        s = makeLr4Split4 (points, sampleRate);
}

/** Set to latencySamples(BandDomain) when the latency mode changes (transport stopped). */
inline void setDelay (SidechainSplit& sc, int samples) noexcept
{
    for (auto& d : sc.delay)
        setDelay (d, static_cast<std::size_t> (std::max (0, samples)));
}

inline void setCrossovers (SidechainSplit& sc, CrossoverPoints points) noexcept
{
    for (auto& s : sc.split)
        setCrossovers (s, points);
}

inline StereoBands process (SidechainSplit& sc, float l, float r) noexcept
{
    StereoBands out;
    out.l = process (sc.split[0], process (sc.delay[0], l));
    out.r = process (sc.split[1], process (sc.delay[1], r));
    return out;
}

inline bool resetIfNotFinite (SidechainSplit& sc) noexcept
{
    bool bad = false;
    for (auto& s : sc.split)
        bad |= resetIfNotFinite (s);
    for (auto& d : sc.delay)
        bad |= sanitise (d);
    return bad;
}

inline void flushDenormals (SidechainSplit& sc) noexcept
{
    for (auto& s : sc.split)
        flushDenormals (s);
}

} // namespace strat3::dsp
