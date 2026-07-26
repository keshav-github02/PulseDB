#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <utility>

namespace pulsedb::queue {

/// A bounded, thread-safe FIFO queue for producer-consumer pipelines.
///
/// This is the decoupling point between ingestion (the Collector) and
/// processing (the Worker Pool). The bounded capacity provides
/// backpressure: producers block in push() while the queue is full, and
/// consumers block in pop() while it is empty.
///
/// Shutdown is cooperative via close(): after closing, every pending and
/// future push() fails, while pop() keeps draining the items already in
/// the queue and then returns std::nullopt. A consumer loop is therefore
/// simply:
///
///     while (auto item = queue.pop()) {
///         process(*item);
///     }
///
/// All member functions are safe to call from multiple threads
/// concurrently.
template <typename T>
class BoundedBlockingQueue {
public:
    /// @throws std::invalid_argument if capacity is zero.
    explicit BoundedBlockingQueue(std::size_t capacity) : capacity_(capacity) {
        if (capacity_ == 0) {
            throw std::invalid_argument("BoundedBlockingQueue capacity must be greater than zero");
        }
    }

    BoundedBlockingQueue(const BoundedBlockingQueue&) = delete;
    BoundedBlockingQueue& operator=(const BoundedBlockingQueue&) = delete;

    /// Adds an item, blocking while the queue is full.
    /// @return true on success, false if the queue was closed before the
    ///         item could be added (the item is dropped).
    ///
    /// [[nodiscard]] because ignoring the result silently loses the item.
    [[nodiscard]] bool push(T value) {
        std::unique_lock lock(mutex_);
        not_full_.wait(lock, [this] { return items_.size() < capacity_ || closed_; });
        if (closed_) {
            return false;
        }
        items_.push(std::move(value));
        lock.unlock();
        not_empty_.notify_one();
        return true;
    }

    /// Adds an item without blocking.
    /// @return false if the queue is full or closed.
    ///
    /// [[nodiscard]] because ignoring the result silently drops the item --
    /// which for an ingest queue means silently losing telemetry.
    [[nodiscard]] bool try_push(T value) {
        {
            std::lock_guard lock(mutex_);
            if (closed_ || items_.size() >= capacity_) {
                return false;
            }
            items_.push(std::move(value));
        }
        not_empty_.notify_one();
        return true;
    }

    /// Removes the oldest item, blocking while the queue is empty.
    /// @return the item, or std::nullopt once the queue is closed and
    ///         fully drained.
    [[nodiscard]] std::optional<T> pop() {
        std::unique_lock lock(mutex_);
        not_empty_.wait(lock, [this] { return !items_.empty() || closed_; });
        if (items_.empty()) {
            return std::nullopt;
        }
        std::optional<T> value{std::move(items_.front())};
        items_.pop();
        lock.unlock();
        not_full_.notify_one();
        return value;
    }

    /// Removes the oldest item without blocking.
    /// @return the item, or std::nullopt if the queue is empty.
    [[nodiscard]] std::optional<T> try_pop() {
        std::optional<T> value;
        {
            std::lock_guard lock(mutex_);
            if (items_.empty()) {
                return std::nullopt;
            }
            value.emplace(std::move(items_.front()));
            items_.pop();
        }
        not_full_.notify_one();
        return value;
    }

    /// Closes the queue and wakes every blocked producer and consumer.
    /// Idempotent. Items already queued remain available to pop().
    void close() {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    /// @return true once close() has been called.
    [[nodiscard]] bool closed() const {
        std::lock_guard lock(mutex_);
        return closed_;
    }

    /// @return the number of items currently queued.
    [[nodiscard]] std::size_t size() const {
        std::lock_guard lock(mutex_);
        return items_.size();
    }

    /// @return true if no items are currently queued.
    [[nodiscard]] bool empty() const {
        std::lock_guard lock(mutex_);
        return items_.empty();
    }

    /// @return the maximum number of items the queue can hold.
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable not_full_;
    std::condition_variable not_empty_;
    std::queue<T> items_;
    bool closed_ = false;
};

}  // namespace pulsedb::queue
