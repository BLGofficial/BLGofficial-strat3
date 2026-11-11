#pragma once
#include <cstdint>

namespace strat3::dsp {

/** What the host says about the block: from its play head. */
struct Transport
{
    bool playing = false;
    bool hasPosition = false;
    std::int64_t sample = 0;   // timeline position of the block's first sample
};

} // namespace strat3::dsp
