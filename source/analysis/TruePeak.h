#pragma once
// True-peak detector: 4x polyphase interpolation, the ITU-R BS.1770-4 Annex 2 method.
// Own filter, not the Annex's example coefficient table [INFERRED]: Kaiser-windowed sinc,
// 16 taps per phase (the Annex example uses 12), each phase normalised to unity DC gain.
// Phase 0 is the sample itself (a sinc is zero at the other integers), so it needs no taps.
// push() returns the peak of the interval [n - D, n - D + 1), D = kTruePeakLookahead: the
// interpolator needs D samples of future. Shared by the CEILING layer and the loudness meter.
// Sampling a sine at 4x can miss its crest by up to ~0.5 dB near 20 kHz at 48 kHz: the
// method's own limit, which every BS.1770 meter shares.
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

#include "dsp/crossover/FirDesign.h"   // firdetail::besselI0

namespace strat3::analysis {

inline constexpr int kTruePeakOversample = 4;
inline constexpr int kTruePeakLookahead = 8;                 // support half-width, samples
inline constexpr int kTruePeakTaps = 2 * kTruePeakLookahead; // per interpolated phase
inline constexpr double kTruePeakBeta = 7.0;

struct TruePeakTaps
{
    // phase p = 1..3 (index p - 1): value at t0 + p/4 = sum_j tap[j] * x[t0 - S + 1 + j]
    std::array<std::array<float, kTruePeakTaps>, kTruePeakOversample - 1> tap {};
};

inline TruePeakTaps makeTruePeakTaps() noexcept
{
    TruePeakTaps t;
    const double s = kTruePeakLookahead;
    const double i0b = dsp::firdetail::besselI0 (kTruePeakBeta);
    for (int p = 1; p < kTruePeakOversample; ++p)
    {
        const double frac = static_cast<double> (p) / kTruePeakOversample;
        std::array<double, kTruePeakTaps> h {};
        double sum = 0.0;
        for (int j = 0; j < kTruePeakTaps; ++j)
        {
            const double m = -s + 1.0 + j;          // sample offset from t0
            const double tau = frac - m;            // distance from the sample to the point
            const double x = std::numbers::pi * tau;
            const double r = tau / (s + 1.0);       // window wider than the support: no zero end taps
            const double w = dsp::firdetail::besselI0 (kTruePeakBeta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0b;
            h[static_cast<size_t> (j)] = std::sin (x) / x * w;
            sum += h[static_cast<size_t> (j)];
        }
        for (int j = 0; j < kTruePeakTaps; ++j)
            t.tap[static_cast<size_t> (p - 1)][static_cast<size_t> (j)] = static_cast<float> (h[static_cast<size_t> (j)] / sum);
    }
    return t;
}

/** Built at load time: never on the audio thread. */
inline const TruePeakTaps kTruePeakFilter = makeTruePeakTaps();

struct TruePeakDetector
{
    std::array<float, 2 * kTruePeakTaps> ring {};   // mirrored: one contiguous read per phase
    int pos = 0;
};

inline void reset (TruePeakDetector& d) noexcept { d = {}; }

/** Pushes x[n]; returns the true peak of [n - D, n - D + 1). */
inline float push (TruePeakDetector& d, float x) noexcept
{
    d.pos = (d.pos + 1) % kTruePeakTaps;
    d.ring[static_cast<size_t> (d.pos)] = x;
    d.ring[static_cast<size_t> (d.pos + kTruePeakTaps)] = x;
    const float* h0 = d.ring.data() + d.pos + 1;   // oldest .. newest = x[n-15] .. x[n]
    // t0 = n - D: oldest tap x[t0 - S + 1] = x[n - 2S + 1]: exactly the ring.
    float peak = std::abs (h0[kTruePeakLookahead - 1]);   // x[t0]
    for (const auto& ph : kTruePeakFilter.tap)
    {
        float acc = 0.0f;
        for (int j = 0; j < kTruePeakTaps; ++j)
            acc += ph[static_cast<size_t> (j)] * h0[j];
        peak = std::max (peak, std::abs (acc));
    }
    return peak;
}

inline bool isFinite (const TruePeakDetector& d) noexcept
{
    for (float v : d.ring)
        if (! std::isfinite (v))
            return false;
    return true;
}

} // namespace strat3::analysis
