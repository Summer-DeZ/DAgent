#define BOOST_TEST_MODULE dagent_tui
#include <boost/test/included/unit_test.hpp>

#include <cstdlib>
#include <new>

#include "support.hpp"

namespace test_support {
std::atomic<long> g_alloc_count{0};
thread_local bool g_alloc_counting = false;
} // namespace test_support

// 替换全局分配函数，用于断言"稳态帧零分配"。
void* operator new(std::size_t n) {
    if (test_support::g_alloc_counting) ++test_support::g_alloc_count;
    if (void* p = std::malloc(n == 0 ? 1 : n)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
