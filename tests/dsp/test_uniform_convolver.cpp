// FFT convolver tests: checked against direct-form convolution, the only reference that
// cannot share a bug with the FFT path.
#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "dsp/crossover/UniformConvolver.h"
#include "support/Analysis.h"

using namespace strat3::dsp;
using namespace strat3::test;

namespace {

std::vector<float> directConvolve (const std::vector<float>& x, const std::vector<float>& h)
{
    std::vector<float> y (x.size(), 0.0f);
    for (size_t n = 0; n < x.size(); ++n)
    {
        double acc = 0.0;
        for (size_t k = 0; k < h.size() && k <= n; ++k)
            acc += static_cast<double> (h[k]) * static_cast<double> (x[n - k]);
        y[n] = static_cast<float> (acc);
    }
    return y;
}

std::vector<float> noiseVector (size_t n, unsigned seed)
{
    Noise noise { seed };
    std::vector<float> v (n);
    for (auto& s : v)
        s = noise.next();
    return v;
}

} // namespace

TEST_CASE ("convolver matches direct convolution, delayed by one partition", "[dsp][conv]")
{
    struct Case { int partition; size_t kernelLength; };
    for (const auto c : { Case { 64, 1000 }, Case { 64, 64 }, Case { 64, 10 }, Case { 256, 4097 }, Case { 32, 33 } })
    {
        INFO ("partition = " << c.partition << " kernel = " << c.kernelLength);
        const auto h1 = noiseVector (c.kernelLength, 1u);
        const auto h2 = noiseVector (c.kernelLength, 2u);
        const auto x  = noiseVector (8192, 3u);

        UniformConvolver conv;
        prepare (conv, c.partition, { h1, h2 });
        REQUIRE (latencySamples (conv) == c.partition);
        REQUIRE (numKernels (conv) == 2);

        std::vector<float> y1 (x.size()), y2 (x.size());
        for (size_t n = 0; n < x.size(); ++n)
        {
            float outs[2];
            process (conv, x[n], outs);
            y1[n] = outs[0];
            y2[n] = outs[1];
        }

        const auto r1 = directConvolve (x, h1);
        const auto r2 = directConvolve (x, h2);
        const auto L = static_cast<size_t> (c.partition);
        double maxErr = 0.0;
        for (size_t n = L; n < x.size(); ++n)
        {
            maxErr = std::max (maxErr, static_cast<double> (std::abs (y1[n] - r1[n - L])));
            maxErr = std::max (maxErr, static_cast<double> (std::abs (y2[n] - r2[n - L])));
        }
        for (size_t n = 0; n < L; ++n)
            REQUIRE (y1[n] == 0.0f);   // nothing leaks out before the reported latency

        // Error budget scales with kernel energy: ~sqrt(N) * float eps * |x||h|.
        CHECK (maxErr < 1e-5 * std::sqrt (static_cast<double> (c.kernelLength)) * 4.0);
    }
}

TEST_CASE ("convolver: unit impulse kernel is a pure delay", "[dsp][conv][null]")
{
    UniformConvolver conv;
    prepare (conv, 128, { std::vector<float> { 1.0f } });
    const auto x = noiseVector (4096, 9u);
    double maxErr = 0.0;
    for (size_t n = 0; n < x.size(); ++n)
    {
        float y;
        process (conv, x[n], &y);
        if (n >= 128)
            maxErr = std::max (maxErr, static_cast<double> (std::abs (y - x[n - 128])));
    }
    CHECK (maxErr < 1e-6);
}

TEST_CASE ("convolver: reset clears history; unprepared is silent and safe", "[dsp][conv]")
{
    UniformConvolver conv;
    float y = 1.0f;
    process (conv, 1.0f, &y);   // before prepare
    CHECK (y == 0.0f);

    prepare (conv, 64, { noiseVector (300, 4u) });
    for (int i = 0; i < 1000; ++i)
        process (conv, 1.0f, &y);
    reset (conv);
    for (int i = 0; i < 1000; ++i)
    {
        process (conv, 0.0f, &y);
        REQUIRE (y == 0.0f);
    }
}
