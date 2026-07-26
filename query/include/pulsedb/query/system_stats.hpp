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
    double cpu_percent();

    /// Resident memory of this process, in bytes.
    static std::size_t memory_bytes();

private:
    std::uint64_t last_cpu_ns_ = 0;
    std::uint64_t last_wall_ns_ = 0;
};

}  // namespace pulsedb::query
