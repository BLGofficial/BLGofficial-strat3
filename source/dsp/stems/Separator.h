#pragma once
// STEMS separator, model-free: it splits a captured stretch of the mix into 4 masks per bin and
// frame from what DSP can tell without a trained model. It stands in for a model behind the
// same interface (a MaskCache out), so a model can replace it without touching the layer.
//   STFT: periodic Hann, 1024 / hop 512, of L and R; S = sqrt(|L|^2 + |R|^2)
//   harmonic / percussive (median filtering, D. Fitzgerald, DAFx 2010): H = median of S over
//     17 frames, P = median of S over 17 bins; mh = H^2 / (H^2 + P^2), mp = 1 - mh
//   centre: c = max(0, 2 Re(L conj R) / (|L|^2 + |R|^2)), 1 only where L and R are the same
//     in level and phase, 0 for one side only or out of phase
//   bass weight b: 1 below 120 Hz, falling to 0 at 250 Hz
//   vocal band v: 0 below 150 Hz, 1 from 250 Hz to 5 kHz, falling to 0 at 10 kHz
//   drums = mp; bass = b mh; vocals = (1 - b) mh v c^2; other = what is left
// So vocals are sustained, centred, mid-band content: a lead vocal, but also a centred synth
// or piano. That is the known limit of a model-free split [INFERRED weights and bands].
// Runs on a background thread (22b); allocates. Cost is a few seconds per 4 minutes.
#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <memory>
#include <vector>

#include "dsp/stems/MaskCache.h"

namespace strat3::dsp {

inline constexpr int kHpssSpan = 17;                 // frames (time) and bins (frequency)
inline constexpr float kBassFullHz = 120.0f, kBassEndHz = 250.0f;
inline constexpr float kVocalStartHz = 150.0f, kVocalFullHz = 250.0f;
inline constexpr float kVocalFullEndHz = 5000.0f, kVocalEndHz = 10000.0f;
// A bin this quiet is no instrument: it goes to "other" (-120 dB under a full-scale sine).
inline constexpr float kSilentMagnitude = 1.0e-6f * static_cast<float> (kMaskFft) / 4.0f;

namespace sepdetail {

/** 0 at or below lo, 1 at or above hi, linear between. */
inline float ramp (float x, float lo, float hi) noexcept { return std::clamp ((x - lo) / (hi - lo), 0.0f, 1.0f); }

inline float bassWeight (float hz) noexcept { return 1.0f - ramp (hz, kBassFullHz, kBassEndHz); }

inline float vocalBand (float hz) noexcept { return ramp (hz, kVocalStartHz, kVocalFullHz) * (1.0f - ramp (hz, kVocalFullEndHz, kVocalEndHz)); }

inline float median (float* values, int count) noexcept
{
    auto* mid = values + count / 2;
    std::nth_element (values, mid, values + count);
    return *mid;
}

struct Frame
{
    std::vector<float> magnitude;   // S per bin
    std::vector<float> centre;      // c per bin
};

/** One analysis frame of both channels at `first` (samples into the stretch). */
inline void analyse (juce::dsp::FFT& fft, const std::vector<float>& window, std::vector<float>& workL, std::vector<float>& workR,
                     const float* l, const float* r, int first, Frame& out)
{
    std::fill (workL.begin(), workL.end(), 0.0f);
    std::fill (workR.begin(), workR.end(), 0.0f);
    for (int n = 0; n < kMaskFft; ++n)
    {
        // A non-finite sample would make every median meaningless: it counts as silence.
        const float a = l[first + n], b = r[first + n];
        workL[static_cast<std::size_t> (n)] = (std::isfinite (a) ? a : 0.0f) * window[static_cast<std::size_t> (n)];
        workR[static_cast<std::size_t> (n)] = (std::isfinite (b) ? b : 0.0f) * window[static_cast<std::size_t> (n)];
    }
    fft.performRealOnlyForwardTransform (workL.data(), true);
    fft.performRealOnlyForwardTransform (workR.data(), true);
    const auto* xl = reinterpret_cast<const std::complex<float>*> (workL.data());
    const auto* xr = reinterpret_cast<const std::complex<float>*> (workR.data());
    for (std::size_t k = 0; k < kMaskBins; ++k)
    {
        const float pl = std::norm (xl[k]), pr = std::norm (xr[k]);
        out.magnitude[k] = std::sqrt (pl + pr);
        const float sum = pl + pr;
        out.centre[k] = sum > 0.0f ? std::max (0.0f, 2.0f * (xl[k] * std::conj (xr[k])).real() / sum) : 0.0f;
    }
}

/** Quantises 3 masks to bytes and gives "other" the rest. Rounding the running sum, not each
 *  mask, keeps the 4 bytes at exactly 255 whatever the inputs (each byte is within 1 of its mask). */
inline void store (std::uint8_t* bytes, std::size_t binStride, float voc, float bass, float drm) noexcept
{
    const auto upTo = [] (float sum) { return static_cast<int> (std::lround (std::clamp (sum, 0.0f, 1.0f) * kMaskFull)); };
    voc = std::clamp (voc, 0.0f, 1.0f);
    bass = std::clamp (bass, 0.0f, 1.0f);
    drm = std::clamp (drm, 0.0f, 1.0f);
    const int a = upTo (voc), b = upTo (voc + bass), c = upTo (voc + bass + drm);
    bytes[kStemVoc * binStride] = static_cast<std::uint8_t> (a);
    bytes[kStemBass * binStride] = static_cast<std::uint8_t> (b - a);
    bytes[kStemDrm * binStride] = static_cast<std::uint8_t> (c - b);
    bytes[kStemOth * binStride] = static_cast<std::uint8_t> (kMaskFull - c);
}

} // namespace sepdetail

/** The separator fed as the audio arrives (22b: during the analysis pass), so no copy of the
 *  whole stretch is ever held. A frame is finished once the 8 frames after it are in. */
struct StreamSeparator
{
    double sampleRate = 48000.0;
    int maxFrames = 0;
    std::int64_t maxSamples = 0, pushed = 0;
    std::unique_ptr<juce::dsp::FFT> fft {};
    std::vector<float> window {}, workL {}, workR {};
    std::array<float, kMaskBins> bass {}, band {};
    std::vector<sepdetail::Frame> ring {};
    std::vector<float> pendL {}, pendR {};   // the window being filled
    int pendFill = 0;
    int analysed = 0, finished = 0;
    PrintMeter meter {};
    std::vector<BlockPrint> prints {};
    std::vector<std::uint8_t> masks {};
};

namespace sepdetail {

inline constexpr int kHalfSpan = kHpssSpan / 2;

/** Masks for frame f; the time median uses frames [f - 8, last]. */
inline void finishFrame (StreamSeparator& s, int f, int last) noexcept
{
    std::array<float, kHpssSpan> values {};
    const int f0 = std::max (0, f - kHalfSpan);   // near the edges: the frames there are
    const auto& now = s.ring[static_cast<std::size_t> (f % kHpssSpan)];
    for (int k = 0; k < kMaskBins; ++k)
    {
        const auto bin = static_cast<std::size_t> (k);
        auto* bytes = s.masks.data() + maskIndex (f, 0, k);
        const float mag = now.magnitude[bin];
        if (mag < kSilentMagnitude)
        {
            store (bytes, kMaskBins, 0.0f, 0.0f, 0.0f);
            continue;
        }

        int count = 0;
        for (int t = f0; t <= last; ++t)
            values[static_cast<std::size_t> (count++)] = s.ring[static_cast<std::size_t> (t % kHpssSpan)].magnitude[bin];
        const float h = median (values.data(), count);

        count = 0;
        for (int j = std::max (0, k - kHalfSpan); j <= std::min (kMaskBins - 1, k + kHalfSpan); ++j)
            values[static_cast<std::size_t> (count++)] = now.magnitude[static_cast<std::size_t> (j)];
        const float p = median (values.data(), count);

        const float hh = h * h, pp = p * p;
        const float mh = hh + pp > 0.0f ? hh / (hh + pp) : 0.5f;   // both medians 0: undecided
        const float centre = now.centre[bin];
        const float voc = (1.0f - s.bass[bin]) * mh * s.band[bin] * centre * centre;
        store (bytes, kMaskBins, voc, s.bass[bin] * mh, 1.0f - mh);
    }
}

} // namespace sepdetail

/** Allocates for up to `maxFrames` frames (the masks are reserved now, so they never move). */
inline void begin (StreamSeparator& s, double sampleRate, int maxFrames)
{
    s.sampleRate = sampleRate;
    s.maxFrames = std::max (0, maxFrames);
    s.maxSamples = s.maxFrames == 0 ? 0 : static_cast<std::int64_t> (s.maxFrames - 1) * kMaskHop + kMaskFft;
    s.pushed = 0;
    s.fft = std::make_unique<juce::dsp::FFT> (juce::roundToInt (std::log2 (kMaskFft)));
    s.window.assign (kMaskFft, 0.0f);
    for (int i = 0; i < kMaskFft; ++i)
        s.window[static_cast<std::size_t> (i)] = static_cast<float> (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * i / kMaskFft));
    s.workL.assign (2 * kMaskFft, 0.0f);
    s.workR.assign (2 * kMaskFft, 0.0f);
    for (std::size_t k = 0; k < kMaskBins; ++k)
    {
        const auto hz = static_cast<float> (static_cast<double> (k) * sampleRate / kMaskFft);
        s.bass[k] = sepdetail::bassWeight (hz);
        s.band[k] = sepdetail::vocalBand (hz);
    }
    s.ring.assign (kHpssSpan, sepdetail::Frame { std::vector<float> (kMaskBins), std::vector<float> (kMaskBins) });
    s.pendL.assign (kMaskFft, 0.0f);
    s.pendR.assign (kMaskFft, 0.0f);
    s.pendFill = 0;
    s.analysed = s.finished = 0;
    prepare (s.meter, sampleRate);
    s.prints.clear();
    s.prints.reserve (static_cast<std::size_t> (s.maxSamples / kPrintBlock));
    s.masks.clear();
    s.masks.reserve (static_cast<std::size_t> (s.maxFrames) * kStems * kMaskBins);
}

/** Adds samples; returns how many it took (fewer once the frame budget is reached). */
inline int push (StreamSeparator& s, const float* l, const float* r, int n)
{
    const auto room = std::max<std::int64_t> (0, s.maxSamples - s.pushed);
    const int take = static_cast<int> (std::min<std::int64_t> (std::max (0, n), room));
    int i = 0;
    while (i < take)
    {
        const int len = std::min (take - i, kMaskFft - s.pendFill);
        for (int j = 0; j < len; ++j)
        {
            const auto at = static_cast<std::size_t> (s.pendFill + j);
            s.pendL[at] = l[i + j];
            s.pendR[at] = r[i + j];
            if (push (s.meter, 0.5f * (l[i + j] + r[i + j])))
                s.prints.push_back (lastPrint (s.meter));
        }
        s.pendFill += len;
        i += len;
        if (s.pendFill < kMaskFft)
            break;

        const int g = s.analysed++;
        sepdetail::analyse (*s.fft, s.window, s.workL, s.workR, s.pendL.data(), s.pendR.data(), 0,
                            s.ring[static_cast<std::size_t> (g % kHpssSpan)]);
        s.masks.resize (static_cast<std::size_t> (s.analysed) * kStems * kMaskBins);
        if (g >= sepdetail::kHalfSpan)
            sepdetail::finishFrame (s, s.finished++, g);
        // Slide the window by one hop.
        std::copy (s.pendL.begin() + kMaskHop, s.pendL.end(), s.pendL.begin());
        std::copy (s.pendR.begin() + kMaskHop, s.pendR.end(), s.pendR.begin());
        s.pendFill = kMaskFft - kMaskHop;
    }
    s.pushed += take;
    return take;
}

inline bool isFull (const StreamSeparator& s) noexcept { return s.pushed >= s.maxSamples; }
inline std::int64_t pushedSamples (const StreamSeparator& s) noexcept { return s.pushed; }

/** Finishes the frames still waiting for their medians and hands the cache over; the
 *  separator is spent. `startSample` is the timeline position of the first sample pushed. */
inline MaskCache finish (StreamSeparator& s, std::int64_t startSample)
{
    while (s.finished < s.analysed)
        sepdetail::finishFrame (s, s.finished++, s.analysed - 1);
    MaskCache c;
    c.sampleRate = s.sampleRate;
    c.startSample = startSample;
    c.frames = s.analysed;
    c.masks = std::move (s.masks);
    c.prints = std::move (s.prints);
    s = {};
    return c;
}

/**
 * Separates a captured stretch into stem masks and fingerprints it, in one pass.
 * @param l, r the stretch as played, `n` samples per channel
 * @param sampleRate the rate it was captured at; the layer only uses the cache at that rate
 * @param startSample the timeline position of l[0]
 */
inline MaskCache separate (const float* l, const float* r, int n, double sampleRate, std::int64_t startSample)
{
    StreamSeparator s;
    begin (s, sampleRate, maskFramesFor (n) + 1);   // + 1: room for every sample, prints included
    push (s, l, r, n);
    return finish (s, startSample);
}

} // namespace strat3::dsp
