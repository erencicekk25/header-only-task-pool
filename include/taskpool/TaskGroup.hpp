// SPDX-License-Identifier: MIT
//
// header-only-task-pool
// TaskGroup: RAII scope for a batch of related tasks (structured concurrency).
//
//   {
//       taskpool::TaskGroup group(pool);
//       for (auto& item : items) group.post(process, std::ref(item));
//   }   // ~TaskGroup waits for exactly these tasks - nothing else in the pool
//
// Every task submitted through a group carries a move-only completion token.
// The token's destructor reports completion, so the bookkeeping stays correct
// whether the task ran, threw, or was destroyed without ever running.

#ifndef TASKPOOL_TASK_GROUP_HPP
#define TASKPOOL_TASK_GROUP_HPP

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <future>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>

#include "Priority.hpp"
#include "TaskPool.hpp"

namespace taskpool {

namespace detail {

struct GroupState {
    std::mutex mutex;
    std::condition_variable all_done;
    std::size_t unfinished = 0;
};

/// Registers one task with a group on construction and reports its completion
/// on destruction. Shared ownership keeps the state alive even if a token
/// outlives the TaskGroup object itself.
class GroupToken {
public:
    explicit GroupToken(std::shared_ptr<GroupState> state) : state_(std::move(state)) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        ++state_->unfinished;
    }

    GroupToken(GroupToken&&) noexcept = default;
    GroupToken& operator=(GroupToken&&) = delete;
    GroupToken(const GroupToken&) = delete;
    GroupToken& operator=(const GroupToken&) = delete;

    ~GroupToken() {
        if (!state_) {
            return;  // moved-from
        }
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (--state_->unfinished == 0) {
            state_->all_done.notify_all();
        }
    }

private:
    std::shared_ptr<GroupState> state_;
};

}  // namespace detail

class TaskGroup {
public:
    /// Creates a group whose tasks run on `pool` in the lane for `priority`.
    explicit TaskGroup(TaskPool& pool, Priority priority = Priority::Normal)
        : pool_(pool), priority_(priority), state_(std::make_shared<detail::GroupState>()) {}

    /// Waits for every task of this group (RAII).
    ~TaskGroup() { wait(); }

    TaskGroup(const TaskGroup&) = delete;
    TaskGroup& operator=(const TaskGroup&) = delete;
    TaskGroup(TaskGroup&&) = delete;
    TaskGroup& operator=(TaskGroup&&) = delete;

    /// Like `TaskPool::submit()`, but the task is tracked by this group.
    template <typename F, typename... Args>
    [[nodiscard]] auto submit(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>> {
        // The token is attached to the queued task rather than captured by
        // the callable: a packaged_task keeps its callable alive as long as
        // the future, which would otherwise delay completion until the
        // caller drops the future.
        return pool_.submit_impl(priority_, detail::GroupToken(state_), std::forward<F>(f),
                                 std::forward<Args>(args)...);
    }

    /// Like `TaskPool::post()`, but the task is tracked by this group.
    template <typename F, typename... Args>
    void post(F&& f, Args&&... args) {
        pool_.post_impl(priority_, detail::GroupToken(state_), std::forward<F>(f),
                        std::forward<Args>(args)...);
    }

    /// Blocks until every task of this group has finished and been destroyed.
    /// Do not call from inside one of the group's own tasks.
    void wait() {
        std::unique_lock<std::mutex> lock(state_->mutex);
        state_->all_done.wait(lock, [this] { return state_->unfinished == 0; });
    }

    /// Like `wait()`, but gives up after `timeout`. Returns true if all done.
    template <typename Rep, typename Period>
    [[nodiscard]] bool wait_for(const std::chrono::duration<Rep, Period>& timeout) {
        std::unique_lock<std::mutex> lock(state_->mutex);
        return state_->all_done.wait_for(lock, timeout,
                                         [this] { return state_->unfinished == 0; });
    }

    /// Tasks of this group that are queued or running.
    [[nodiscard]] std::size_t unfinished_tasks() const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->unfinished;
    }

    [[nodiscard]] Priority priority() const noexcept { return priority_; }

private:
    TaskPool& pool_;
    Priority priority_;
    std::shared_ptr<detail::GroupState> state_;
};

}  // namespace taskpool

#endif  // TASKPOOL_TASK_GROUP_HPP
