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

/**
 * Separates a captured stretch into stem masks and fingerprints it.
 * @param l, r the stretch as played, `n` samples per channel
 * @param sampleRate the rate it was captured at; the layer only uses the cache at that rate
 * @param startSample the timeline position of l[0]
 */
inline MaskCache separate (const float* l, const float* r, int n, double sampleRate, std::int64_t startSample)
{
    using namespace sepdetail;
    MaskCache c;
    c.sampleRate = sampleRate;
    c.startSample = startSample;
    c.frames = maskFramesFor (n);
    c.prints = fingerprints (l, r, n, sampleRate);
    c.masks.assign (static_cast<std::size_t> (c.frames) * kStems * kMaskBins, 0);
    if (c.frames == 0)
        return c;

    juce::dsp::FFT fft (juce::roundToInt (std::log2 (kMaskFft)));
    std::vector<float> window (kMaskFft), workL (2 * kMaskFft), workR (2 * kMaskFft);
    for (int i = 0; i < kMaskFft; ++i)
        window[static_cast<std::size_t> (i)] = static_cast<float> (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * i / kMaskFft));

    std::array<float, kMaskBins> bass {}, band {};
    for (std::size_t k = 0; k < kMaskBins; ++k)
    {
        const auto hz = static_cast<float> (static_cast<double> (k) * sampleRate / kMaskFft);
        bass[k] = bassWeight (hz);
        band[k] = vocalBand (hz);
    }

    // The time median needs 8 frames either side: a ring of 17 analysed frames, so memory stays
    // small however long the stretch. Frame f is finished once frame f + 8 is analysed.
    constexpr int half = kHpssSpan / 2;
    std::vector<Frame> ring (kHpssSpan, Frame { std::vector<float> (kMaskBins), std::vector<float> (kMaskBins) });
    std::array<float, kHpssSpan> values {};

    for (int g = 0; g < c.frames + half; ++g)
    {
        if (g < c.frames)
            analyse (fft, window, workL, workR, l, r, g * kMaskHop, ring[static_cast<std::size_t> (g % kHpssSpan)]);
        const int f = g - half;
        if (f < 0)
            continue;

        // Near the edges the medians use the frames and bins there are.
        const int f0 = std::max (0, f - half), f1 = std::min (c.frames - 1, f + half);
        const auto& now = ring[static_cast<std::size_t> (f % kHpssSpan)];
        for (int k = 0; k < kMaskBins; ++k)
        {
            const auto bin = static_cast<std::size_t> (k);
            auto* bytes = c.masks.data() + maskIndex (f, 0, k);
            const float s = now.magnitude[bin];
            if (s < kSilentMagnitude)
            {
                store (bytes, kMaskBins, 0.0f, 0.0f, 0.0f);
                continue;
            }

            int count = 0;
            for (int t = f0; t <= f1; ++t)
                values[static_cast<std::size_t> (count++)] = ring[static_cast<std::size_t> (t % kHpssSpan)].magnitude[bin];
            const float h = median (values.data(), count);

            count = 0;
            for (int j = std::max (0, k - half); j <= std::min (kMaskBins - 1, k + half); ++j)
                values[static_cast<std::size_t> (count++)] = now.magnitude[static_cast<std::size_t> (j)];
            const float p = median (values.data(), count);

            const float hh = h * h, pp = p * p;
            const float mh = hh + pp > 0.0f ? hh / (hh + pp) : 0.5f;   // both medians 0: undecided
            const float centre = now.centre[bin];
            const float voc = (1.0f - bass[bin]) * mh * band[bin] * centre * centre;
            store (bytes, kMaskBins, voc, bass[bin] * mh, 1.0f - mh);
        }
    }
    return c;
}

} // namespace strat3::dsp
