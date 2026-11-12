// SPDX-License-Identifier: MIT
//
// header-only-task-pool
// Task<T>: a handle with an observable lifecycle state machine.
//
//              cancel()
//    Pending ───────────► Cancelled
//       │
//       │ worker claims the task
//       ▼
//    Running ──── returns ────► Completed
//       │
//       └──── throws ─────────► Failed
//
// All transitions are lock-free compare-and-swap operations on one shared
// atomic, so a task can never both run and be cancelled. The terminal state is
// published *before* the result becomes ready: once `get()` returns or throws,
// `state()` reports the final state.

#ifndef TASKPOOL_TASK_HPP
#define TASKPOOL_TASK_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <stdexcept>
#include <utility>

namespace taskpool {

class TaskPool;

/// Lifecycle states of a task created with `TaskPool::spawn()`.
enum class TaskState : std::uint8_t {
    Pending,    ///< queued, not started yet; can still be cancelled
    Running,    ///< claimed by a worker and executing
    Completed,  ///< returned normally
    Failed,     ///< exited with an exception
    Cancelled,  ///< cancelled before it started; it will never run
};

[[nodiscard]] constexpr const char* to_string(TaskState state) noexcept {
    switch (state) {
        case TaskState::Pending:
            return "Pending";
        case TaskState::Running:
            return "Running";
        case TaskState::Completed:
            return "Completed";
        case TaskState::Failed:
            return "Failed";
        case TaskState::Cancelled:
            return "Cancelled";
    }
    return "Unknown";
}

/// True for states that can never change again.
[[nodiscard]] constexpr bool is_terminal(TaskState state) noexcept {
    return state == TaskState::Completed || state == TaskState::Failed ||
           state == TaskState::Cancelled;
}

/// Thrown by `Task<T>::get()` when the task was cancelled before it started.
class TaskCancelled : public std::runtime_error {
public:
    TaskCancelled() : std::runtime_error("taskpool: task was cancelled before it started") {}
};

namespace detail {

/// Shared between the Task handle and the queued closure.
struct TaskControl {
    std::atomic<TaskState> state{TaskState::Pending};

    bool try_transition(TaskState from, TaskState to) noexcept {
        return state.compare_exchange_strong(from, to, std::memory_order_acq_rel,
                                             std::memory_order_acquire);
    }
};

/// Publishes Completed or Failed when the running task leaves scope. Because
/// it is destroyed after the return value is constructed but before the
/// packaged_task stores it, the state is final before the future is ready.
class FinishGuard {
public:
    explicit FinishGuard(TaskControl& control) noexcept
        : control_(control), exceptions_on_entry_(std::uncaught_exceptions()) {}

    FinishGuard(const FinishGuard&) = delete;
    FinishGuard& operator=(const FinishGuard&) = delete;

    ~FinishGuard() {
        const bool failed = std::uncaught_exceptions() > exceptions_on_entry_;
        control_.state.store(failed ? TaskState::Failed : TaskState::Completed,
                             std::memory_order_release);
    }

private:
    TaskControl& control_;
    int exceptions_on_entry_;
};

/// Wraps `closure` so that it drives the lifecycle state machine.
template <typename Closure>
auto make_lifecycle_task(std::shared_ptr<TaskControl> control, Closure&& closure) {
    return [control = std::move(control),
            task = std::forward<Closure>(closure)]() mutable -> decltype(auto) {
        if (!control->try_transition(TaskState::Pending, TaskState::Running)) {
            throw TaskCancelled();  // cancelled while queued: never run the body
        }
        FinishGuard guard(*control);
        return task();
    };
}

}  // namespace detail

/// Move-only handle to a task created with `TaskPool::spawn()`.
template <typename T>
class Task {
public:
    /// An empty handle; `valid()` is false.
    Task() noexcept = default;

    Task(Task&&) noexcept = default;
    Task& operator=(Task&&) noexcept = default;
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    ~Task() = default;

    /// Current lifecycle state. Throws `std::future_error(no_state)` on an
    /// empty handle.
    [[nodiscard]] TaskState state() const {
        return control().state.load(std::memory_order_acquire);
    }

    /// Cancels the task if it has not started yet. Returns true on success;
    /// false if it is already running or finished (running tasks are never
    /// interrupted).
    bool cancel() { return control().try_transition(TaskState::Pending, TaskState::Cancelled); }

    /// True while the handle still owns a result that `get()` can retrieve.
    [[nodiscard]] bool valid() const noexcept { return future_.valid(); }

    /// True once `get()` would not block.
    [[nodiscard]] bool ready() const {
        return state() == TaskState::Cancelled ||
               future_.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    }

    /// Returns the result, rethrows the task's exception, or throws
    /// `TaskCancelled`. Cancellation is reported immediately, without waiting
    /// for the cancelled task to reach the front of the queue.
    T get() {
        if (state() == TaskState::Cancelled) {
            throw TaskCancelled();
        }
        return future_.get();
    }

    void wait() const {
        if (state() != TaskState::Cancelled) {
            future_.wait();
        }
    }

    template <typename Rep, typename Period>
    [[nodiscard]] std::future_status wait_for(
        const std::chrono::duration<Rep, Period>& timeout) const {
        if (state() == TaskState::Cancelled) {
            return std::future_status::ready;
        }
        return future_.wait_for(timeout);
    }

    template <typename Clock, typename Duration>
    [[nodiscard]] std::future_status wait_until(
        const std::chrono::time_point<Clock, Duration>& deadline) const {
        if (state() == TaskState::Cancelled) {
            return std::future_status::ready;
        }
        return future_.wait_until(deadline);
    }

private:
    friend class TaskPool;

    Task(std::future<T> future, std::shared_ptr<detail::TaskControl> control) noexcept
        : future_(std::move(future)), control_(std::move(control)) {}

    detail::TaskControl& control() const {
        if (!control_) {
            throw std::future_error(std::future_errc::no_state);
        }
        return *control_;
    }

    std::future<T> future_;
    std::shared_ptr<detail::TaskControl> control_;
};

}  // namespace taskpool

#endif  // TASKPOOL_TASK_HPP
