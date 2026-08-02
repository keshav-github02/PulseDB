#pragma once

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <vector>

#include <nlohmann/json.hpp>

namespace pulsedb::sdk {

/// On-disk spool of event batches that could not be delivered.
///
/// Each batch is written to its own JSON file named `batch-NNNNNNNN.json` and
/// ordered by the number in that name, so oldest-first holds regardless of how
/// many digits the index needs. Writes are atomic
/// (temp file + rename) and flushed to stable storage, so neither a crash nor a
/// full disk can leave a partial batch that later looks valid.
/// Survives process restarts: a new store continues the numbering from the
/// files already present.
///
/// The spool is bounded. An unbounded spool turns a long collector outage into
/// a full disk on the client -- trading one outage for a worse one -- so once
/// max_batches is reached the oldest batch is evicted to make room. Losing the
/// oldest data is the right trade: the newest telemetry is the most valuable,
/// and bounded loss beats filling the host's disk.
class SpoolStore {
public:
    /// @param dir         directory holding the spooled batches.
    /// @param max_batches retained batches before the oldest is evicted.
    explicit SpoolStore(std::filesystem::path dir, std::size_t max_batches = 1'000);

    /// Persist a batch, evicting the oldest if the spool is full.
    /// @return the file written, or an empty path if the write failed.
    [[nodiscard]] std::filesystem::path save(const nlohmann::json& events);

    /// Batches evicted because the spool was full.
    std::size_t evicted_count() const noexcept {
        return evicted_.load(std::memory_order_relaxed);
    }

    /// Retention bound.
    std::size_t max_batches() const noexcept { return max_batches_; }

    /// Spooled batch files, oldest-first.
    [[nodiscard]] std::vector<std::filesystem::path> list() const;

    /// Load a spooled batch. Throws nlohmann::json::exception on corruption.
    [[nodiscard]] nlohmann::json load(const std::filesystem::path& file) const;

    /// Delete a spooled batch (e.g. after a successful replay).
    void remove(const std::filesystem::path& file);

    /// Number of spooled batches.
    [[nodiscard]] std::size_t count() const;

    const std::filesystem::path& dir() const noexcept { return dir_; }

private:
    // Caller must hold mutex_.
    std::vector<std::filesystem::path> list_locked() const;

    std::filesystem::path dir_;
    std::size_t max_batches_;
    mutable std::mutex mutex_;
    unsigned long long next_index_;
    /// Atomic because evicted_count() reads it without taking mutex_, while
    /// save() writes it holding the lock -- a plain size_t there is a data race
    /// on a class that otherwise presents as thread-safe.
    std::atomic<std::size_t> evicted_{0};
};

}  // namespace pulsedb::sdk
