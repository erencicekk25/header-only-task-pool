// SPDX-License-Identifier: MIT
//
// header-only-task-pool
// TaskPool: a fixed-size, RAII-managed pool of worker threads.
//
//   * Workers are started in the constructor and joined in the destructor.
//   * The destructor waits for every queued task (including tasks spawned by
//     other tasks) to finish before the threads are joined - no work is lost.
//   * Tasks are stored as move-only callables, so nothing is ever copied.
//   * `submit()` wraps work in a `std::packaged_task` and hands back the
//     matching `std::future`; `post()` is the zero-overhead fire-and-forget path.

#ifndef TASKPOOL_TASK_POOL_HPP
#define TASKPOOL_TASK_POOL_HPP

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "TaskQueue.hpp"
#include "detail/MoveOnlyTask.hpp"

#define TASKPOOL_VERSION_MAJOR 1
#define TASKPOOL_VERSION_MINOR 0
#define TASKPOOL_VERSION_PATCH 0

namespace taskpool {

namespace detail {

/// Binds `f` and `args...` into a nullary callable. Arguments are decay-copied
/// (or moved) into the closure, exactly like `std::thread` and `std::async`;
/// use `std::ref` / `std::cref` to pass by reference.
template <typename F, typename... Args>
auto bind_task(F&& f, Args&&... args) {
    return [fn = std::forward<F>(f),
            bound = std::make_tuple(std::forward<Args>(args)...)]() mutable -> decltype(auto) {
        return std::apply(std::move(fn), std::move(bound));
    };
}

}  // namespace detail

class TaskPool {
public:
    /// Starts `thread_count` workers. Zero selects `default_thread_count()`.
    explicit TaskPool(std::size_t thread_count = 0) {
        const std::size_t count = thread_count == 0 ? default_thread_count() : thread_count;
        workers_.reserve(count);
        try {
            for (std::size_t i = 0; i < count; ++i) {
                workers_.emplace_back([this] { worker_loop(); });
            }
        } catch (...) {
            // The destructor will not run for a partially constructed object,
            // so release the threads that did start before rethrowing.
            stop_and_join();
            throw;
        }
    }

    /// Waits for all pending work to complete, then joins every worker.
    ~TaskPool() {
        wait_idle();
        stop_and_join();
    }

    // Workers capture `this`, so the pool must never be copied or relocated.
    TaskPool(const TaskPool&) = delete;
    TaskPool& operator=(const TaskPool&) = delete;
    TaskPool(TaskPool&&) = delete;
    TaskPool& operator=(TaskPool&&) = delete;

    /// Schedules `f(args...)` and returns a `std::future` for its result.
    ///
    /// * Works with any callable: free functions, lambdas, functors, member
    ///   function pointers (`submit(&Foo::bar, &foo, 1)`), etc.
    /// * Arguments are moved/copied into the task (use `std::ref` to share).
    /// * Move-only callables and arguments are fully supported.
    /// * An exception thrown by the task is captured and rethrown from
    ///   `future.get()`.
    ///
    /// Throws `std::runtime_error` if the pool is shutting down.
    template <typename F, typename... Args>
    [[nodiscard]] auto submit(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>> {
        using Result = std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;

        std::packaged_task<Result()> task(
            detail::bind_task(std::forward<F>(f), std::forward<Args>(args)...));
        std::future<Result> future = task.get_future();

        // packaged_task is move-only: it travels into the queue without a copy.
        enqueue(detail::MoveOnlyTask(std::move(task)));
        return future;
    }

    /// Fire-and-forget: schedules `f(args...)` and returns immediately.
    /// Exceptions escaping the task are swallowed so a worker never dies;
    /// use `submit()` when you need the result or the exception.
    template <typename F, typename... Args>
    void post(F&& f, Args&&... args) {
        static_assert(std::is_invocable_v<std::decay_t<F>, std::decay_t<Args>...>,
                      "post(): callable cannot be invoked with the given arguments");
        enqueue(detail::MoveOnlyTask(
            detail::bind_task(std::forward<F>(f), std::forward<Args>(args)...)));
    }

    /// Blocks until the queue is empty and no task is running.
    /// Must not be called from inside a task of the same pool (it would
    /// wait for itself forever).
    void wait_idle() {
        std::unique_lock<std::mutex> lock(idle_mutex_);
        idle_cv_.wait(lock, [this] { return unfinished_.load(std::memory_order_acquire) == 0; });
    }

    /// Number of worker threads owned by the pool.
    [[nodiscard]] std::size_t thread_count() const noexcept { return workers_.size(); }

    /// Tasks waiting in the queue that no worker has picked up yet.
    [[nodiscard]] std::size_t pending_tasks() const { return queue_.size(); }

    /// Tasks that are either queued or currently executing.
    [[nodiscard]] std::size_t unfinished_tasks() const noexcept {
        return unfinished_.load(std::memory_order_acquire);
    }

    /// `std::thread::hardware_concurrency()`, falling back to 1 when unknown.
    [[nodiscard]] static std::size_t default_thread_count() noexcept {
        const unsigned hw = std::thread::hardware_concurrency();
        return hw == 0 ? 1 : static_cast<std::size_t>(hw);
    }

private:
    void enqueue(detail::MoveOnlyTask&& task) {
        // Count the task *before* it becomes visible to workers, otherwise a
        // fast worker could finish it and decrement below zero.
        unfinished_.fetch_add(1, std::memory_order_relaxed);
        if (!queue_.push(std::move(task))) {
            task_finished();
            throw std::runtime_error("taskpool::TaskPool: cannot enqueue on a stopped pool");
        }
    }

    void worker_loop() noexcept {
        while (auto task = queue_.wait_pop()) {
            try {
                (*task)();
            } catch (...) {
                // post() is fire-and-forget; submit() captures exceptions in
                // its future, so nothing reaches this point from there.
            }
            // Destroy the task (and everything it captured) before reporting
            // completion, so wait_idle() also implies resources are released.
            task.reset();
            task_finished();
        }
    }

    void task_finished() noexcept {
        if (unfinished_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            // Taking the lock closes the window between a waiter checking the
            // predicate and going to sleep, preventing a lost wake-up.
            std::lock_guard<std::mutex> lock(idle_mutex_);
            idle_cv_.notify_all();
        }
    }

    void stop_and_join() noexcept {
        queue_.close();
        for (std::thread& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    TaskQueue<detail::MoveOnlyTask> queue_;
    std::vector<std::thread> workers_;

    std::atomic<std::size_t> unfinished_{0};
    std::mutex idle_mutex_;
    std::condition_variable idle_cv_;
};

}  // namespace taskpool

#endif  // TASKPOOL_TASK_POOL_HPP
