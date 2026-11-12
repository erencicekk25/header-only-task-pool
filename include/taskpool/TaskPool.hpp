// SPDX-License-Identifier: MIT
//
// header-only-task-pool
// TaskPool: a fixed-size, RAII-managed pool of worker threads.
//
//   * Workers are started in the constructor and joined in the destructor.
//   * The destructor drains every queued task (including tasks spawned by
//     other tasks) before the threads are joined - no work is lost.
//   * `shutdown()` stops the pool explicitly, either draining the backlog or
//     discarding tasks that have not started yet.
//   * Tasks are stored as move-only callables, so nothing is ever copied.
//   * `submit()` wraps work in a `std::packaged_task` and hands back the
//     matching `std::future`; `post()` is the zero-overhead fire-and-forget path.
//   * `spawn()` returns a `Task<T>` handle with an observable lifecycle
//     (Pending -> Running -> Completed / Failed, or Pending -> Cancelled).
//   * Every entry point has a `*_with_priority` variant; workers always serve
//     the highest-priority lane first.

#ifndef TASKPOOL_TASK_POOL_HPP
#define TASKPOOL_TASK_POOL_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "Priority.hpp"
#include "Task.hpp"
#include "TaskQueue.hpp"
#include "detail/MoveOnlyTask.hpp"

#define TASKPOOL_VERSION_MAJOR 1
#define TASKPOOL_VERSION_MINOR 1
#define TASKPOOL_VERSION_PATCH 0

namespace taskpool {

/// What `shutdown()` does with tasks that are queued but not started yet.
enum class ShutdownPolicy {
    Drain,           ///< run every queued task (including nested ones) first
    DiscardPending,  ///< drop queued tasks; only running tasks are finished
};

/// Thrown when work is enqueued on a pool that has been shut down.
class PoolStoppedError : public std::runtime_error {
public:
    PoolStoppedError() : std::runtime_error("taskpool::TaskPool: the pool has been shut down") {}
};

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

/// Some MSVC STL releases store a packaged_task's callable in a copyable
/// wrapper and therefore reject move-only callables. On that STL only, a
/// move-only closure is placed behind a shared_ptr to make it copyable.
/// Every other standard library receives the closure untouched.
template <typename Closure>
auto make_packaged_task_compatible(Closure&& closure) {
#if defined(_MSVC_STL_VERSION)
    using Stored = std::decay_t<Closure>;
    if constexpr (!std::is_copy_constructible_v<Stored>) {
        return [shared = std::make_shared<Stored>(std::forward<Closure>(closure))]() mutable
               -> decltype(auto) { return (*shared)(); };
    } else {
        return Stored(std::forward<Closure>(closure));
    }
#else
    return std::decay_t<Closure>(std::forward<Closure>(closure));
#endif
}

/// Marker for "no attachment": the task is queued without any wrapper.
struct NoAttachment {};

/// A callable bundled with an attachment. Members are destroyed in reverse
/// declaration order, so the callable (and everything it captured) is always
/// released *before* the attachment reports completion. A lambda cannot give
/// that guarantee: the destruction order of its captures is unspecified.
template <typename Callable, typename Attachment>
class AttachedTask {
public:
    template <typename C, typename A>
    AttachedTask(C&& callable, A&& attachment)
        : attachment_(std::forward<A>(attachment)), callable_(std::forward<C>(callable)) {}

    void operator()() { callable_(); }

private:
    Attachment attachment_;  // declared first => destroyed last
    Callable callable_;
};

/// Packs `callable` and `attachment` into one queued task. The attachment is
/// destroyed together with the task: after it runs, or when it is discarded.
template <typename Callable, typename Attachment>
MoveOnlyTask attach(Callable&& callable, Attachment&& attachment) {
    if constexpr (std::is_same_v<std::decay_t<Attachment>, NoAttachment>) {
        return MoveOnlyTask(std::forward<Callable>(callable));
    } else {
        return MoveOnlyTask(AttachedTask<std::decay_t<Callable>, std::decay_t<Attachment>>(
            std::forward<Callable>(callable), std::forward<Attachment>(attachment)));
    }
}

}  // namespace detail

class TaskGroup;

class TaskPool {
public:
    /// Receives exceptions that escape `post()` tasks, on the worker thread.
    using ErrorHandler = std::function<void(std::exception_ptr)>;

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
            accepting_.store(false, std::memory_order_release);
            queue_.close();
            join_workers();
            throw;
        }
    }

    /// Equivalent to `shutdown(ShutdownPolicy::Drain)`; a no-op if the pool
    /// was already shut down. Must not run on one of the pool's own workers.
    ~TaskPool() { shutdown(ShutdownPolicy::Drain); }

    // Workers capture `this`, so the pool must never be copied or relocated.
    TaskPool(const TaskPool&) = delete;
    TaskPool& operator=(const TaskPool&) = delete;
    TaskPool(TaskPool&&) = delete;
    TaskPool& operator=(TaskPool&&) = delete;

    // ------------------------------------------------------------------
    // Scheduling
    // ------------------------------------------------------------------

    /// Schedules `f(args...)` and returns a `std::future` for its result.
    ///
    /// * Works with any callable: free functions, lambdas, functors, member
    ///   function pointers (`submit(&Foo::bar, &foo, 1)`), etc.
    /// * Arguments are moved/copied into the task (use `std::ref` to share).
    /// * Move-only callables and arguments are fully supported.
    /// * An exception thrown by the task is captured and rethrown from
    ///   `future.get()`.
    ///
    /// Throws `PoolStoppedError` if the pool has been shut down.
    template <typename F, typename... Args>
    [[nodiscard]] auto submit(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>> {
        return submit_with_priority(Priority::Normal, std::forward<F>(f),
                                    std::forward<Args>(args)...);
    }

    /// Same as `submit()`, but schedules the task in the lane for `priority`.
    template <typename F, typename... Args>
    [[nodiscard]] auto submit_with_priority(Priority priority, F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>> {
        return submit_impl(priority, detail::NoAttachment{}, std::forward<F>(f),
                           std::forward<Args>(args)...);
    }

    /// Schedules `f(args...)` and returns a `Task<R>` handle that exposes the
    /// task's lifecycle (`state()`) and allows cancelling it before it starts.
    template <typename F, typename... Args>
    [[nodiscard]] auto spawn(F&& f, Args&&... args)
        -> Task<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>> {
        return spawn_with_priority(Priority::Normal, std::forward<F>(f),
                                   std::forward<Args>(args)...);
    }

    /// Same as `spawn()`, but schedules the task in the lane for `priority`.
    template <typename F, typename... Args>
    [[nodiscard]] auto spawn_with_priority(Priority priority, F&& f, Args&&... args)
        -> Task<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>> {
        using Result = std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;

        auto control = std::make_shared<detail::TaskControl>();
        // The LifecycleOwner rides on the queued task (not inside the
        // packaged_task, whose callable lives as long as the future), so a
        // discarded task is marked Cancelled as soon as it is dropped.
        std::future<Result> future = submit_impl(
            priority, detail::LifecycleOwner(control),
            detail::make_lifecycle_task(control, detail::bind_task(std::forward<F>(f),
                                                                   std::forward<Args>(args)...)));
        return Task<Result>(std::move(future), std::move(control));
    }

    /// Fire-and-forget: schedules `f(args...)` and returns immediately.
    /// Exceptions escaping the task are passed to the error handler (see
    /// `set_error_handler()`) and never kill a worker.
    template <typename F, typename... Args>
    void post(F&& f, Args&&... args) {
        post_with_priority(Priority::Normal, std::forward<F>(f), std::forward<Args>(args)...);
    }

    /// Same as `post()`, but schedules the task in the lane for `priority`.
    template <typename F, typename... Args>
    void post_with_priority(Priority priority, F&& f, Args&&... args) {
        post_impl(priority, detail::NoAttachment{}, std::forward<F>(f),
                  std::forward<Args>(args)...);
    }

    // ------------------------------------------------------------------
    // Waiting
    // ------------------------------------------------------------------

    /// Blocks until the queue is empty and no task is running.
    /// Must not be called from inside a task of the same pool (it would
    /// wait for itself forever).
    void wait_idle() {
        std::unique_lock<std::mutex> lock(idle_mutex_);
        idle_cv_.wait(lock, [this] { return is_idle(); });
    }

    /// Like `wait_idle()`, but gives up after `timeout`.
    /// Returns true if the pool became idle, false on timeout.
    template <typename Rep, typename Period>
    [[nodiscard]] bool wait_idle_for(const std::chrono::duration<Rep, Period>& timeout) {
        std::unique_lock<std::mutex> lock(idle_mutex_);
        return idle_cv_.wait_for(lock, timeout, [this] { return is_idle(); });
    }

    // ------------------------------------------------------------------
    // Lifecycle & error handling
    // ------------------------------------------------------------------

    /// Stops the pool and joins every worker.
    ///
    /// * `Drain` (default): runs every queued task first, including tasks
    ///   enqueued by other tasks while draining.
    /// * `DiscardPending`: rejects new work immediately and destroys queued
    ///   tasks without running them. Their `std::future`s report
    ///   `std::future_errc::broken_promise`; their `Task<T>` handles report
    ///   `TaskState::Cancelled`. Running tasks are always allowed to finish.
    ///
    /// Idempotent and safe to call from several threads at once. Afterwards,
    /// enqueuing throws `PoolStoppedError`. Calling it from one of the pool's
    /// own workers throws `std::logic_error` (a worker cannot join itself).
    void shutdown(ShutdownPolicy policy = ShutdownPolicy::Drain) {
        if (is_worker_thread()) {
            throw std::logic_error(
                "taskpool::TaskPool::shutdown() called from one of the pool's own workers");
        }
        std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
        if (stopped_) {
            return;
        }

        if (policy == ShutdownPolicy::Drain) {
            wait_idle();
            accepting_.store(false, std::memory_order_release);
            queue_.close();  // anything enqueued meanwhile is still drained
        } else {
            accepting_.store(false, std::memory_order_release);
            std::vector<detail::MoveOnlyTask> discarded = queue_.close_and_take_all();
            const std::size_t count = discarded.size();
            discarded.clear();  // destroy before reporting, like a finished task
            for (std::size_t i = 0; i < count; ++i) {
                task_finished();
            }
        }

        join_workers();
        stopped_ = true;
    }

    /// True until `shutdown()` starts; afterwards no new work is accepted.
    [[nodiscard]] bool is_running() const noexcept {
        return accepting_.load(std::memory_order_acquire);
    }

    /// True when called from one of this pool's worker threads.
    [[nodiscard]] bool is_worker_thread() const noexcept { return current_pool() == this; }

    /// Installs a handler for exceptions escaping `post()` tasks (pass an
    /// empty function to remove it). The handler runs on the worker thread
    /// that caught the exception; exceptions thrown by the handler itself are
    /// contained. Safe to call at any time, from any thread.
    void set_error_handler(ErrorHandler handler) {
        std::shared_ptr<const ErrorHandler> replacement;
        if (handler) {
            replacement = std::make_shared<const ErrorHandler>(std::move(handler));
        }
        std::lock_guard<std::mutex> lock(handler_mutex_);
        error_handler_ = std::move(replacement);
    }

    // ------------------------------------------------------------------
    // Introspection
    // ------------------------------------------------------------------

    /// Number of worker threads owned by the pool.
    [[nodiscard]] std::size_t thread_count() const noexcept { return workers_.size(); }

    /// Tasks waiting in the queue that no worker has picked up yet.
    [[nodiscard]] std::size_t pending_tasks() const { return queue_.size(); }

    /// Tasks waiting in the lane for `priority`.
    [[nodiscard]] std::size_t pending_tasks(Priority priority) const {
        return queue_.size(priority);
    }

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
    friend class TaskGroup;

    // An "attachment" is an object that travels with a queued task and is
    // destroyed right after the task runs - or when it is discarded without
    // running. It is stored next to (not inside) the packaged_task because a
    // packaged_task's callable lives as long as its future does.

    template <typename Attachment, typename F, typename... Args>
    auto submit_impl(Priority priority, Attachment&& attachment, F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>> {
        using Result = std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;

        std::packaged_task<Result()> task(detail::make_packaged_task_compatible(
            detail::bind_task(std::forward<F>(f), std::forward<Args>(args)...)));
        std::future<Result> future = task.get_future();

        // packaged_task is move-only: it travels into the queue without a copy.
        enqueue(detail::attach(std::move(task), std::forward<Attachment>(attachment)), priority);
        return future;
    }

    template <typename Attachment, typename F, typename... Args>
    void post_impl(Priority priority, Attachment&& attachment, F&& f, Args&&... args) {
        static_assert(std::is_invocable_v<std::decay_t<F>, std::decay_t<Args>...>,
                      "post(): callable cannot be invoked with the given arguments");
        enqueue(detail::attach(detail::bind_task(std::forward<F>(f), std::forward<Args>(args)...),
                               std::forward<Attachment>(attachment)),
                priority);
    }

    /// The pool whose worker is running on the calling thread, if any.
    static const TaskPool*& current_pool() noexcept {
        thread_local const TaskPool* pool = nullptr;
        return pool;
    }

    bool is_idle() const noexcept { return unfinished_.load(std::memory_order_acquire) == 0; }

    void enqueue(detail::MoveOnlyTask&& task, Priority priority) {
        // Count the task *before* it becomes visible to workers, otherwise a
        // fast worker could finish it and decrement below zero.
        unfinished_.fetch_add(1, std::memory_order_relaxed);
        if (!queue_.push(std::move(task), priority)) {
            task_finished();
            throw PoolStoppedError();
        }
    }

    void worker_loop() noexcept {
        current_pool() = this;
        while (auto task = queue_.wait_pop()) {
            try {
                (*task)();
            } catch (...) {
                // Only post() tasks get here: submit() and spawn() capture
                // exceptions in their futures.
                report_error(std::current_exception());
            }
            // Destroy the task (and everything it captured) before reporting
            // completion, so wait_idle() also implies resources are released.
            task.reset();
            task_finished();
        }
        current_pool() = nullptr;
    }

    void report_error(std::exception_ptr error) noexcept {
        std::shared_ptr<const ErrorHandler> handler;
        {
            std::lock_guard<std::mutex> lock(handler_mutex_);
            handler = error_handler_;
        }
        if (handler) {
            try {
                (*handler)(std::move(error));
            } catch (...) {
                // A faulty handler must not take the worker down with it.
            }
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

    void join_workers() noexcept {
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

    std::atomic<bool> accepting_{true};
    std::mutex lifecycle_mutex_;  // serializes shutdown()
    bool stopped_ = false;        // guarded by lifecycle_mutex_

    std::mutex handler_mutex_;
    std::shared_ptr<const ErrorHandler> error_handler_;  // guarded by handler_mutex_
};

}  // namespace taskpool

#endif  // TASKPOOL_TASK_POOL_HPP
