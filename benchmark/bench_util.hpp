#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>

// A tiny dependency-free benchmark harness. (Google Benchmark is the
// canonical choice, but it fails to compile against this MinGW distro's
// COM/OLE headers; this harness measures the same throughput/latency
// figures and builds cleanly everywhere.)
namespace pulsedb::bench {

using Clock = std::chrono::steady_clock;

/// Prevent the optimiser from discarding a computed value.
template <typename T>
inline void keep(const T& value) {
    asm volatile("" : : "m"(value) : "memory");
}

/// Time a callable, returning elapsed wall-clock seconds.
template <typename Fn>
double time_it(Fn&& fn) {
    const auto start = Clock::now();
    fn();
    const auto end = Clock::now();
    return std::chrono::duration<double>(end - start).count();
}

/// Print one result row: throughput in items/s (+ optional MB/s) and the
/// per-item latency in nanoseconds.
inline void report(const char* name, long long items, double seconds, long long bytes = 0) {
    const double items_per_s = seconds > 0.0 ? static_cast<double>(items) / seconds : 0.0;
    const double ns_per_item = items > 0 ? seconds * 1e9 / static_cast<double>(items) : 0.0;
    std::printf("  %-32s %11lld items  %7.3fs  %13.0f items/s  %8.1f ns/item", name, items,
                seconds, items_per_s, ns_per_item);
    if (bytes > 0) {
        std::printf("  %8.1f MB/s", static_cast<double>(bytes) / 1e6 / seconds);
    }
    std::printf("\n");
}

inline void header(const char* title) {
    std::printf("\n== %s ==\n", title);
}

}  // namespace pulsedb::bench
