#pragma once

#include <cstddef>
#include <mutex>
#include <unordered_map>

#include "pulsedb/storage/minute_key.hpp"

namespace pulsedb::storage {

/// An in-memory time-series store: a nested
/// Year -> Month -> Day -> Hour -> Minute map of buckets, giving O(1)
/// point lookup (a fixed five hash-map hops).
///
/// @tparam Bucket the per-minute value type. Buckets are default-constructed
///         in place and never moved or erased, so a reference returned by
///         get_or_create() stays valid for the lifetime of the store even as
///         other buckets are inserted concurrently (std::unordered_map keeps
///         element addresses stable across rehash). When Bucket updates via
///         atomics, callers may mutate it without holding any store lock.
///
/// The store's mutex guards only the map structure; bucket contents update
/// through atomics, so callers mutate a bucket without holding it.
///
/// This is deliberately a plain std::mutex and not a std::shared_mutex, even
/// though the access pattern (many readers, a writer once per minute) is
/// exactly what a shared_mutex is for. On MinGW/winpthreads,
/// std::shared_mutex intermittently fails to acquire: libstdc++ forwards
/// lock_shared() to pthread_rwlock_rdlock(), which returns an error under
/// sustained reader concurrency. With assertions enabled that aborts; without
/// them std::shared_lock proceeds *without holding the lock*, so readers run
/// concurrently with an inserting writer and corrupt the maps.
///
/// Measured on this toolchain with an otherwise identical store, 8 threads x
/// 20k get_or_create calls, 40 runs each: shared_mutex produced wrong bucket
/// counts, lost increments, or a STATUS_HEAP_CORRUPTION exit in 7 of 40 runs;
/// std::mutex in 0 of 40. Linux/glibc is unaffected, which is why CI never
/// caught it.
///
/// The cost is losing read parallelism. That is acceptable here: writes are
/// the hot path, reads are infrequent (a dashboard poll), and for_each already
/// serialised against bucket creation regardless. Restoring read scalability
/// belongs with the flat-ring redesign, not with a primitive this toolchain
/// cannot honour.
template <typename Bucket>
class TimeSeriesStore {
public:
    /// Return the bucket for @p key, creating it (and any missing calendar
    /// nodes) if absent. The returned reference is stable.
    Bucket& get_or_create(const MinuteKey& key) {
        std::lock_guard lock(mutex_);
        if (Bucket* existing = find_locked(key)) {
            return *existing;
        }
        return years_[key.year][key.month][key.day][key.hour]
            .try_emplace(key.minute)
            .first->second;
    }

    /// Invoke @p fn(const Bucket&) if a bucket exists for @p key.
    /// @return true if the bucket existed.
    template <typename Fn>
    bool with_bucket(const MinuteKey& key, Fn&& fn) const {
        std::lock_guard lock(mutex_);
        if (const Bucket* bucket = find_locked(key)) {
            fn(*bucket);
            return true;
        }
        return false;
    }

    /// Invoke @p fn(const MinuteKey&, const Bucket&) for every bucket, under
    /// the store lock. Order is unspecified.
    template <typename Fn>
    void for_each(Fn&& fn) const {
        std::lock_guard lock(mutex_);
        for (const auto& [year, months] : years_) {
            for (const auto& [month, days] : months) {
                for (const auto& [day, hours] : days) {
                    for (const auto& [hour, minutes] : hours) {
                        for (const auto& [minute, bucket] : minutes) {
                            fn(MinuteKey{year, month, day, hour, minute}, bucket);
                        }
                    }
                }
            }
        }
    }

    /// Number of distinct minute buckets currently stored.
    std::size_t minute_count() const {
        std::size_t count = 0;
        for_each([&count](const MinuteKey&, const Bucket&) { ++count; });
        return count;
    }

private:
    using MinuteMap = std::unordered_map<int, Bucket>;
    using HourMap = std::unordered_map<int, MinuteMap>;
    using DayMap = std::unordered_map<int, HourMap>;
    using MonthMap = std::unordered_map<int, DayMap>;
    using YearMap = std::unordered_map<int, MonthMap>;

    // Caller must hold at least a shared lock. Returns nullptr if absent.
    const Bucket* find_locked(const MinuteKey& key) const {
        const auto year = years_.find(key.year);
        if (year == years_.end()) return nullptr;
        const auto month = year->second.find(key.month);
        if (month == year->second.end()) return nullptr;
        const auto day = month->second.find(key.day);
        if (day == month->second.end()) return nullptr;
        const auto hour = day->second.find(key.hour);
        if (hour == day->second.end()) return nullptr;
        const auto minute = hour->second.find(key.minute);
        if (minute == hour->second.end()) return nullptr;
        return &minute->second;
    }

    Bucket* find_locked(const MinuteKey& key) {
        const auto* self = this;
        return const_cast<Bucket*>(self->find_locked(key));
    }

    mutable std::mutex mutex_;
    YearMap years_;
};

}  // namespace pulsedb::storage
