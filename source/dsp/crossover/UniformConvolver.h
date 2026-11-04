#pragma once
// Uniformly partitioned overlap-save FFT convolver (UPOLS) with K kernels sharing one input.
// The band FIRs all filter the same signal, so the input FFT and its frequency-domain delay
// line are computed once and reused by every kernel.
// Latency: exactly one partition (B samples). Cost per block: 1 forward FFT + K * (P complex
// MACs + 1 inverse FFT), with P = ceil(kernelLength / B).
#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <complex>
#include <memory>
#include <vector>

namespace strat3::dsp {

struct UniformConvolver
{
    int partition = 0;       // B
    int fftSize = 0;         // 2B
    int numPartitions = 0;   // P
    int numKernels = 0;      // K
    int bins = 0;            // B + 1 non-negative-frequency bins
    std::unique_ptr<juce::dsp::FFT> fft {};

    std::vector<std::vector<std::complex<float>>> kernelSpectra {};   // [k][p * bins + bin]
    std::vector<std::complex<float>> fdl {};                           // [p * bins + bin]
    int fdlHead = 0;

    std::vector<float> frame {};        // [previous B | current B]
    std::vector<float> work {};         // 2 * fftSize scratch for JUCE's in-place real FFT
    std::vector<std::complex<float>> accum {};
    std::vector<float> outputs {};      // [k * B + i], the block being emitted
    int fill = 0;
};

namespace convdetail {

inline void forwardInto (UniformConvolver& c, const float* timeDomain, std::complex<float>* spectrum)
{
    std::fill (c.work.begin(), c.work.end(), 0.0f);
    std::copy (timeDomain, timeDomain + c.fftSize, c.work.begin());
    c.fft->performRealOnlyForwardTransform (c.work.data(), true);
    const auto* bins = reinterpret_cast<const std::complex<float>*> (c.work.data());
    std::copy (bins, bins + c.bins, spectrum);
}

inline void computeBlock (UniformConvolver& c)
{
    forwardInto (c, c.frame.data(), c.fdl.data() + static_cast<size_t> (c.fdlHead * c.bins));

    for (int k = 0; k < c.numKernels; ++k)
    {
        std::fill (c.accum.begin(), c.accum.end(), std::complex<float> {});
        const auto& H = c.kernelSpectra[static_cast<size_t> (k)];

        for (int p = 0; p < c.numPartitions; ++p)
        {
            const int slot = (c.fdlHead - p + c.numPartitions) % c.numPartitions;
            const auto* X  = c.fdl.data() + static_cast<size_t> (slot * c.bins);
            const auto* Hp = H.data() + static_cast<size_t> (p * c.bins);
            for (int b = 0; b < c.bins; ++b)
                c.accum[static_cast<size_t> (b)] += X[b] * Hp[b];
        }

        std::fill (c.work.begin(), c.work.end(), 0.0f);
        std::copy (c.accum.begin(), c.accum.end(), reinterpret_cast<std::complex<float>*> (c.work.data()));
        c.fft->performRealOnlyInverseTransform (c.work.data());

        // Overlap-save: only the last B samples of the circular result are linear convolution.
        std::copy (c.work.begin() + c.partition, c.work.begin() + c.fftSize,
                   c.outputs.begin() + k * c.partition);
    }

    std::copy (c.frame.begin() + c.partition, c.frame.end(), c.frame.begin());
    c.fdlHead = (c.fdlHead + 1) % c.numPartitions;
}

} // namespace convdetail

/** Allocates and transforms the kernels. prepareToPlay / background thread only.
 *  @param partition power of two >= 16; it is the latency. */
inline void prepare (UniformConvolver& c, int partition, const std::vector<std::vector<float>>& kernels)
{
    jassert (partition >= 16 && juce::isPowerOfTwo (partition));
    c.partition = partition;
    c.fftSize = 2 * partition;
    c.bins = partition + 1;
    c.numKernels = static_cast<int> (kernels.size());

    size_t longest = 1;
    for (const auto& h : kernels)
        longest = std::max (longest, h.size());
    c.numPartitions = static_cast<int> ((longest + static_cast<size_t> (partition) - 1) / static_cast<size_t> (partition));

    c.fft = std::make_unique<juce::dsp::FFT> (juce::roundToInt (std::log2 (c.fftSize)));
    c.work.assign (static_cast<size_t> (2 * c.fftSize), 0.0f);
    c.frame.assign (static_cast<size_t> (c.fftSize), 0.0f);
    c.accum.assign (static_cast<size_t> (c.bins), {});
    c.fdl.assign (static_cast<size_t> (c.numPartitions * c.bins), {});
    c.outputs.assign (static_cast<size_t> (c.numKernels * c.partition), 0.0f);
    c.fdlHead = 0;
    c.fill = 0;

    c.kernelSpectra.assign (kernels.size(), {});
    std::vector<float> segment (static_cast<size_t> (c.fftSize));
    for (size_t k = 0; k < kernels.size(); ++k)
    {
        auto& H = c.kernelSpectra[k];
        H.assign (static_cast<size_t> (c.numPartitions * c.bins), {});
        for (int p = 0; p < c.numPartitions; ++p)
        {
            std::fill (segment.begin(), segment.end(), 0.0f);
            const size_t start = static_cast<size_t> (p * partition);
            for (size_t i = 0; i < static_cast<size_t> (partition) && start + i < kernels[k].size(); ++i)
                segment[i] = kernels[k][start + i];
            convdetail::forwardInto (c, segment.data(), H.data() + static_cast<size_t> (p * c.bins));
        }
    }
}

inline int latencySamples (const UniformConvolver& c) noexcept { return c.partition; }
inline int numKernels (const UniformConvolver& c) noexcept { return c.numKernels; }

/** One input sample in, one output per kernel out (outs must hold numKernels floats).
 *  Unprepared: writes nothing useful but stays safe (outputs zero if any are requested). */
inline void process (UniformConvolver& c, float x, float* outs) noexcept
{
    if (c.fft == nullptr)
    {
        outs[0] = 0.0f;
        return;
    }

    c.frame[static_cast<size_t> (c.partition + c.fill)] = x;
    for (int k = 0; k < c.numKernels; ++k)
        outs[k] = c.outputs[static_cast<size_t> (k * c.partition + c.fill)];

    if (++c.fill == c.partition)
    {
        c.fill = 0;
        convdetail::computeBlock (c);
    }
}

inline void reset (UniformConvolver& c) noexcept
{
    std::fill (c.frame.begin(), c.frame.end(), 0.0f);
    std::fill (c.fdl.begin(), c.fdl.end(), std::complex<float> {});
    std::fill (c.outputs.begin(), c.outputs.end(), 0.0f);
    c.fill = 0;
    c.fdlHead = 0;
}

} // namespace strat3::dsp
