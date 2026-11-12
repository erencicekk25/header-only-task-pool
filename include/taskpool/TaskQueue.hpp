// SPDX-License-Identifier: MIT
//
// header-only-task-pool
// TaskQueue: an unbounded, closable, thread-safe priority queue.
//
// Built on `std::queue` + `std::mutex` + `std::condition_variable`, with one
// FIFO lane per `Priority`. Consumers block in `wait_pop()` until an item
// arrives or the queue is closed, and always receive an item from the highest
// non-empty lane. Closing is a one-way transition: producers are rejected
// afterwards, while consumers keep draining the remaining items before they
// observe the end of the stream.

#ifndef TASKPOOL_TASK_QUEUE_HPP
#define TASKPOOL_TASK_QUEUE_HPP

#include <array>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <queue>
#include <utility>

#include "Priority.hpp"

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

    /// Moves `item` into the lane for `priority` and wakes one waiting consumer.
    /// Returns false (leaving `item` untouched) if the queue has been closed.
    [[nodiscard]] bool push(T&& item, Priority priority = Priority::Normal) {
        bool wake_consumer = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) {
                return false;
            }
            lanes_[detail::lane_index(priority)].push(std::move(item));
            ++size_;
            // Skip the (potentially syscall-backed) notification when every
            // consumer is busy: they will find the item on their next pop.
            wake_consumer = waiting_consumers_ > 0;
        }
        // Notify outside the lock so the woken consumer does not immediately
        // block on a mutex we still hold.
        if (wake_consumer) {
            not_empty_.notify_one();
        }
        return true;
    }

    /// Copies `item` into the queue. Only available for copyable `T`.
    [[nodiscard]] bool push(const T& item, Priority priority = Priority::Normal) {
        T copy(item);
        return push(std::move(copy), priority);
    }

    /// Blocks until an item is available or the queue is closed and drained.
    /// Returns std::nullopt only when no more items will ever arrive.
    [[nodiscard]] std::optional<T> wait_pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!closed_ && size_ == 0) {
            ++waiting_consumers_;
            not_empty_.wait(lock, [this] { return closed_ || size_ != 0; });
            --waiting_consumers_;
        }
        return pop_highest_locked();
    }

    /// Non-blocking pop. Returns std::nullopt if the queue is currently empty.
    [[nodiscard]] std::optional<T> try_pop() {
        std::lock_guard<std::mutex> lock(mutex_);
        return pop_highest_locked();
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

    /// Total number of queued items across all lanes.
    [[nodiscard]] size_type size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return size_;
    }

    /// Number of queued items in the lane for `priority`.
    [[nodiscard]] size_type size(Priority priority) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lanes_[detail::lane_index(priority)].size();
    }

    [[nodiscard]] bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return size_ == 0;
    }

private:
    // Precondition: mutex_ is held by the caller.
    std::optional<T> pop_highest_locked() {
        if (size_ == 0) {
            return std::nullopt;
        }
        for (std::size_t lane = kPriorityCount; lane-- > 0;) {
            std::queue<T>& queue = lanes_[lane];
            if (!queue.empty()) {
                std::optional<T> item(std::move(queue.front()));
                queue.pop();
                --size_;
                return item;
            }
        }
        return std::nullopt;  // unreachable while size_ is consistent
    }

    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::array<std::queue<T>, kPriorityCount> lanes_;
    std::size_t size_ = 0;               // sum of all lane sizes
    std::size_t waiting_consumers_ = 0;  // consumers blocked in wait_pop()
    bool closed_ = false;
};

}  // namespace taskpool

#endif  // TASKPOOL_TASK_QUEUE_HPP
