#pragma once
// Impact analyser: the data behind the IMPACT MAP ("who is doing what, where").
//
// Audio thread (ImpactCapture): every layer boundary is a tap (N layers -> N + 1 taps). One SPSC
// ring holds a frame of mid/side for every tap per sample. A host block is reserved whole or
// dropped whole, so the taps can never fall out of step (one ring per tap could drop in one
// and not another, and the before/after comparison would then compare different audio).
//
// Analysis thread (ImpactAnalyser, ~30 Hz): for layer i, the window from tap i ("before") ends
// latency_i samples earlier than the window from tap i + 1 ("after"), so both hold the same
// audio. Hann window, real FFT (2048 at 44.1/48 kHz, scaled with the rate to keep ~23 Hz bins),
// bin powers spread into 1/6-octave bands by fractional overlap (bands narrower than a bin
// take their share of it). Band energies are smoothed per frame (one-pole, 250 ms, own tuning)
// before the ratios:
//   gainDb  = 10 log10 (E_after / E_before),  E = mid + side energy
//   widthDb = 10 log10 (S/M after) - 10 log10 (S/M before)
// clamped to +-24 dB, |x| < 1e-4 dB shown as exactly 0, published by a triple buffer.
// prepare()/setLatencies() run on the analysis thread (or while it is stopped).
#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>
#include <vector>

namespace strat3::analysis {

inline constexpr int kImpactBands = 61;              // 1 kHz * 2^(k/6), k = -34 .. 26: 19.7 Hz .. 20.2 kHz
inline constexpr int kMaxImpactLayers = 12;
inline constexpr int kImpactCaptureFrames = 32768;   // ~0.7 s at 48 kHz; the consumer drains at 30 Hz
inline constexpr double kImpactFrameHz = 30.0;
inline constexpr double kImpactSmoothingMs = 250.0;
inline constexpr float kImpactClampDb = 24.0f;
inline constexpr double kImpactMaxLatencyMs = 100.0;
inline constexpr double kImpactEnergyFloor = 1e-12;   // -120 dB: below this a band is "silent"
inline constexpr double kImpactWidthFloor = 1e-6;     // side/mid floor relative to the band energy

inline double impactBandCentreHz (int band) noexcept { return 1000.0 * std::pow (2.0, static_cast<double> (band - 34) / 6.0); }

/** 2048 up to 48 kHz, doubled per doubling of the rate. */
inline int impactFftSize (double sampleRate) noexcept
{
    int n = 2048;
    while (n < 8192 && sampleRate > 48000.0 * (n / 2048) + 1.0)
        n *= 2;
    return n;
}

// ------------------------------------------------------------------- triple buffer

/** Single writer, single reader. The writer never waits; the reader gets the newest whole slot. */
template <typename T>
struct TripleBuffer
{
    std::array<T, 3> slot {};
    std::atomic<std::uint8_t> middle { 1 };   // index | kNew
    std::uint8_t backIdx = 0, frontIdx = 2;
    static constexpr std::uint8_t kNew = 4;
};

template <typename T>
T& back (TripleBuffer<T>& tb) noexcept { return tb.slot[tb.backIdx]; }

template <typename T>
void publish (TripleBuffer<T>& tb) noexcept
{
    tb.backIdx = static_cast<std::uint8_t> (tb.middle.exchange (static_cast<std::uint8_t> (tb.backIdx | TripleBuffer<T>::kNew), std::memory_order_acq_rel) & 3);
}

/** Copies the newest slot into `out` if one was published since the last fetch. */
template <typename T>
bool fetch (TripleBuffer<T>& tb, T& out) noexcept
{
    if ((tb.middle.load (std::memory_order_relaxed) & TripleBuffer<T>::kNew) == 0)
        return false;
    tb.frontIdx = static_cast<std::uint8_t> (tb.middle.exchange (tb.frontIdx, std::memory_order_acq_rel) & 3);
    out = tb.slot[tb.frontIdx];
    return true;
}

// ------------------------------------------------------------------- capture (audio thread)

struct ImpactCapture
{
    int taps = 0, stride = 0;   // stride = 2 * taps floats per frame: mid, side per tap
    std::vector<float> buf {};
    std::atomic<std::uint32_t> head { 0 }, tail { 0 }, dropped { 0 };
    std::uint32_t blockStart = 0;
    int blockLen = 0;
    bool blockOk = false;
};

/** Allocates. The consumer must not be running. */
inline void prepare (ImpactCapture& c, int taps)
{
    c.taps = std::clamp (taps, 1, kMaxImpactLayers + 1);
    c.stride = 2 * c.taps;
    c.buf.assign (static_cast<std::size_t> (kImpactCaptureFrames) * static_cast<std::size_t> (c.stride), 0.0f);
    c.head.store (0);
    c.tail.store (0);
    c.dropped.store (0);
    c.blockOk = false;
}

inline std::uint32_t droppedSamples (const ImpactCapture& c) noexcept { return c.dropped.load (std::memory_order_relaxed); }

/** Audio thread, once per block. False (and counted) if the whole block does not fit. */
inline bool beginBlock (ImpactCapture& c, int numSamples) noexcept
{
    const auto h = c.head.load (std::memory_order_relaxed);
    const auto used = h - c.tail.load (std::memory_order_acquire);
    c.blockOk = numSamples > 0 && static_cast<std::uint32_t> (numSamples) <= static_cast<std::uint32_t> (kImpactCaptureFrames) - used;
    c.blockStart = h;
    c.blockLen = numSamples;
    if (! c.blockOk && numSamples > 0)
        c.dropped.fetch_add (static_cast<std::uint32_t> (numSamples), std::memory_order_relaxed);
    return c.blockOk;
}

/** Audio thread. A no-op inside a dropped block. */
inline void write (ImpactCapture& c, int tap, int index, float l, float r) noexcept
{
    if (! c.blockOk)
        return;
    const auto frame = (c.blockStart + static_cast<std::uint32_t> (index)) & static_cast<std::uint32_t> (kImpactCaptureFrames - 1);
    float* f = c.buf.data() + static_cast<std::size_t> (frame) * static_cast<std::size_t> (c.stride) + 2 * static_cast<std::size_t> (tap);
    f[0] = 0.5f * (l + r);
    f[1] = 0.5f * (l - r);
}

inline void endBlock (ImpactCapture& c) noexcept
{
    if (c.blockOk)
        c.head.store (c.blockStart + static_cast<std::uint32_t> (c.blockLen), std::memory_order_release);
    c.blockOk = false;
}

/** Consumer: copies one frame (stride floats) out. False if empty. */
inline bool popFrame (ImpactCapture& c, float* out) noexcept
{
    const auto t = c.tail.load (std::memory_order_relaxed);
    if (t == c.head.load (std::memory_order_acquire))
        return false;
    const float* f = c.buf.data() + static_cast<std::size_t> (t & static_cast<std::uint32_t> (kImpactCaptureFrames - 1)) * static_cast<std::size_t> (c.stride);
    std::copy (f, f + c.stride, out);
    c.tail.store (t + 1, std::memory_order_release);
    return true;
}

// ------------------------------------------------------------------- analyser (analysis thread)

struct ImpactFrame
{
    int layers = 0;
    std::array<std::array<float, kImpactBands>, kMaxImpactLayers> gainDb {}, widthDb {};
};

struct ImpactBandShare
{
    int band = 0, bin = 0;
    double weight = 0.0;
};

struct ImpactAnalyser
{
    double sampleRate = 48000.0;
    int layers = 0, fftSize = 2048, histLen = 0, maxLatency = 0;
    std::vector<int> latency {};
    std::vector<std::vector<float>> mid {}, side {};   // per tap, ring of histLen
    std::uint64_t written = 0;
    std::vector<float> window {}, frame {};
    std::vector<juce::dsp::Complex<float>> fftIn {}, fftOut {};
    std::unique_ptr<juce::dsp::FFT> fft {};
    std::vector<ImpactBandShare> shares {};
    std::vector<double> binPowM {}, binPowS {};
    struct Cached { int tap = -1; std::uint64_t end = 0; std::array<double, kImpactBands> m {}, s {}; bool ok = false; };
    Cached cache {};   // post of layer i is pre of layer i + 1 when that layer has no latency
    double coeff = 0.0;
    struct Smoothed { std::array<double, kImpactBands> preM {}, preS {}, postM {}, postS {}; };
    std::vector<Smoothed> smooth {};
    ImpactFrame current {};   // a layer without enough history keeps its last values
    TripleBuffer<ImpactFrame> out {};
};

/** Analysis thread setup: allocates, designs the band map. */
inline void prepare (ImpactAnalyser& a, double sampleRate, int layers)
{
    a.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    a.layers = std::clamp (layers, 1, kMaxImpactLayers);
    a.fftSize = impactFftSize (a.sampleRate);
    a.maxLatency = static_cast<int> (std::lround (kImpactMaxLatencyMs * 0.001 * a.sampleRate));
    a.histLen = 1;
    while (a.histLen < a.fftSize + a.maxLatency)
        a.histLen *= 2;
    a.latency.assign (static_cast<std::size_t> (a.layers), 0);
    a.mid.assign (static_cast<std::size_t> (a.layers + 1), std::vector<float> (static_cast<std::size_t> (a.histLen), 0.0f));
    a.side = a.mid;
    a.written = 0;
    a.window.resize (static_cast<std::size_t> (a.fftSize));
    for (int n = 0; n < a.fftSize; ++n)
        a.window[static_cast<std::size_t> (n)] = static_cast<float> (0.5 - 0.5 * std::cos (2.0 * std::numbers::pi * n / a.fftSize));
    a.fftIn.assign (static_cast<std::size_t> (a.fftSize), {});
    a.fftOut.assign (static_cast<std::size_t> (a.fftSize), {});
    a.frame.assign (2 * static_cast<std::size_t> (a.layers + 1), 0.0f);
    int order = 0;
    while ((1 << order) < a.fftSize)
        ++order;
    a.fft = std::make_unique<juce::dsp::FFT> (order);

    // Bin k spans [k - 1/2, k + 1/2] * df with flat density; a band takes the overlapping share.
    a.shares.clear();
    const double df = a.sampleRate / a.fftSize;
    const double edge = std::pow (2.0, 1.0 / 12.0);
    for (int b = 0; b < kImpactBands; ++b)
    {
        const double lo = impactBandCentreHz (b) / edge, hi = impactBandCentreHz (b) * edge;
        for (int k = std::max (1, static_cast<int> (std::floor (lo / df))); k <= a.fftSize / 2 && (k - 0.5) * df < hi; ++k)
        {
            const double ov = std::min (hi, (k + 0.5) * df) - std::max (lo, (k - 0.5) * df);
            if (ov > 0.0)
                a.shares.push_back ({ b, k, ov / df });
        }
    }
    a.binPowM.assign (static_cast<std::size_t> (a.fftSize / 2 + 1), 0.0);
    a.binPowS = a.binPowM;
    a.cache = {};
    a.coeff = std::exp (-1.0 / (kImpactFrameHz * kImpactSmoothingMs * 0.001));
    a.smooth.assign (static_cast<std::size_t> (a.layers), {});
    a.current = {};
    a.current.layers = a.layers;
    a.out.slot = {};
}

/** Per-layer latency in samples (clamped to 100 ms). Call when the chain's latency changes. */
inline void setLatencies (ImpactAnalyser& a, std::span<const int> lat)
{
    for (std::size_t i = 0; i < a.latency.size(); ++i)
        a.latency[i] = i < lat.size() ? std::clamp (lat[i], 0, a.maxLatency) : 0;
}

/** Drains the capture into the analyser's history. Returns frames taken. */
inline int pull (ImpactAnalyser& a, ImpactCapture& c) noexcept
{
    const auto taps = static_cast<std::size_t> (std::min (c.taps, a.layers + 1));
    const auto t0 = c.tail.load (std::memory_order_relaxed);
    const auto n = c.head.load (std::memory_order_acquire) - t0;
    const auto mask = static_cast<std::uint64_t> (a.histLen - 1);
    for (std::uint32_t i = 0; i < n; ++i)
    {
        const float* f = c.buf.data() + static_cast<std::size_t> ((t0 + i) & static_cast<std::uint32_t> (kImpactCaptureFrames - 1)) * static_cast<std::size_t> (c.stride);
        const auto p = static_cast<std::size_t> ((a.written + i) & mask);
        for (std::size_t t = 0; t < taps; ++t)
        {
            a.mid[t][p] = f[2 * t];
            a.side[t][p] = f[2 * t + 1];
        }
    }
    a.written += n;
    c.tail.store (t0 + n, std::memory_order_release);
    return static_cast<int> (n);
}

namespace impactdetail {

/** Mid and side band energies of the fftSize samples of tap `tap` ending at `end`, from one
 *  complex FFT of mid + j side (both real, so their spectra separate by conjugate symmetry).
 *  False if any sample is non-finite. */
inline bool bandEnergies (ImpactAnalyser& a, int tap, std::uint64_t end,
                          std::array<double, kImpactBands>& em, std::array<double, kImpactBands>& es) noexcept
{
    if (a.cache.tap == tap && a.cache.end == end)
    {
        em = a.cache.m;
        es = a.cache.s;
        return a.cache.ok;
    }
    const auto mask = static_cast<std::uint64_t> (a.histLen - 1);
    const std::uint64_t start = end - static_cast<std::uint64_t> (a.fftSize);
    const auto& rm = a.mid[static_cast<std::size_t> (tap)];
    const auto& rs = a.side[static_cast<std::size_t> (tap)];
    bool ok = true;
    for (int n = 0; n < a.fftSize; ++n)
    {
        const auto p = static_cast<std::size_t> ((start + static_cast<std::uint64_t> (n)) & mask);
        const float w = a.window[static_cast<std::size_t> (n)];
        a.fftIn[static_cast<std::size_t> (n)] = { rm[p] * w, rs[p] * w };
        ok = ok && std::isfinite (rm[p]) && std::isfinite (rs[p]);
    }
    a.cache.tap = tap;
    a.cache.end = end;
    a.cache.ok = ok;
    if (! ok)
        return false;
    a.fft->perform (a.fftIn.data(), a.fftOut.data(), false);
    const int n = a.fftSize;
    for (int k = 0; k <= n / 2; ++k)
    {
        const auto x = std::complex<double> (a.fftOut[static_cast<std::size_t> (k)].real(), a.fftOut[static_cast<std::size_t> (k)].imag());
        const auto y = std::conj (std::complex<double> (a.fftOut[static_cast<std::size_t> ((n - k) % n)].real(), a.fftOut[static_cast<std::size_t> ((n - k) % n)].imag()));
        a.binPowM[static_cast<std::size_t> (k)] = std::norm (0.5 * (x + y));
        a.binPowS[static_cast<std::size_t> (k)] = std::norm (0.5 * (x - y));   // |(x - y) / 2j|
    }
    // Scale so a band's energy is a mean square: |X|^2 * 2 / (N * sum w^2), sum w^2 = 3N/8.
    const double scale = 2.0 / (static_cast<double> (n) * 0.375 * static_cast<double> (n));
    em.fill (0.0);
    es.fill (0.0);
    for (const auto& sh : a.shares)
    {
        em[static_cast<std::size_t> (sh.band)] += sh.weight * a.binPowM[static_cast<std::size_t> (sh.bin)] * scale;
        es[static_cast<std::size_t> (sh.band)] += sh.weight * a.binPowS[static_cast<std::size_t> (sh.bin)] * scale;
    }
    a.cache.m = em;
    a.cache.s = es;
    return true;
}

inline float finishDb (double db) noexcept
{
    if (! std::isfinite (db) || std::abs (db) < 1e-4)
        return 0.0f;
    return std::clamp (static_cast<float> (db), -kImpactClampDb, kImpactClampDb);
}

/** S/M with a floor relative to the band's own energy: mono stays "mono" at any level, and a
 *  silent band reads 1 (no width change to show). */
inline double sideRatio (double s, double m) noexcept
{
    if (s + m < kImpactEnergyFloor)
        return 1.0;
    const double rel = kImpactWidthFloor * (s + m);
    return (s + rel) / (m + rel);
}

} // namespace impactdetail

/** Analysis thread, ~30 Hz. Publishes a frame once enough history exists. */
inline void computeFrame (ImpactAnalyser& a) noexcept
{
    auto& f = a.current;
    std::array<double, kImpactBands> pm {}, ps {}, qm {}, qs {};
    bool any = false;
    for (int i = 0; i < a.layers; ++i)
    {
        const auto li = static_cast<std::size_t> (i);
        const auto lat = static_cast<std::uint64_t> (a.latency[li]);
        if (a.written < static_cast<std::uint64_t> (a.fftSize) + lat)
            continue;
        const auto end = a.written, endPre = a.written - lat;
        const bool okPre = impactdetail::bandEnergies (a, i, endPre, pm, ps);
        const bool ok = impactdetail::bandEnergies (a, i + 1, end, qm, qs) && okPre;
        if (! ok)
            continue;   // a NaN in the window: keep the last smoothed state, never publish it
        auto& s = a.smooth[li];
        for (std::size_t b = 0; b < kImpactBands; ++b)
        {
            s.preM[b] = pm[b] + a.coeff * (s.preM[b] - pm[b]);
            s.preS[b] = ps[b] + a.coeff * (s.preS[b] - ps[b]);
            s.postM[b] = qm[b] + a.coeff * (s.postM[b] - qm[b]);
            s.postS[b] = qs[b] + a.coeff * (s.postS[b] - qs[b]);
            const double before = s.preM[b] + s.preS[b], after = s.postM[b] + s.postS[b];
            f.gainDb[li][b] = impactdetail::finishDb (10.0 * std::log10 ((after + kImpactEnergyFloor) / (before + kImpactEnergyFloor)));
            f.widthDb[li][b] = impactdetail::finishDb (10.0 * std::log10 (impactdetail::sideRatio (s.postS[b], s.postM[b])
                                                                          / impactdetail::sideRatio (s.preS[b], s.preM[b])));
        }
        any = true;
    }
    if (any)
    {
        back (a.out) = f;
        publish (a.out);
    }
}

/** UI side: the newest published frame. Returns true if it is new since the last read. */
inline bool readLatest (ImpactAnalyser& a, ImpactFrame& f) noexcept
{
    if (fetch (a.out, f))
        return true;
    f = a.out.slot[a.out.frontIdx];   // the reader owns the front slot
    return false;
}

} // namespace strat3::analysis
