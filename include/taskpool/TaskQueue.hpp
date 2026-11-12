// SPDX-License-Identifier: MIT
//
// header-only-task-pool
// TaskQueue: an unbounded, closable, thread-safe FIFO queue.
//
// Built on `std::queue` + `std::mutex` + `std::condition_variable`. Consumers
// block in `wait_pop()` until an item arrives or the queue is closed. Closing
// is a one-way transition: producers are rejected afterwards, while consumers
// keep draining the remaining items before they observe the end of the stream.

#ifndef TASKPOOL_TASK_QUEUE_HPP
#define TASKPOOL_TASK_QUEUE_HPP

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <queue>
#include <utility>

namespace taskpool {

template <typename T>
class TaskQueue {
public:
    using value_type = T;
    using size_type = std::size_t;

    TaskQueue() = default;
    ~TaskQueue() = default;

    // A queue owns synchronization primitives; it is pinned in memory.
    TaskQueue(const TaskQueue&) = delete;
    TaskQueue& operator=(const TaskQueue&) = delete;
    TaskQueue(TaskQueue&&) = delete;
    TaskQueue& operator=(TaskQueue&&) = delete;

    /// Moves `item` into the queue and wakes one waiting consumer.
    /// Returns false (leaving `item` untouched) if the queue has been closed.
    [[nodiscard]] bool push(T&& item) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) {
                return false;
            }
            queue_.push(std::move(item));
        }
        // Notify outside the lock so the woken consumer does not immediately
        // block on a mutex we still hold.
        not_empty_.notify_one();
        return true;
    }

    /// Copies `item` into the queue. Only available for copyable `T`.
    [[nodiscard]] bool push(const T& item) {
        T copy(item);
        return push(std::move(copy));
    }

    /// Blocks until an item is available or the queue is closed and drained.
    /// Returns std::nullopt only when no more items will ever arrive.
    [[nodiscard]] std::optional<T> wait_pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait(lock, [this] { return closed_ || !queue_.empty(); });
        return pop_front_locked();
    }

    /// Non-blocking pop. Returns std::nullopt if the queue is currently empty.
    [[nodiscard]] std::optional<T> try_pop() {
        std::lock_guard<std::mutex> lock(mutex_);
        return pop_front_locked();
    }

    /// Rejects future pushes and wakes every blocked consumer. Idempotent.
    void close() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        not_empty_.notify_all();
    }

    [[nodiscard]] bool closed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    [[nodiscard]] size_type size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    [[nodiscard]] bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

private:
    // Precondition: mutex_ is held by the caller.
    std::optional<T> pop_front_locked() {
        if (queue_.empty()) {
            return std::nullopt;
        }
        std::optional<T> item(std::move(queue_.front()));
        queue_.pop();
        return item;
    }

    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::queue<T> queue_;
    bool closed_ = false;
};

}  // namespace taskpool

#endif  // TASKPOOL_TASK_QUEUE_HPP
