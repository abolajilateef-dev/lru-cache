// tests/test_lru.cpp
// GoogleTest suite for LruCache.
//
// The allocation-counting test overrides the GLOBAL operator new/delete. That
// is why this lives only in the test binary: we never want those overrides in
// the header or the benchmark.

#include "lru_cache.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <new>
#include <random>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

// ---------------------------------------------------------------------------
// Test peer: the single friend that can reach LruCache's private
// check_invariants(). Keeping the bridge here, in test code, means the
// production header exposes nothing extra.
// ---------------------------------------------------------------------------
namespace lru_test {
class Peer {
public:
    template <typename K, typename V>
    static void check(const LruCache<K, V>& c) {
        c.check_invariants();
    }
};
}  // namespace lru_test

// ---------------------------------------------------------------------------
// Global allocation counter. We only count operator new / operator delete
// calls. std::atomic because the concurrency test allocates on many threads;
// the allocation test itself is single-threaded.
//
// IMPORTANT: AddressSanitizer installs its own operator new/delete to track
// allocations. Replacing them here fights ASan's allocator and triggers a
// spurious alloc-dealloc-mismatch inside ASan/gtest internals (not our code).
// So under ASan we compile out the override and skip the allocation test; it
// still runs in the normal and TSan builds, which is where it matters.
// ---------------------------------------------------------------------------
#if defined(__SANITIZE_ADDRESS__)
#define LRU_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define LRU_ASAN 1
#endif
#endif

#ifndef LRU_ASAN
namespace {
std::atomic<uint64_t> g_new_count{0};
std::atomic<uint64_t> g_delete_count{0};
std::atomic<bool> g_count_enabled{false};  // only count inside the window we care about
}  // namespace

// We replace global new/delete with malloc/free. GCC's -Wmismatched-new-delete
// fires a false positive here because, after inlining, it pairs its *builtin*
// operator new with our std::free. Replacing global allocation this way is
// well-defined, so the warning is silenced narrowly around these definitions.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

void* operator new(std::size_t n) {
    if (g_count_enabled.load(std::memory_order_relaxed)) {
        g_new_count.fetch_add(1, std::memory_order_relaxed);
    }
    void* p = std::malloc(n ? n : 1);  // malloc(0) is implementation-defined; avoid it
    if (!p) throw std::bad_alloc();
    return p;
}

void operator delete(void* p) noexcept {
    if (g_count_enabled.load(std::memory_order_relaxed)) {
        g_delete_count.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}
// Sized delete (C++14+): the compiler may call this form instead of the
// unsized one. Route it to the same place so our counter is not bypassed.
void operator delete(void* p, std::size_t) noexcept {
    if (g_count_enabled.load(std::memory_order_relaxed)) {
        g_delete_count.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
#endif  // !LRU_ASAN

// ---------------------------------------------------------------------------
// Basic behavior
// ---------------------------------------------------------------------------

TEST(LruBasic, MissReturnsNullopt) {
    LruCache<uint64_t, uint64_t> c(2);
    EXPECT_FALSE(c.get(42).has_value());
}

TEST(LruBasic, PutThenGet) {
    LruCache<uint64_t, uint64_t> c(2);
    c.put(1, 100);
    auto v = c.get(1);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 100u);
    EXPECT_EQ(c.size(), 1u);
}

TEST(LruBasic, UpdateExistingKeyKeepsOneEntry) {
    LruCache<uint64_t, uint64_t> c(2);
    c.put(1, 100);
    c.put(1, 200);  // update, not insert
    EXPECT_EQ(c.size(), 1u);
    EXPECT_EQ(*c.get(1), 200u);
    lru_test::Peer::check(c);
}

TEST(LruBasic, CapacityOne) {
    LruCache<uint64_t, uint64_t> c(1);
    c.put(1, 10);
    c.put(2, 20);           // evicts key 1
    EXPECT_FALSE(c.get(1).has_value());
    EXPECT_EQ(*c.get(2), 20u);
    EXPECT_EQ(c.size(), 1u);
    lru_test::Peer::check(c);
}

TEST(LruEviction, EvictsTrueLru) {
    LruCache<uint64_t, uint64_t> c(2);
    c.put(1, 10);
    c.put(2, 20);
    c.put(3, 30);           // at capacity: evicts the LRU, which is key 1
    EXPECT_FALSE(c.get(1).has_value());
    EXPECT_TRUE(c.get(2).has_value());
    EXPECT_TRUE(c.get(3).has_value());
    lru_test::Peer::check(c);
}

TEST(LruEviction, GetRefreshesRecency) {
    LruCache<uint64_t, uint64_t> c(2);
    c.put(1, 10);
    c.put(2, 20);
    EXPECT_EQ(*c.get(1), 10u);  // touch 1 -> now 2 is the LRU
    c.put(3, 30);               // should evict 2, not 1
    EXPECT_TRUE(c.get(1).has_value());
    EXPECT_FALSE(c.get(2).has_value());
    EXPECT_TRUE(c.get(3).has_value());
    lru_test::Peer::check(c);
}

TEST(LruEviction, OrderOfMultipleEvictions) {
    LruCache<uint64_t, uint64_t> c(3);
    c.put(1, 1);
    c.put(2, 2);
    c.put(3, 3);
    c.put(4, 4);  // evict 1
    c.put(5, 5);  // evict 2
    EXPECT_FALSE(c.get(1).has_value());
    EXPECT_FALSE(c.get(2).has_value());
    EXPECT_TRUE(c.get(3).has_value());
    EXPECT_TRUE(c.get(4).has_value());
    EXPECT_TRUE(c.get(5).has_value());
    EXPECT_EQ(c.size(), 3u);
    lru_test::Peer::check(c);
}

// ---------------------------------------------------------------------------
// Randomized single-thread test against a reference model.
// The reference is a dead-simple LRU built from std::list<key> + map, written
// for obvious correctness rather than speed. We drive both with the same
// random ops and assert identical observable behavior, plus invariants.
// ---------------------------------------------------------------------------
namespace {
class ReferenceLru {
public:
    explicit ReferenceLru(size_t cap) : cap_(cap) {}

    std::optional<uint64_t> get(uint64_t k) {
        auto it = pos_.find(k);
        if (it == pos_.end()) return std::nullopt;
        order_.erase(it->second);          // remove from current position
        order_.push_front(k);              // reinsert at MRU
        it->second = order_.begin();
        return vals_[k];
    }

    void put(uint64_t k, uint64_t v) {
        auto it = pos_.find(k);
        if (it != pos_.end()) {
            vals_[k] = v;
            order_.erase(it->second);
            order_.push_front(k);
            it->second = order_.begin();
            return;
        }
        if (pos_.size() == cap_) {
            uint64_t victim = order_.back();  // LRU
            order_.pop_back();
            pos_.erase(victim);
            vals_.erase(victim);
        }
        order_.push_front(k);
        pos_[k] = order_.begin();
        vals_[k] = v;
    }

    size_t size() const { return pos_.size(); }

private:
    size_t cap_;
    std::list<uint64_t> order_;
    std::map<uint64_t, std::list<uint64_t>::iterator> pos_;
    std::map<uint64_t, uint64_t> vals_;
};
}  // namespace

TEST(LruRandomized, MatchesReferenceModel) {
    const size_t cap = 16;
    LruCache<uint64_t, uint64_t> c(cap);
    ReferenceLru ref(cap);

    std::mt19937_64 rng(12345);  // fixed seed -> reproducible failures
    std::uniform_int_distribution<uint64_t> key_dist(0, 40);  // small space -> collisions
    std::uniform_int_distribution<int> op_dist(0, 1);
    std::uniform_int_distribution<uint64_t> val_dist(0, 1'000'000);

    for (int i = 0; i < 100000; ++i) {
        uint64_t k = key_dist(rng);
        if (op_dist(rng) == 0) {
            auto a = c.get(k);
            auto b = ref.get(k);
            ASSERT_EQ(a.has_value(), b.has_value());
            if (a.has_value()) {
                ASSERT_EQ(*a, *b);
            }
        } else {
            uint64_t v = val_dist(rng);
            c.put(k, v);
            ref.put(k, v);
        }
        ASSERT_EQ(c.size(), ref.size());
        lru_test::Peer::check(c);  // invariants after EVERY operation
    }
}

// ---------------------------------------------------------------------------
// Allocation test: the eviction path must not touch the heap.
// Strategy: fill to capacity (allocations expected here), reset counter, then
// do N puts of brand-new keys (all evictions). Assert zero new/delete.
//
// Compiled out under ASan (see note at the global new/delete override).
// ---------------------------------------------------------------------------
#ifndef LRU_ASAN
TEST(LruAllocation, EvictionPathIsAllocationFree) {
    const size_t cap = 1024;
    LruCache<uint64_t, uint64_t> c(cap);

    // Fill to capacity. These puts DO allocate; we are not measuring yet.
    for (uint64_t k = 0; k < cap; ++k) c.put(k, k);

    // Measure only the eviction window.
    g_new_count.store(0);
    g_delete_count.store(0);
    g_count_enabled.store(true);

    const uint64_t N = 100000;
    for (uint64_t i = 0; i < N; ++i) {
        c.put(cap + i, i);  // every key is new and the cache is full -> evict
    }

    g_count_enabled.store(false);

    EXPECT_EQ(g_new_count.load(), 0u)
        << "eviction path performed " << g_new_count.load() << " allocations";
    EXPECT_EQ(g_delete_count.load(), 0u)
        << "eviction path performed " << g_delete_count.load() << " frees";
    EXPECT_EQ(c.size(), cap);
    lru_test::Peer::check(c);
}
#endif  // !LRU_ASAN

// ---------------------------------------------------------------------------
// Concurrency stress test. Thread count comes from an env var so the same
// binary can be run light in CI and heavy by hand. We cannot check behavior
// against a model under races; instead we assert the cache does not corrupt:
// invariants hold and size stays within capacity after all threads join.
// ---------------------------------------------------------------------------
TEST(LruConcurrency, StressStaysConsistent) {
    unsigned threads = 8;
    if (const char* env = std::getenv("LRU_STRESS_THREADS")) {
        threads = static_cast<unsigned>(std::max(1, std::atoi(env)));
    }
    const size_t cap = 256;
    const uint64_t key_space = 1024;
    const int ops_per_thread = 200000;

    LruCache<uint64_t, uint64_t> c(cap);

    auto worker = [&](unsigned seed) {
        std::mt19937_64 rng(seed);
        std::uniform_int_distribution<uint64_t> key_dist(0, key_space - 1);
        std::uniform_int_distribution<int> op_dist(0, 1);
        for (int i = 0; i < ops_per_thread; ++i) {
            uint64_t k = key_dist(rng);
            if (op_dist(rng) == 0) {
                (void)c.get(k);
            } else {
                c.put(k, k * 2);
            }
        }
    };

    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (unsigned t = 0; t < threads; ++t) pool.emplace_back(worker, t + 1);
    for (auto& th : pool) th.join();

    EXPECT_LE(c.size(), cap);
    lru_test::Peer::check(c);  // safe: single-threaded now, all joined
}
