// bench/bench.cpp
// Standalone throughput/latency benchmark for LruCache. No Google Benchmark.
//
// What it measures: aggregate ops/sec and per-op latency percentiles under a
// configurable read/write mix, key distribution, thread count, and duration.
//
// Key design decisions (and why), since the spec cares about measurement hygiene:
//   * Each thread PRE-GENERATES its entire op sequence (key + read/write bit)
//     before timing starts, so RNG cost is outside the measured window. A
//     duration-driven loop needs more ops than we can know in advance, so we
//     generate a fixed power-of-two buffer per thread and wrap around it with a
//     bitmask (cheap, branchless). This means keys repeat after the buffer
//     length, which is fine: the buffer is large and the distribution is
//     preserved.
//   * All threads wait on a hand-rolled barrier (C++17 has no std::barrier) so
//     they start the measured section together.
//   * Latency is sampled on every 64th op only, to keep clock reads off the
//     hot path. The deadline is checked every 1024 ops for the same reason;
//     this makes the actual run slightly longer than S seconds, which we
//     correct for by dividing by the real elapsed time, not by S.

#include "lru_cache.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

// ---- Hand-rolled barrier (C++17 lacks std::barrier) ----------------------
// Blocks until `count` threads have arrived, then releases them all at once.
class Barrier {
public:
    explicit Barrier(unsigned count) : threshold_(count), waiting_(count), generation_(0) {}
    void arrive_and_wait() {
        std::unique_lock<std::mutex> lk(m_);
        unsigned gen = generation_;
        if (--waiting_ == 0) {
            // Last thread in: reset for reuse and wake everyone.
            generation_++;
            waiting_ = threshold_;
            cv_.notify_all();
        } else {
            // Wait until the generation advances (guards against spurious wakeups).
            cv_.wait(lk, [this, gen] { return gen != generation_; });
        }
    }
private:
    std::mutex m_;
    std::condition_variable cv_;
    unsigned threshold_;
    unsigned waiting_;
    unsigned generation_;
};

// ---- Config ---------------------------------------------------------------
struct Config {
    unsigned threads = 1;
    int read_pct = 50;          // 0..100
    uint64_t keys = 100000;     // key space size
    size_t capacity = 10000;
    int duration_sec = 5;
    std::string dist = "uniform";  // "uniform" | "zipf"
    double zipf_theta = 0.99;
    int runs = 3;
    uint64_t seed = 1;
};

// ---- Zipf generator (Gray et al. "quickly generating billion-record..." ) --
// Produces skewed keys in [0, n). theta in [0,1); higher = more skew.
class Zipf {
public:
    Zipf(uint64_t n, double theta, std::mt19937_64& rng) : n_(n), rng_(rng) {
        // Precompute the normalization constants once; sampling is then O(1).
        zetan_ = zeta(n_, theta);
        theta_ = theta;
        alpha_ = 1.0 / (1.0 - theta_);
        eta_ = (1.0 - std::pow(2.0 / static_cast<double>(n_), 1.0 - theta_)) /
               (1.0 - zeta(2, theta) / zetan_);
    }
    uint64_t next() {
        std::uniform_real_distribution<double> u(0.0, 1.0);
        double uu = u(rng_);
        double uz = uu * zetan_;
        if (uz < 1.0) return 0;
        if (uz < 1.0 + std::pow(0.5, theta_)) return 1;
        return static_cast<uint64_t>(
            static_cast<double>(n_) * std::pow(eta_ * uu - eta_ + 1.0, alpha_));
    }
private:
    static double zeta(uint64_t n, double theta) {
        double sum = 0.0;
        for (uint64_t i = 1; i <= n; ++i) sum += 1.0 / std::pow(static_cast<double>(i), theta);
        return sum;
    }
    uint64_t n_;
    std::mt19937_64& rng_;
    double theta_, zetan_, alpha_, eta_;
};

// One pre-generated op: which key, and whether it is a read.
struct Op {
    uint64_t key;
    bool is_read;
};

// Pre-generate a thread's op buffer. Size is a power of two so the hot loop can
// wrap with & (len-1) instead of %.
std::vector<Op> make_ops(const Config& cfg, uint64_t seed, size_t len_pow2) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> pct(1, 100);
    std::uniform_int_distribution<uint64_t> uni(0, cfg.keys - 1);

    std::vector<Op> ops;
    ops.reserve(len_pow2);

    if (cfg.dist == "zipf") {
        Zipf z(cfg.keys, cfg.zipf_theta, rng);
        for (size_t i = 0; i < len_pow2; ++i) {
            uint64_t k = z.next();
            if (k >= cfg.keys) k = cfg.keys - 1;  // clamp numeric edge cases
            ops.push_back(Op{k, pct(rng) <= cfg.read_pct});
        }
    } else {
        for (size_t i = 0; i < len_pow2; ++i) {
            ops.push_back(Op{uni(rng), pct(rng) <= cfg.read_pct});
        }
    }
    return ops;
}

size_t next_pow2(size_t x) {
    size_t p = 1;
    while (p < x) p <<= 1;
    return p;
}

// Per-thread result from the measured section.
struct ThreadResult {
    uint64_t ops = 0;
    std::vector<uint64_t> latencies_ns;  // sampled latencies
};

double percentile(std::vector<uint64_t>& v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    // Nearest-rank method; clamp index to valid range.
    size_t idx = static_cast<size_t>(p / 100.0 * (v.size() - 1));
    return static_cast<double>(v[idx]);
}

// Run one measurement (warmup + measure) and return median-free raw numbers.
struct RunResult {
    double ops_per_sec;
    double p50_ns;
    double p99_ns;
};

RunResult run_once(const Config& cfg) {
    using clock = std::chrono::steady_clock;

    LruCache<uint64_t, uint64_t> cache(cfg.capacity);

    // Prefill so steady-state eviction behavior dominates from op 1.
    for (uint64_t k = 0; k < cfg.capacity && k < cfg.keys; ++k) cache.put(k, k);

    // Pre-generate each thread's op buffer BEFORE timing. Buffer length is
    // generous so we rarely exhaust distinct randomness within a run.
    const size_t buf_len = next_pow2(1u << 20);  // ~1M ops per thread
    std::vector<std::vector<Op>> thread_ops(cfg.threads);
    for (unsigned t = 0; t < cfg.threads; ++t) {
        thread_ops[t] = make_ops(cfg, cfg.seed * 1000 + t, buf_len);
    }
    const size_t mask = buf_len - 1;

    Barrier start_barrier(cfg.threads + 1);  // +1 for the coordinator (this thread)
    // Two independent flags so the warmup stop signal can never be mistaken for
    // the measured-run stop signal (which caused a race when one flag was
    // toggled true->false quickly).
    std::atomic<bool> warmup_stop{false};
    std::atomic<bool> measure_stop{false};
    std::vector<ThreadResult> results(cfg.threads);

    auto worker = [&](unsigned tid) {
        const std::vector<Op>& ops = thread_ops[tid];
        ThreadResult& res = results[tid];
        res.latencies_ns.reserve(1u << 16);

        // ---- Warmup: run until the coordinator sets warmup_stop. Measurements
        // here are discarded; the point is to reach steady state (caches warm,
        // allocator settled) before we start timing. ----
        start_barrier.arrive_and_wait();  // phase 1: begin warmup together

        uint64_t i = 0;
        while (!warmup_stop.load(std::memory_order_relaxed)) {
            const Op& op = ops[i & mask];
            if (op.is_read) (void)cache.get(op.key);
            else cache.put(op.key, op.key);
            ++i;
            if ((i & 1023) == 0 && warmup_stop.load(std::memory_order_relaxed)) break;
        }

        start_barrier.arrive_and_wait();  // phase 2: begin measured run together

        uint64_t count = 0;
        uint64_t sample_tick = 0;
        while (!measure_stop.load(std::memory_order_relaxed)) {
            const Op& op = ops[(i + count) & mask];

            // Sample latency on every 64th op only.
            if ((sample_tick & 63) == 0) {
                auto t0 = clock::now();
                if (op.is_read) (void)cache.get(op.key);
                else cache.put(op.key, op.key);
                auto t1 = clock::now();
                res.latencies_ns.push_back(
                    static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
            } else {
                if (op.is_read) (void)cache.get(op.key);
                else cache.put(op.key, op.key);
            }
            ++sample_tick;
            ++count;

            // Check the deadline only every 1024 ops to keep clock reads cheap.
            if ((count & 1023) == 0 && measure_stop.load(std::memory_order_relaxed)) break;
        }
        res.ops = count;
    };

    std::vector<std::thread> pool;
    pool.reserve(cfg.threads);
    for (unsigned t = 0; t < cfg.threads; ++t) pool.emplace_back(worker, t);

    // ---- Warmup phase (coordinator) ----
    start_barrier.arrive_and_wait();  // release workers into warmup
    std::this_thread::sleep_for(std::chrono::seconds(1));
    warmup_stop.store(true, std::memory_order_relaxed);

    // ---- Measured phase (coordinator) ----
    auto measure_start = clock::now();
    start_barrier.arrive_and_wait();  // release workers into measured run

    std::this_thread::sleep_for(std::chrono::seconds(cfg.duration_sec));
    measure_stop.store(true, std::memory_order_relaxed);
    for (auto& th : pool) th.join();
    auto measure_end = clock::now();

    double elapsed_sec =
        std::chrono::duration_cast<std::chrono::duration<double>>(measure_end - measure_start)
            .count();

    uint64_t total_ops = 0;
    std::vector<uint64_t> all_latencies;
    for (auto& r : results) {
        total_ops += r.ops;
        all_latencies.insert(all_latencies.end(), r.latencies_ns.begin(), r.latencies_ns.end());
    }

    RunResult rr;
    rr.ops_per_sec = elapsed_sec > 0 ? static_cast<double>(total_ops) / elapsed_sec : 0.0;
    rr.p50_ns = percentile(all_latencies, 50.0);
    rr.p99_ns = percentile(all_latencies, 99.0);
    return rr;
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    if (v.empty()) return 0.0;
    return v[v.size() / 2];
}

void usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s [options]\n"
        "  --threads N        worker threads (default 1)\n"
        "  --read-pct P       read percentage 0-100 (default 50)\n"
        "  --keys K           key space size (default 100000)\n"
        "  --capacity C       cache capacity (default 10000)\n"
        "  --duration-sec S   measured seconds per run (default 5)\n"
        "  --dist D           uniform|zipf (default uniform)\n"
        "  --zipf-theta T     zipf skew (default 0.99)\n"
        "  --runs R           repeat count; median reported (default 3)\n"
        "  --seed S           RNG seed (default 1)\n",
        prog);
}

bool parse_args(int argc, char** argv, Config& cfg) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--threads") { auto v = need("--threads"); if (!v) return false; cfg.threads = std::max(1, std::atoi(v)); }
        else if (a == "--read-pct") { auto v = need("--read-pct"); if (!v) return false; cfg.read_pct = std::atoi(v); }
        else if (a == "--keys") { auto v = need("--keys"); if (!v) return false; cfg.keys = std::strtoull(v, nullptr, 10); }
        else if (a == "--capacity") { auto v = need("--capacity"); if (!v) return false; cfg.capacity = std::strtoull(v, nullptr, 10); }
        else if (a == "--duration-sec") { auto v = need("--duration-sec"); if (!v) return false; cfg.duration_sec = std::atoi(v); }
        else if (a == "--dist") { auto v = need("--dist"); if (!v) return false; cfg.dist = v; }
        else if (a == "--zipf-theta") { auto v = need("--zipf-theta"); if (!v) return false; cfg.zipf_theta = std::atof(v); }
        else if (a == "--runs") { auto v = need("--runs"); if (!v) return false; cfg.runs = std::max(1, std::atoi(v)); }
        else if (a == "--seed") { auto v = need("--seed"); if (!v) return false; cfg.seed = std::strtoull(v, nullptr, 10); }
        else if (a == "--help" || a == "-h") { usage(argv[0]); return false; }
        else { std::fprintf(stderr, "unknown arg: %s\n", a.c_str()); usage(argv[0]); return false; }
    }
    if (cfg.read_pct < 0 || cfg.read_pct > 100) {
        std::fprintf(stderr, "--read-pct must be 0..100\n");
        return false;
    }
    if (cfg.dist != "uniform" && cfg.dist != "zipf") {
        std::fprintf(stderr, "--dist must be uniform or zipf\n");
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg;
    if (!parse_args(argc, argv, cfg)) return 2;

    std::vector<double> ops_samples;
    std::vector<double> p50_samples;
    std::vector<double> p99_samples;
    for (int r = 0; r < cfg.runs; ++r) {
        RunResult rr = run_once(cfg);
        ops_samples.push_back(rr.ops_per_sec);
        p50_samples.push_back(rr.p50_ns);
        p99_samples.push_back(rr.p99_ns);
    }

    double med_ops = median(ops_samples);
    double med_p50 = median(p50_samples);
    double med_p99 = median(p99_samples);

    // Human-readable summary (to stderr so CSV on stdout stays clean).
    std::fprintf(stderr,
        "threads=%u read_pct=%d dist=%s keys=%llu capacity=%zu duration=%ds runs=%d\n"
        "  median ops/sec = %.0f\n"
        "  p50 latency    = %.0f ns\n"
        "  p99 latency    = %.0f ns\n",
        cfg.threads, cfg.read_pct, cfg.dist.c_str(),
        (unsigned long long)cfg.keys, cfg.capacity, cfg.duration_sec, cfg.runs,
        med_ops, med_p50, med_p99);

    // CSV line (stdout): six columns.
    // threads,read_pct,dist,median_ops_sec,p50_ns,p99_ns
    std::printf("%u,%d,%s,%.0f,%.0f,%.0f\n",
        cfg.threads, cfg.read_pct, cfg.dist.c_str(), med_ops, med_p50, med_p99);
    return 0;
}
