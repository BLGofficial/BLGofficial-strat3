#pragma once
// Measurement helpers for DSP tests. Double precision so the measurement never limits
// the assertion tolerance.
#include <cmath>
#include <complex>
#include <cstdint>
#include <numbers>
#include <vector>

namespace strat3::test {

/** Complex DTFT of `ir` at frequency `hz`. Exact for a finite IR. */
inline std::complex<double> dtft (const std::vector<float>& ir, double hz, double sampleRate)
{
    const double w = 2.0 * std::numbers::pi * hz / sampleRate;
    std::complex<double> sum { 0.0, 0.0 };
    for (size_t n = 0; n < ir.size(); ++n)
        sum += static_cast<double> (ir[n]) * std::polar (1.0, -w * static_cast<double> (n));
    return sum;
}

/** Magnitude in dB of the DTFT of `ir` at frequency `hz`. */
inline double magnitudeDb (const std::vector<float>& ir, double hz, double sampleRate)
{
    return 20.0 * std::log10 (std::max (std::abs (dtft (ir, hz, sampleRate)), 1e-30));
}

/** Runs `process(x)` on a unit impulse followed by zeros. */
template <typename Fn>
std::vector<float> impulseResponse (Fn&& process, size_t length)
{
    std::vector<float> out (length);
    for (size_t n = 0; n < length; ++n)
        out[n] = process (n == 0 ? 1.0f : 0.0f);
    return out;
}

inline bool isSubnormal (float v) { return std::fpclassify (v) == FP_SUBNORMAL; }

/** Deterministic white noise in [-1, 1] (LCG): tests must not depend on a random seed. */
struct Noise
{
    unsigned state = 0x12345678u;
    float next()
    {
        state = state * 1664525u + 1013904223u;
        return static_cast<float> (state >> 8) / static_cast<float> (1u << 23) * 2.0f - 1.0f;
    }
};

/** White at any stride and independent across seeds (splitmix64 of a counter). Use it when
 *  channels or bands must be uncorrelated: the LCG Noise above is not. Read with a stride
 *  of 4 (one value per band) its sum has ~30 dB more energy at 10 Hz than at 1 kHz, and two
 *  seeds (default and 0xBEEF) measured a correlation of 0.43. */
struct WhiteNoise
{
    std::uint64_t counter = 0;
    float next()
    {
        std::uint64_t z = (counter += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        return static_cast<float> (z >> 40) / static_cast<float> (1u << 24) * 2.0f - 1.0f;
    }
};

} // namespace strat3::test
