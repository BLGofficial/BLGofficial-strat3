// Global operator new replacement for the real-time "no allocation" tests. Thread-local
// flags: only allocations on the thread under test are counted.
#include <algorithm>
#include <cstdlib>
#include <new>

#include "support/AllocCounter.h"

namespace strat3::test {
std::size_t& threadAllocCount() noexcept { thread_local std::size_t n = 0; return n; }
bool& threadAllocCounting() noexcept { thread_local bool on = false; return on; }
} // namespace strat3::test

namespace {
void* countedAlloc (std::size_t size)
{
    if (strat3::test::threadAllocCounting())
        ++strat3::test::threadAllocCount();
    if (void* p = std::malloc (size == 0 ? 1 : size))
        return p;
    throw std::bad_alloc();
}
} // namespace

void* operator new (std::size_t size) { return countedAlloc (size); }
void* operator new[] (std::size_t size) { return countedAlloc (size); }
void operator delete (void* p) noexcept { std::free (p); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { std::free (p); }

// Over-aligned types (alignas > 16) get the aligned forms; replacing only the plain ones lets
// the library's aligned new and our free() disagree about the block.
namespace {
void* countedAlignedAlloc (std::size_t size, std::align_val_t al)
{
    if (strat3::test::threadAllocCounting())
        ++strat3::test::threadAllocCount();
    const auto a = static_cast<std::size_t> (al);
    const std::size_t rounded = (std::max<std::size_t> (size, 1) + a - 1) / a * a;
    if (void* p = std::aligned_alloc (a, rounded))
        return p;
    throw std::bad_alloc();
}
} // namespace

void* operator new (std::size_t size, std::align_val_t al) { return countedAlignedAlloc (size, al); }
void* operator new[] (std::size_t size, std::align_val_t al) { return countedAlignedAlloc (size, al); }
void operator delete (void* p, std::align_val_t) noexcept { std::free (p); }
void operator delete[] (void* p, std::align_val_t) noexcept { std::free (p); }
void operator delete (void* p, std::size_t, std::align_val_t) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t, std::align_val_t) noexcept { std::free (p); }
