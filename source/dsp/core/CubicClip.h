#pragma once
// Cubic soft clip with unity slope at 0 and a hard ceiling c:
//   f(u) = c * s(u / c),  s(v) = v - 4 v^3 / 27  for |v| <= 1.5,  sign(v) beyond (C1 at the knee).
// Given in residual form r = f - u, with its integral R (R(0) = 0), for first-order ADAA.
// Shared by the SATURATE triode model (different c per polarity) and the LIMIT clipper.
// Each half-plane depends only on its own c, so an asymmetric clip is just a per-sign c.
#include <cmath>

namespace strat3::dsp {

inline double cubicClipResidual (double u, double c) noexcept
{
    if (std::abs (u) <= 1.5 * c)
        return -4.0 * u * u * u / (27.0 * c * c);
    return (u >= 0.0 ? c : -c) - u;
}

inline double cubicClipIntegral (double u, double c) noexcept
{
    const double a = std::abs (u);
    if (a <= 1.5 * c)
        return -u * u * u * u / (27.0 * c * c);   // direct: no u^2/2 cancellation near 0
    return c * a - 0.5625 * c * c - 0.5 * u * u;  // continuous at the knee: both -0.1875 c^2
}

} // namespace strat3::dsp
