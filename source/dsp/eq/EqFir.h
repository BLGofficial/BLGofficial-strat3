#pragma once
// FIR EQ for MASTER mode: the static bands as one kernel per M/S channel, with eq.phase
// blending from minimum phase (0) to linear phase (100).
// Static bands only: dynamic nodes stay in the IIR EQ, because redesigning a 4k-tap FIR per
// control tick is not affordable.
// Always runs in M/S: kernel_M = stereo + mid bands, kernel_S = stereo + side bands. Equal to
// L/R processing for stereo-only settings, because a stereo band is linear in both domains.
// Latency is the same at every phase blend (taps/2 + partition): minimum phase is delayed by
// the linear-phase centre, so eq.phase can be automated without a host latency change. The
// cost is that minimum phase saves no latency here; TRACK/MIX use the zero-latency IIR EQ.
// Runtime redesigns go through FirSwap (background design, warm-up, 50 ms crossfade).
#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <numbers>
#include <utility>
#include <vector>

#include "dsp/core/FirSwap.h"
#include "dsp/crossover/FirDesign.h"
#include "dsp/crossover/UniformConvolver.h"
#include "dsp/eq/EqResponse.h"

namespace strat3::dsp {

inline constexpr float kMinimumPhasePercent = 0.0f;
inline constexpr float kLinearPhasePercent = 100.0f;
inline constexpr float kFirEqFadeMs = 50.0f;
/** Magnitude floor for the cepstrum: log(0) at an HP/LP null would be -inf. -120 dB is far
 *  below anything audible and keeps the cepstrum short (less time aliasing). */
inline constexpr double kKernelFloorDb = -120.0;

namespace eqfirdetail {

/** Grid of at least 4x the kernel length: limits time aliasing of the sampled response. */
inline int gridOrder (std::size_t numTaps) noexcept
{
    int order = 1;
    while ((std::size_t { 1 } << order) < numTaps * 4)
        ++order;
    return order;
}

inline double flooredDb (const EqSettings& s, double hz, double sampleRate, EqChannel channel) noexcept
{
    const double db = settingsMagnitudeDb (s, hz, sampleRate, channel, false);
    return std::isfinite (db) ? std::max (db, kKernelFloorDb) : kKernelFloorDb;
}

inline float sanitisePhase (float phasePercent) noexcept
{
    return std::isfinite (phasePercent) ? std::clamp (phasePercent, kMinimumPhasePercent, kLinearPhasePercent)
                                        : kLinearPhasePercent;
}

} // namespace eqfirdetail

/** Frequency-sampling design: sample the analytic magnitude on a fine grid, inverse FFT to a
 *  zero-phase response, window to `numTaps`, centre. Allocates; never on the audio thread.
 *  Built symmetric by construction, so the kernel is exactly linear phase. */
inline std::vector<float> designLinearPhaseEq (const EqSettings& settings, double sampleRate,
                                               std::size_t numTaps, EqChannel channel, double beta = 6.0)
{
    if (numTaps % 2 == 0)
        ++numTaps;
    const std::size_t centre = numTaps / 2;

    const int order = eqfirdetail::gridOrder (numTaps);
    const int size = 1 << order;

    std::vector<float> data (static_cast<std::size_t> (2 * size), 0.0f);
    auto* bins = reinterpret_cast<std::complex<float>*> (data.data());
    for (int k = 0; k <= size / 2; ++k)
    {
        const double hz = static_cast<double> (k) * sampleRate / size;
        const double db = settingsMagnitudeDb (settings, hz, sampleRate, channel, false);
        bins[k] = { static_cast<float> (std::pow (10.0, db / 20.0)), 0.0f };
    }

    juce::dsp::FFT fft (order);
    fft.performRealOnlyInverseTransform (data.data());   // zero-phase IR, centred at index 0

    std::vector<float> h (numTaps);
    const double i0Beta = firdetail::besselI0 (beta);
    for (std::size_t i = 0; i <= centre; ++i)
    {
        const double r = static_cast<double> (i) / static_cast<double> (centre);
        const double w = firdetail::besselI0 (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0Beta;
        // Average the two circular halves so tiny FFT asymmetries cannot break symmetry.
        const double zp = 0.5 * (static_cast<double> (data[i]) + static_cast<double> (data[(static_cast<std::size_t> (size) - i) % static_cast<std::size_t> (size)]));
        h[centre + i] = h[centre - i] = static_cast<float> (zp * w);
    }
    return h;
}

/** Kernel for eq.phase = `phasePercent` (0 minimum, 100 linear). Allocates; never on the
 *  audio thread.
 *  Minimum phase comes from the real cepstrum (folded to causal), whose imaginary part after
 *  the forward FFT is the already-unwrapped minimum phase, so no phase unwrapping is needed.
 *  Blend: phase = a * phi_min - w * centre, with a = 1 - phase/100; the magnitude is never
 *  touched, so every blend has the same magnitude as the linear design. */
inline std::vector<float> designEqKernel (const EqSettings& settings, double sampleRate, std::size_t numTaps,
                                          EqChannel channel, float phasePercent, double beta = 6.0)
{
    const float phase = eqfirdetail::sanitisePhase (phasePercent);
    if (phase >= kLinearPhasePercent)
        return designLinearPhaseEq (settings, sampleRate, numTaps, channel, beta);   // exactly symmetric

    if (numTaps % 2 == 0)
        ++numTaps;
    const std::size_t centre = numTaps / 2;
    const int order = eqfirdetail::gridOrder (numTaps);
    const auto size = static_cast<std::size_t> (1) << order;
    const auto half = size / 2;

    using C = std::complex<float>;
    std::vector<C> a (size), b (size);
    std::vector<double> magDb (half + 1);
    juce::dsp::FFT fft (order);

    // 1. Log magnitude (natural log), even around the circle -> real cepstrum.
    for (std::size_t k = 0; k <= half; ++k)
    {
        magDb[k] = eqfirdetail::flooredDb (settings, static_cast<double> (k) * sampleRate / static_cast<double> (size), sampleRate, channel);
        a[k] = { static_cast<float> (magDb[k] * std::numbers::ln10 / 20.0), 0.0f };
        if (k > 0 && k < half)
            a[size - k] = a[k];
    }
    fft.perform (a.data(), b.data(), true);

    // 2. Fold to a causal cepstrum: its spectrum is log|H| + j * phi_min.
    std::fill (a.begin(), a.end(), C {});
    a[0] = { b[0].real(), 0.0f };
    for (std::size_t n = 1; n < half; ++n)
        a[n] = { 2.0f * b[n].real(), 0.0f };
    a[half] = { b[half].real(), 0.0f };
    fft.perform (a.data(), b.data(), false);

    // 3. Exact magnitude, blended phase, delayed to the linear-phase centre (Hermitian).
    const double alpha = 1.0 - static_cast<double> (phase) / 100.0;
    for (std::size_t k = 0; k <= half; ++k)
    {
        const double w = 2.0 * std::numbers::pi * static_cast<double> (k) / static_cast<double> (size);
        const double phi = alpha * static_cast<double> (b[k].imag()) - w * static_cast<double> (centre);
        const auto h = std::polar (std::pow (10.0, magDb[k] / 20.0), phi);
        a[k] = { static_cast<float> (h.real()), static_cast<float> (h.imag()) };
        if (k > 0 && k < half)
            a[size - k] = std::conj (a[k]);
    }
    a[0] = { a[0].real(), 0.0f };
    a[half] = { a[half].real(), 0.0f };
    fft.perform (a.data(), b.data(), true);

    // 4. Window around the centre: the minimum-phase part starts at the window peak, so the
    //    same symmetric Kaiser treats its tail like the linear kernel's right half.
    std::vector<float> h (numTaps);
    const double i0Beta = firdetail::besselI0 (beta);
    for (std::size_t n = 0; n < numTaps; ++n)
    {
        const double r = (static_cast<double> (n) - static_cast<double> (centre)) / static_cast<double> (centre);
        const double w = firdetail::besselI0 (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0Beta;
        h[n] = static_cast<float> (static_cast<double> (b[n].real()) * w);
    }
    return h;
}

// ------------------------------------------------------------------------- runtime EQ

struct FirEqDesign
{
    EqSettings settings {};
    float phasePercent = kLinearPhasePercent;
};

struct FirEqSlot
{
    std::array<UniformConvolver, 2> conv {};   // [mid, side]
    FirEqDesign design {};
};

/** Not copyable or movable (atomic in FirSwap): construct in place, then prepare(). */
struct FirEq
{
    FirSwap<FirEqSlot> swap {};
    double sampleRate = 48000.0;
    int partition = 256;
    int latency = 0;
    bool prepared = false;
};

namespace eqfirdetail {

struct Kernels
{
    std::vector<float> mid, side;
};

inline Kernels designKernels (const FirEqDesign& d, double sampleRate)
{
    const auto taps = firCrossoverTaps (sampleRate);   // same latency as the FIR crossover
    return { designEqKernel (d.settings, sampleRate, taps, EqChannel::Mid, d.phasePercent),
             designEqKernel (d.settings, sampleRate, taps, EqChannel::Side, d.phasePercent) };
}

inline void loadSlot (FirEqSlot& slot, const Kernels& k, const FirEqDesign& d, int partition)
{
    prepare (slot.conv[0], partition, { k.mid });
    prepare (slot.conv[1], partition, { k.side });
    slot.design = d;
}

} // namespace eqfirdetail

/** prepareToPlay only (allocates both slots; no concurrent designStandby). */
inline void prepare (FirEq& eq, const EqSettings& settings, float phasePercent, double sampleRate, int partition = 256)
{
    eq.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    eq.partition = partition;
    const FirEqDesign d { settings, eqfirdetail::sanitisePhase (phasePercent) };
    const auto kernels = eqfirdetail::designKernels (d, eq.sampleRate);
    const int fade = static_cast<int> (std::lround (static_cast<double> (kFirEqFadeMs) * 0.001 * eq.sampleRate));
    swapPrepare (eq.swap, fade, [&] (FirEqSlot& slot) { eqfirdetail::loadSlot (slot, kernels, d, partition); });
    eq.latency = static_cast<int> (firCrossoverTaps (eq.sampleRate) / 2) + partition;
    eq.prepared = true;
}

inline int latencySamples (const FirEq& eq) noexcept { return eq.prepared ? eq.latency : 0; }

/** Background thread only (allocates, FFTs). False if unprepared or a swap is pending or
 *  running; the caller keeps its latest request and retries. */
inline bool designStandby (FirEq& eq, const EqSettings& settings, float phasePercent)
{
    if (! eq.prepared)
        return false;
    const FirEqDesign d { settings, eqfirdetail::sanitisePhase (phasePercent) };
    return swapDesign (eq.swap, [&] (FirEqSlot& slot)
    {
        eqfirdetail::loadSlot (slot, eqfirdetail::designKernels (d, eq.sampleRate), d, eq.partition);
    });
}

/** Audio thread, once per block: picks up a finished design. */
inline void beginBlock (FirEq& eq) noexcept
{
    if (eq.prepared)
        swapBegin (eq.swap, static_cast<int> (firCrossoverTaps (eq.sampleRate)) - 1 + eq.partition);
}

inline bool isSwapping (const FirEq& eq) noexcept { return isSwapping (eq.swap); }

/** Design the audible slot was built from (audio thread, or after the swap completes). */
inline const FirEqDesign& activeDesign (const FirEq& eq) noexcept { return activeSlot (eq.swap).design; }

inline void process (FirEq& eq, float& l, float& r) noexcept
{
    if (! eq.prepared)
        return;
    const float m = (l + r) * 0.5f, s = (l - r) * 0.5f;
    auto run = [&] (FirEqSlot& slot)
    {
        std::pair<float, float> out { 0.0f, 0.0f };
        process (slot.conv[0], m, &out.first);
        process (slot.conv[1], s, &out.second);
        return out;
    };
    auto mix = [] (std::pair<float, float> a, std::pair<float, float> b, float t)
    {
        return std::pair { a.first + t * (b.first - a.first), a.second + t * (b.second - a.second) };
    };
    const auto [om, os] = swapProcess (eq.swap, run, mix);
    l = om + os;
    r = om - os;
}

} // namespace strat3::dsp
