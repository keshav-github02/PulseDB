#pragma once

#include <cstddef>
#include <cstdint>

namespace pulsedb::query {

/// Samples this process's CPU and memory usage (Windows + Linux).
///
/// Not thread-safe: call cpu_percent() from a single sampler thread. It
/// reports CPU used since the previous call, where 100% means one core
/// fully utilized (can exceed 100% for a multi-threaded process).
class SystemStats {
public:
    SystemStats() = default;

    /// Process CPU percentage since the previous call (0 on the first call).
    ///
    /// Returns 0 when the underlying counter could not be read, rather than a
    /// number derived from an unusable sample.
    double cpu_percent();

    /// Resident memory of this process, in bytes.
    static std::size_t memory_bytes();

    /// CPU percentage between two samples, or 0 if they cannot yield one.
    ///
    /// Pure, and public so the guard can be tested directly: the interesting
    /// inputs (a counter that went backwards, a zero-width interval) are the
    /// ones a live process will not reproduce on demand.
    static double percent_from_samples(std::uint64_t cpu_ns, std::uint64_t wall_ns,
                                       std::uint64_t last_cpu_ns,
                                       std::uint64_t last_wall_ns);

private:
    std::uint64_t last_cpu_ns_ = 0;
    std::uint64_t last_wall_ns_ = 0;
};

}  // namespace pulsedb::query
