#pragma once
// Linear-phase FIR design for the MASTER-mode crossover. Allocates: message/background
// thread only, never the audio thread.
#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

namespace strat3::dsp {

/** Odd tap count giving ~42.7 ms group delay at any sample rate (2048 samples at 48 kHz).
 *  Scaling taps with fs keeps the frequency resolution, and the latency in ms, constant. */
inline std::size_t firCrossoverTaps (double sampleRate) noexcept
{
    const double half = std::round (2048.0 * (sampleRate > 0.0 ? sampleRate : 48000.0) / 48000.0);
    return 2 * static_cast<std::size_t> (half) + 1;
}

namespace firdetail {

/** Modified Bessel I0 by power series. Own implementation: std::cyl_bessel_i is missing
 *  from libc++ (macOS), which is a release target. */
inline double besselI0 (double x) noexcept
{
    double sum = 1.0, term = 1.0;
    const double q = x * x / 4.0;
    for (int k = 1; k < 64; ++k)
    {
        term *= q / (static_cast<double> (k) * static_cast<double> (k));
        sum += term;
        if (term < sum * 1e-17)
            break;
    }
    return sum;
}

} // namespace firdetail

/** Kaiser-windowed sinc low-pass, normalised to exactly unity DC gain.
 *  Built from the centre outwards and mirrored, so it is bit-exactly symmetric (true linear
 *  phase): band differences LP2 - LP1 then stay linear phase too.
 *  beta 9 gives ~-90 dB sidelobes; transition width ~ 5.7 * fs / numTaps.
 *  @param numTaps must be odd; an even value is bumped up by one. */
inline std::vector<float> designLinearPhaseLowpass (float fc, double sampleRate, std::size_t numTaps, double beta = 9.0)
{
    if (numTaps % 2 == 0)
        ++numTaps;

    const std::size_t centre = numTaps / 2;
    const double wc = 2.0 * static_cast<double> (fc) / sampleRate;   // normalised cutoff, 1 = Nyquist
    const double i0Beta = firdetail::besselI0 (beta);

    std::vector<double> h (numTaps);
    for (std::size_t i = 0; i <= centre; ++i)
    {
        const double m = static_cast<double> (i);   // distance from centre
        const double x = std::numbers::pi * wc * m;
        const double sinc = i == 0 ? wc : wc * std::sin (x) / x;
        const double r = m / static_cast<double> (centre);
        const double window = firdetail::besselI0 (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0Beta;
        h[centre + i] = h[centre - i] = sinc * window;
    }

    double dc = 0.0;
    for (const double v : h)
        dc += v;

    std::vector<float> out (numTaps);
    for (std::size_t i = 0; i <= centre; ++i)
        out[centre + i] = out[centre - i] = static_cast<float> (h[centre + i] / dc);
    return out;
}

} // namespace strat3::dsp
