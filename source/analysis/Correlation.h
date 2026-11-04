#pragma once
// Running L/R correlation: E[LR] / sqrt(E[L^2] E[R^2]), each average a one-pole with a 300 ms
// time constant. Drives the mono guard and the delivery-bar CORR readout.
// Doubles: the averages are long (14k samples at 48 kHz) and must not drift.
#include <algorithm>
#include <cmath>

namespace strat3::analysis {

inline constexpr float kCorrelationMs = 300.0f;

struct CorrelationMeter
{
    double lr = 0.0, ll = 0.0, rr = 0.0;
    double coeff = 0.0;
};

inline void prepare (CorrelationMeter& m, double sampleRate, float timeMs = kCorrelationMs)
{
    const double samples = static_cast<double> (timeMs) * 0.001 * (sampleRate > 0.0 ? sampleRate : 48000.0);
    m.coeff = samples > 0.0 ? std::exp (-1.0 / samples) : 0.0;
    m.lr = m.ll = m.rr = 0.0;
}

namespace corrdetail {
inline double follow (double state, double target, double coeff) noexcept
{
    const double next = target + coeff * (state - target);
    return std::abs (next) < 1e-30 ? 0.0 : next;   // minutes of silence must not reach subnormals
}
} // namespace corrdetail

inline void push (CorrelationMeter& m, float l, float r) noexcept
{
    const double dl = l, dr = r;
    m.lr = corrdetail::follow (m.lr, dl * dr, m.coeff);
    m.ll = corrdetail::follow (m.ll, dl * dl, m.coeff);
    m.rr = corrdetail::follow (m.rr, dr * dr, m.coeff);
}

/** +1 mono, 0 unrelated (or one channel silent), -1 inverted. Silence reads +1: there is
 *  nothing to protect, and a guard reading silence as 0 would kill stereoize at every gap. */
inline float value (const CorrelationMeter& m) noexcept
{
    constexpr double floor = 1e-14;   // -140 dB power
    if (m.ll < floor && m.rr < floor)
        return 1.0f;
    return static_cast<float> (std::clamp (m.lr / std::sqrt (m.ll * m.rr + 1e-30), -1.0, 1.0));
}

inline bool isFinite (const CorrelationMeter& m) noexcept
{
    return std::isfinite (m.lr) && std::isfinite (m.ll) && std::isfinite (m.rr);
}

inline void reset (CorrelationMeter& m) noexcept { m.lr = m.ll = m.rr = 0.0; }

} // namespace strat3::analysis
