#pragma once
// Per-sample polyphase oversampler: 1x, 2x or 4x as cascaded 2x halfband stages.
// Own implementation, not juce::dsp::Oversampling: the band domain runs one sample at a time
// (decided 2026-11-03), and JUCE's oversampler is block-based.
//
//   IIR (TRACK/MIX): halfband H(z) = 1/2 [A0(z^2) + z^-1 A1(z^2)], A0/A1 cascades of first-order
//       all-passes. Coefficients from the closed-form elliptic halfband design (Valenzuela &
//       Constantinides 1983). Near-zero latency, non-linear phase.
//   FIR (MASTER): Kaiser-windowed halfband sinc, linear phase, integer latency.
//
// Both are designed once for the worst case, a 44.1 kHz base rate: passband to 20 kHz,
// >= 100 dB rejection of every image and alias that lands below 20 kHz. Stage 2 (2x -> 4x) only
// has to reject from 2 fs - 20 kHz, so it is much shorter.
//
// Polyphase conventions (the tests pin them):
//   up:   out[2n] = branch0(x[n]),  out[2n+1] = branch1(x[n])  (= 2 h * zero-stuffed x)
//   down: IIR keeps the odd output phase  y[n] = 1/2 [A0(in1) + A1(in0)];
//         FIR keeps the even phase       y[n] = sum h[2j] E[n-j] + sum h[2j+1] O[n-1-j].
// Design runs in makeOsConfig (prepare, allocates). Processing never allocates.
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <numbers>
#include <vector>

#include "dsp/crossover/FirDesign.h"

namespace strat3::dsp {

enum class OsKind { Iir, Fir };

inline constexpr int kOsMaxIirCoefs = 12;
inline constexpr int kOsMaxPhaseTaps = 80;
inline constexpr int kOsHistory = 128;         // ring length per stream (> max tap offset + 1)
inline constexpr double kOsDesignRate = 44100.0;
inline constexpr double kOsPassbandHz = 20000.0;
// Measured (tests): stage 1 >= 106 dB, stage 2 >= 119 dB for IIR; FIR designed for 110 dB.
inline constexpr int kOsIirCoefsStage1 = 10;
inline constexpr int kOsIirCoefsStage2 = 6;
inline constexpr double kOsFirAttenDb = 110.0;

struct HalfbandStage
{
    OsKind kind = OsKind::Iir;
    std::array<float, kOsMaxIirCoefs> a {};    // IIR: even index -> branch 0, odd -> branch 1
    int numCoefs = 0;
    // FIR halfband, polyphase branch p = taps h[2j+p]. One branch is dense (every tap non-zero),
    // the other holds only the centre tap, exactly 1/2. Dense taps are stored time-reversed so
    // the dot product reads the history ascending and contiguously (vectorises).
    std::array<float, kOsMaxPhaseTaps> dense {};
    int numDense = 0;
    int denseBranch = 0;
    int sparseOffset = 0;   // j of the centre tap in its branch
    int centre = 0;                            // FIR: (N - 1) / 2, in samples at this stage's high rate
};

struct OsConfig
{
    int factor = 1;
    OsKind kind = OsKind::Iir;
    HalfbandStage stage1 {}, stage2 {};
    int latency = 0;
};

/** One stream's history, mirrored so the taps read one contiguous run: buf[pos + kOsHistory - j] = s[n - j]. */
struct OsRing
{
    std::array<float, 2 * kOsHistory> buf {};
    int pos = 0;
};

struct HalfbandUpState
{
    std::array<float, kOsMaxIirCoefs> x1 {}, y1 {};
    OsRing in {};
};

struct HalfbandDownState
{
    std::array<float, kOsMaxIirCoefs> x1 {}, y1 {};
    OsRing even {}, odd {};
};

struct UpChain   { HalfbandUpState s1 {}, s2 {}; };
struct DownChain { HalfbandDownState s1 {}, s2 {}; };

namespace osdetail {

/** Elliptic halfband all-pass coefficients, ascending. `transition` = stopband edge - 0.25,
 *  normalised to the high rate. */
inline std::vector<double> designIirHalfband (int numCoefs, double transition)
{
    using std::numbers::pi;
    double k = std::tan ((1.0 - 2.0 * transition) * pi / 4.0);
    k *= k;
    const double kk = std::pow (1.0 - k * k, 0.25);
    const double e = 0.5 * (1.0 - kk) / (1.0 + kk);
    const double e4 = e * e * e * e;
    const double q = e * (1.0 + e4 * (2.0 + e4 * (15.0 + 150.0 * e4)));   // elliptic nome
    const double order = 2.0 * numCoefs + 1.0;

    std::vector<double> c (static_cast<size_t> (numCoefs));
    for (int idx = 0; idx < numCoefs; ++idx)
    {
        const double m = idx + 1.0;
        double num = 0.0, den = 0.0;
        for (int i = 0; i < 64; ++i)   // theta-function series; q < 0.2, so it converges fast
        {
            const double sign = (i % 2 == 0) ? 1.0 : -1.0;
            num += sign * std::pow (q, i * (i + 1.0)) * std::sin ((2.0 * i + 1.0) * m * pi / order);
            if (i > 0)
                den += sign * std::pow (q, static_cast<double> (i) * i) * std::cos (2.0 * i * m * pi / order);
        }
        const double ww = num * std::pow (q, 0.25) / (den + 0.5);
        const double w2 = ww * ww;
        const double x = std::sqrt ((1.0 - w2 * k) * (1.0 - w2 / k)) / (1.0 + w2);
        c[static_cast<size_t> (idx)] = (1.0 - x) / (1.0 + x);
    }
    return c;
}

/** Kaiser halfband: centre 1/2, zeros at even offsets, odd offsets rescaled so each polyphase
 *  branch sums to exactly 1/2 (otherwise DC images at the base rate). */
inline std::vector<double> designFirHalfband (int numTaps, double beta)
{
    const int c = (numTaps - 1) / 2;
    const double i0Beta = firdetail::besselI0 (beta);
    std::vector<double> h (static_cast<size_t> (numTaps), 0.0);
    double sum = 0.0;
    for (int n = 0; n < numTaps; ++n)
    {
        const int d = n - c;
        if (d == 0 || d % 2 == 0)
            continue;
        const double x = std::numbers::pi * 0.5 * d;
        const double r = static_cast<double> (d) / c;
        const double w = firdetail::besselI0 (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0Beta;
        h[static_cast<size_t> (n)] = 0.5 * std::sin (x) / x * w;
        sum += h[static_cast<size_t> (n)];
    }
    for (auto& v : h)
        v *= 0.5 / sum;
    h[static_cast<size_t> (c)] = 0.5;
    return h;
}

/** Kaiser length for `attenDb` over the band [pass, stop] (normalised to the high rate),
 *  rounded up to the form that keeps the cascade latency an integer. */
inline int firHalfbandLength (double attenDb, double pass, double stop, int modulo4)
{
    const double dw = 2.0 * std::numbers::pi * (stop - pass);
    int n = static_cast<int> (std::ceil ((attenDb - 7.95) / (2.285 * dw))) + 1;
    while (n % 4 != modulo4)
        ++n;
    return n;
}

inline HalfbandStage makeIirStage (int numCoefs, double transition)
{
    HalfbandStage s;
    s.kind = OsKind::Iir;
    const auto c = designIirHalfband (numCoefs, transition);
    s.numCoefs = numCoefs;
    for (int i = 0; i < numCoefs; ++i)
        s.a[static_cast<size_t> (i)] = static_cast<float> (c[static_cast<size_t> (i)]);
    return s;
}

inline HalfbandStage makeFirStage (int numTaps)
{
    HalfbandStage s;
    s.kind = OsKind::Fir;
    const double beta = 0.1102 * (kOsFirAttenDb - 8.7);
    const auto h = designFirHalfband (numTaps, beta);
    s.centre = (numTaps - 1) / 2;
    // Centre odd -> it sits in branch 1 and branch 0 is dense; centre even -> the reverse.
    s.denseBranch = s.centre % 2 == 1 ? 0 : 1;
    s.sparseOffset = s.centre / 2;
    s.numDense = (numTaps - s.denseBranch + 1) / 2;
    for (int j = 0; j < s.numDense; ++j)
        s.dense[static_cast<size_t> (s.numDense - 1 - j)] = static_cast<float> (h[static_cast<size_t> (2 * j + s.denseBranch)]);
    return s;
}

inline std::complex<double> iirResponse (const HalfbandStage& s, double w)   // w at the high rate
{
    const std::complex<double> z2 = std::polar (1.0, -2.0 * w);
    std::complex<double> b0 { 1.0, 0.0 }, b1 { 1.0, 0.0 };
    for (int i = 0; i < s.numCoefs; ++i)
    {
        const double a = s.a[static_cast<size_t> (i)];
        const auto ap = (a + z2) / (1.0 + a * z2);
        (i % 2 == 0 ? b0 : b1) *= ap;
    }
    return 0.5 * (b0 + std::polar (1.0, -w) * b1);
}

inline void push (OsRing& r, float v) noexcept
{
    r.pos = (r.pos + 1) & (kOsHistory - 1);
    r.buf[static_cast<size_t> (r.pos)] = v;
    r.buf[static_cast<size_t> (r.pos + kOsHistory)] = v;
}

/** sum_j h[2j + denseBranch] * s[n - j - extra]. Four accumulators: without fast-math the
 *  compiler may not reorder one float sum. */
inline float denseDot (const HalfbandStage& s, const OsRing& r, int extra) noexcept
{
    const float* x = r.buf.data() + r.pos + kOsHistory - extra - (s.numDense - 1);
    const float* h = s.dense.data();
    float a0 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
    int i = 0;
    for (; i + 4 <= s.numDense; i += 4)
    {
        a0 += h[i] * x[i];
        a1 += h[i + 1] * x[i + 1];
        a2 += h[i + 2] * x[i + 2];
        a3 += h[i + 3] * x[i + 3];
    }
    for (; i < s.numDense; ++i)
        a0 += h[i] * x[i];
    return (a0 + a1) + (a2 + a3);
}

/** s[n - j] */
inline float tapAt (const OsRing& r, int j) noexcept
{
    return r.buf[static_cast<size_t> (r.pos + kOsHistory - j)];
}

inline float allpassBranch (const HalfbandStage& s, float* x1, float* y1, int branch, float x) noexcept
{
    for (int i = branch; i < s.numCoefs; i += 2)
    {
        const auto k = static_cast<size_t> (i);
        const float y = s.a[k] * (x - y1[k]) + x1[k];
        x1[k] = x;
        y1[k] = y;
        x = y;
    }
    return x;
}

inline void up2 (const HalfbandStage& s, HalfbandUpState& st, float x, float& o0, float& o1) noexcept
{
    if (s.kind == OsKind::Iir)
    {
        o0 = allpassBranch (s, st.x1.data(), st.y1.data(), 0, x);
        o1 = allpassBranch (s, st.x1.data(), st.y1.data(), 1, x);
        return;
    }
    push (st.in, x);
    const float d = 2.0f * denseDot (s, st.in, 0);
    const float c = tapAt (st.in, s.sparseOffset);   // 2 * 1/2 * x[n - j]
    o0 = s.denseBranch == 0 ? d : c;
    o1 = s.denseBranch == 0 ? c : d;
}

inline float down2 (const HalfbandStage& s, HalfbandDownState& st, float i0, float i1) noexcept
{
    if (s.kind == OsKind::Iir)
        return 0.5f * (allpassBranch (s, st.x1.data(), st.y1.data(), 0, i1)
                       + allpassBranch (s, st.x1.data(), st.y1.data(), 1, i0));
    push (st.even, i0);
    push (st.odd, i1);
    // Branch 0 reads the even stream E[n - j]; branch 1 the odd stream one pair back, O[n - 1 - j].
    if (s.denseBranch == 0)
        return denseDot (s, st.even, 0) + 0.5f * tapAt (st.odd, s.sparseOffset + 1);
    return denseDot (s, st.odd, 1) + 0.5f * tapAt (st.even, s.sparseOffset);
}

template <typename Fn>
void forEachIirState (std::array<float, kOsMaxIirCoefs>& x1, std::array<float, kOsMaxIirCoefs>& y1, Fn&& fn)
{
    for (auto& v : x1) fn (v);
    for (auto& v : y1) fn (v);
}

} // namespace osdetail

/** Non-RT (designs filters). Unsupported factors fall back to 1x. */
inline OsConfig makeOsConfig (int factor, OsKind kind, double sampleRate)
{
    OsConfig c;
    c.kind = kind;
    c.factor = (factor == 2 || factor == 4) ? factor : 1;
    if (c.factor == 1)
        return c;

    const double fs = sampleRate > 0.0 ? sampleRate : 48000.0;
    const double pass1 = kOsPassbandHz / (2.0 * kOsDesignRate), stop1 = 0.5 - pass1;
    const double pass2 = kOsPassbandHz / (4.0 * kOsDesignRate), stop2 = 0.5 - pass2;

    if (kind == OsKind::Iir)
    {
        c.stage1 = osdetail::makeIirStage (kOsIirCoefsStage1, stop1 - 0.25);
        if (c.factor == 4)
            c.stage2 = osdetail::makeIirStage (kOsIirCoefsStage2, stop2 - 0.25);

        // Latency estimate: phase delay at 1 kHz. Each stage's composite is the odd phase of
        // 2 h*h at its high rate (hence the -1 high-rate sample).
        const double wb = 2.0 * std::numbers::pi * 1000.0 / fs;
        auto stageDelay = [&] (const HalfbandStage& s, int rateMul) {   // in base samples
            const double w = wb / rateMul;
            const double phase = 2.0 * std::arg (osdetail::iirResponse (s, w));
            return (-phase / w - 1.0) / rateMul;
        };
        double d = stageDelay (c.stage1, 2);
        if (c.factor == 4)
            d += stageDelay (c.stage2, 4);
        c.latency = static_cast<int> (std::lround (d));
        return c;
    }

    // FIR: stage 1 length 4K+3 (centre odd -> latency (N-1)/2 base samples); stage 2 4K+1 so its
    // (N-1)/4 share is an integer too.
    c.stage1 = osdetail::makeFirStage (osdetail::firHalfbandLength (kOsFirAttenDb, pass1, stop1, 3));
    c.latency = c.stage1.centre;
    if (c.factor == 4)
    {
        c.stage2 = osdetail::makeFirStage (osdetail::firHalfbandLength (kOsFirAttenDb, pass2, stop2, 1));
        c.latency += c.stage2.centre / 2;
    }
    return c;
}

inline int latencySamples (const OsConfig& c) noexcept { return c.latency; }

inline void reset (UpChain& u) noexcept { u = {}; }
inline void reset (DownChain& d) noexcept { d = {}; }

/** Writes `factor` high-rate samples to `out` (room for 4); returns the factor. */
inline int upsample (const OsConfig& c, UpChain& u, float x, float* out) noexcept
{
    if (c.factor == 1)
    {
        out[0] = x;
        return 1;
    }
    if (c.factor == 2)
    {
        osdetail::up2 (c.stage1, u.s1, x, out[0], out[1]);
        return 2;
    }
    float a, b;
    osdetail::up2 (c.stage1, u.s1, x, a, b);
    osdetail::up2 (c.stage2, u.s2, a, out[0], out[1]);
    osdetail::up2 (c.stage2, u.s2, b, out[2], out[3]);
    return 4;
}

/** Reads `factor` high-rate samples from `in`; returns one base-rate sample. */
inline float downsample (const OsConfig& c, DownChain& d, const float* in) noexcept
{
    if (c.factor == 1)
        return in[0];
    if (c.factor == 2)
        return osdetail::down2 (c.stage1, d.s1, in[0], in[1]);
    const float p = osdetail::down2 (c.stage2, d.s2, in[0], in[1]);
    const float q = osdetail::down2 (c.stage2, d.s2, in[2], in[3]);
    return osdetail::down2 (c.stage1, d.s1, p, q);
}

/** IIR states only. The FIR rings hold delayed inputs: a finite memory that empties to exact
 *  zeros within one filter length of silence, so it needs neither flushing nor checking here
 *  beyond the finite check below. */
template <typename Fn>
void forEachFloatState (UpChain& u, Fn&& fn)
{
    osdetail::forEachIirState (u.s1.x1, u.s1.y1, fn);
    osdetail::forEachIirState (u.s2.x1, u.s2.y1, fn);
}

template <typename Fn>
void forEachFloatState (DownChain& d, Fn&& fn)
{
    osdetail::forEachIirState (d.s1.x1, d.s1.y1, fn);
    osdetail::forEachIirState (d.s2.x1, d.s2.y1, fn);
}

namespace osdetail {

inline bool ringFinite (const OsRing& r) noexcept
{
    for (float v : r.buf)
        if (! std::isfinite (v))
            return false;
    return true;
}

} // namespace osdetail

inline bool resetIfNotFinite (UpChain& u) noexcept
{
    bool bad = ! osdetail::ringFinite (u.s1.in) || ! osdetail::ringFinite (u.s2.in);
    forEachFloatState (u, [&] (float& v) { bad |= ! std::isfinite (v); });
    if (bad)
        reset (u);
    return bad;
}

inline bool resetIfNotFinite (DownChain& d) noexcept
{
    bool bad = ! osdetail::ringFinite (d.s1.even) || ! osdetail::ringFinite (d.s1.odd)
            || ! osdetail::ringFinite (d.s2.even) || ! osdetail::ringFinite (d.s2.odd);
    forEachFloatState (d, [&] (float& v) { bad |= ! std::isfinite (v); });
    if (bad)
        reset (d);
    return bad;
}

inline void flushDenormals (UpChain& u) noexcept
{
    forEachFloatState (u, [] (float& v) { if (std::abs (v) < 1e-15f) v = 0.0f; });
}

inline void flushDenormals (DownChain& d) noexcept
{
    forEachFloatState (d, [] (float& v) { if (std::abs (v) < 1e-15f) v = 0.0f; });
}

} // namespace strat3::dsp
