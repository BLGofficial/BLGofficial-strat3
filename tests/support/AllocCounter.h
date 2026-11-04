#pragma once
// Counts heap allocations made on the calling thread while a ScopedAllocCounter is alive.
// The replacement global operator new lives in tests/support/alloc_counter.cpp; it only
// counts (never fails), so it is harmless to every other test in the binary.
#include <cstddef>

namespace strat3::test {

std::size_t& threadAllocCount() noexcept;
bool& threadAllocCounting() noexcept;

struct ScopedAllocCounter
{
    ScopedAllocCounter() noexcept { threadAllocCount() = 0; threadAllocCounting() = true; }
    ~ScopedAllocCounter() { threadAllocCounting() = false; }
    std::size_t count() const noexcept { return threadAllocCount(); }
    ScopedAllocCounter (const ScopedAllocCounter&) = delete;
    ScopedAllocCounter& operator= (const ScopedAllocCounter&) = delete;
};

} // namespace strat3::test
