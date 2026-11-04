#pragma once
// Band -> SVF sections, and analytic magnitude responses. The same functions drive the IIR
// EQ, the linear-phase FIR design and (later) the UI curve, so all three always agree.
#include <array>
#include <cmath>
#include <complex>
#include <numbers>

#include "dsp/core/SvfTpt.h"
#include "dsp/eq/EqTypes.h"

namespace strat3::dsp {

struct BandSections
{
    std::array<SvfCoeffs, 2> sections { passthroughCoeffs(), passthroughCoeffs() };
    int count = 1;
};

/** Coefficients for a band with an extra (dynamic) gain offset. Off bands are passthrough. */
inline BandSections bandSections (const EqBand& b, float extraGainDb, double sampleRate) noexcept
{
    BandSections out;
    if (! b.on)
        return out;

    const float gain = b.gainDb + (supportsGain (b.type) ? extraGainDb : 0.0f);
    switch (b.type)
    {
        case EqType::Bell:      out.sections[0] = makeSvfCoeffs (SvfType::Bell,      b.freq, b.q, gain, sampleRate); break;
        case EqType::LowShelf:  out.sections[0] = makeSvfCoeffs (SvfType::LowShelf,  b.freq, b.q, gain, sampleRate); break;
        case EqType::HighShelf: out.sections[0] = makeSvfCoeffs (SvfType::HighShelf, b.freq, b.q, gain, sampleRate); break;
        case EqType::HighPass:  out.sections[0] = makeSvfCoeffs (SvfType::HighPass,  b.freq, b.q, 0.0f, sampleRate); break;
        case EqType::LowPass:   out.sections[0] = makeSvfCoeffs (SvfType::LowPass,   b.freq, b.q, 0.0f, sampleRate); break;
        case EqType::PassiveShelf:
            out.sections[0] = makeSvfCoeffs (SvfType::LowShelf, b.freq, b.q, gain, sampleRate);
            out.sections[1] = makeSvfCoeffs (SvfType::Bell, b.freq * kPassiveDipRatio, kPassiveDipQ,
                                             -kPassiveDipAmount * gain, sampleRate);
            out.count = 2;
            break;
    }
    return out;
}

/** |H| of one TPT SVF section. TPT = bilinear transform with prewarping, so the digital
 *  response is the analog prototype at x. */
inline double svfMagnitudeDb (const SvfCoeffs& c, double hz, double sampleRate) noexcept
{
    if (c.g <= 0.0f)
        return 20.0 * std::log10 (std::max (std::abs (static_cast<double> (c.m0)), 1e-30));

    const std::complex<double> S { 0.0, std::tan (std::numbers::pi * hz / sampleRate) / static_cast<double> (c.g) };
    const auto D = S * S + static_cast<double> (c.k) * S + 1.0;
    const auto H = static_cast<double> (c.m0) + static_cast<double> (c.m1) * S / D + static_cast<double> (c.m2) / D;
    return 20.0 * std::log10 (std::max (std::abs (H), 1e-30));
}

/** Static magnitude of the bands that act on `channel`:
 *  Stereo -> stereo bands only (L/R path); Mid -> stereo + mid bands; Side -> stereo + side.
 *  (A stereo band applied to M and S equals the same band applied to L and R: it is linear.) */
inline double settingsMagnitudeDb (const EqSettings& s, double hz, double sampleRate, EqChannel channel,
                                   bool includeDynamicBands = true) noexcept
{
    double db = 0.0;
    for (const auto& b : s)
    {
        if (! b.on || (! includeDynamicBands && b.dyn != EqDynMode::Off))
            continue;
        if (b.channel != EqChannel::Stereo && b.channel != channel)
            continue;
        const auto sec = bandSections (b, 0.0f, sampleRate);
        for (int i = 0; i < sec.count; ++i)
            db += svfMagnitudeDb (sec.sections[static_cast<size_t> (i)], hz, sampleRate);
    }
    return db;
}

} // namespace strat3::dsp
