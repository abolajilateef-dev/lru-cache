# LruCache — a C++17 LRU cache

A header-only, thread-safe LRU cache (`LruCache<K, V>`). The goal is clarity
over cleverness, with no features beyond spec.

## Build & run

Requires: Linux (or WSL2 Ubuntu), g++ or clang++, CMake >= 3.14, internet
(GoogleTest is fetched at configure time). Flags used everywhere:
`-std=c++17 -O2 -g`. `NDEBUG` is intentionally left undefined so
`assert()`-based invariant checks stay live.

### Normal build + tests

```bash
cd lru-cache
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
# or run the test binary directly:
./build/lru_tests
```

### Heavier concurrency stress (optional)

```bash
LRU_STRESS_THREADS=16 ./build/lru_tests --gtest_filter='LruConcurrency.*'
```

### ThreadSanitizer build (data-race detection)

```bash
cmake -S . -B build-tsan -DENABLE_TSAN=ON
cmake --build build-tsan -j
./build-tsan/lru_tests
```

### Address + UndefinedBehavior sanitizer build

```bash
cmake -S . -B build-asan -DENABLE_ASAN_UBSAN=ON
cmake --build build-asan -j
./build-asan/lru_tests
```

(TSan and ASan/UBSan are mutually exclusive; configuring both is rejected.)

> Note: the allocation-counting test (`LruAllocation.EvictionPathIsAllocationFree`)
> is compiled out under AddressSanitizer. ASan installs its own global
> `operator new`/`delete`; our counting override would fight it and raise a
> spurious `alloc-dealloc-mismatch` inside ASan/gtest internals. The test still
> runs in the normal and TSan builds, which is where allocation behavior matters.

### Run one benchmark

```bash
./build/lru_bench --threads 4 --read-pct 90 --keys 100000 \
    --capacity 10000 --duration-sec 5 --dist zipf --zipf-theta 0.99 \
    --runs 3 --seed 1
```

CSV goes to stdout; the human-readable summary goes to stderr.

### Run the full sweep

```bash
./run_sweep.sh > results.csv       # threads {1,2,4,8,nproc} x read {50,90} x {uniform,zipf}
```

## Design

The cache is the classic hash-map-plus-linked-list LRU:

- `std::list<Entry>` holds entries ordered **MRU at the front, LRU at the
  back**. A `std::list` is a doubly linked list, so moving a node to the front
  (`splice`) and removing the tail are both O(1) and leave every other node's
  address unchanged.
- `std::unordered_map<K, list::iterator>` maps a key to the iterator of its
  list node, giving O(1) lookup. Because `std::list` iterators stay valid
  across insertion/erasure/splice of *other* nodes, caching them is safe.
- `get` on a hit splices the node to the front and returns a copy of the value.
  On a miss it returns `std::nullopt`.
- `put` has four paths: update-existing (overwrite + splice), insert-when-room
  (`push_front` + map insert), and insert-when-full (evict + recycle).

### Node recycling (zero-allocation eviction)

When full, instead of `erase` + `new`, we reuse both nodes:

1. **List node**: overwrite the tail entry's key/value in place, then `splice`
   that same node to the front. No node is freed or created.
2. **Map node**: `unordered_map::extract()` detaches the old key's node handle
   *without deallocating it*, we mutate the (normally const) key and reset the
   mapped iterator, then `insert()` the same handle back. Net heap allocations
   on the eviction path: **zero** (verified by a test that overrides global
   `operator new`/`delete`).

### Thread safety

One `mutable std::mutex`, taken with `std::lock_guard` at the top of every
public method. We deliberately do **not** use `std::shared_mutex`: `get()` is a
*writer* because it splices the hit node to the front, so there is no
read-only path to optimize. Sharding and lock-free schemes for the cache's own
data structure are out of scope.

## Invariants

Checked by the private `check_invariants()` (exposed to tests via a single
friend `lru_test::Peer`):

- `map.size() == list.size()`
- `list.size() <= capacity`
- every map iterator points at a live list node whose key matches the map key
  (node identity, not just key equality).

The randomized single-thread test asserts these after **every** operation and
also compares observable behavior against an independent, deliberately simple
reference LRU.

## Tradeoffs

- **Single mutex vs sharding/shared_mutex.** Simple and obviously correct;
  becomes the bottleneck under high thread counts. Measuring that is the point
  of the benchmark. `shared_mutex` would not help because `get` mutates.
- **Node recycling vs plain erase/insert.** Recycling removes allocator traffic
  from the eviction hot path at the cost of more intricate code (`extract`,
  mutable key). Justified only because eviction is the steady-state path.
- **Value is copied out of `get`.** Returning a reference would be faster but
  unsafe once the lock is released. Copy keeps the API race-free.
- **Benchmark key buffer is fixed-length and wraps.** Keeps RNG out of the
  timed section at the cost of key repetition after ~1M ops per thread.
- **Deadline/latency sampling are coarse** (every 1024 / every 64 ops) to keep
  clock reads off the hot path; slightly inflates run length, corrected by
  dividing by measured wall time.

## Results

Not yet measured.

| threads | read_pct | dist    | median ops/sec | p50 (ns) | p99 (ns) |
|---------|----------|---------|----------------|----------|----------|
|         |          | uniform |                |          |          |
|         |          | zipf    |                |          |          |

## Machine specs

- CPU:
- Cores / threads:
- RAM:
- OS / kernel:
- Compiler + version:
- CMake version:
