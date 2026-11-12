// SPDX-License-Identifier: MIT
//
// header-only-task-pool
// MoveOnlyTask: a type-erased, move-only `void()` callable.
//
// `std::function` requires its target to be CopyConstructible, which rules out
// `std::packaged_task`, lambdas capturing `std::unique_ptr`, and other
// move-only callables. MoveOnlyTask lifts that restriction so tasks can be
// moved into the queue and out to a worker without a single copy.

#ifndef TASKPOOL_DETAIL_MOVE_ONLY_TASK_HPP
#define TASKPOOL_DETAIL_MOVE_ONLY_TASK_HPP

#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace taskpool::detail {

class MoveOnlyTask {
public:
    MoveOnlyTask() noexcept = default;

    template <typename F,
              typename = std::enable_if_t<!std::is_same_v<std::decay_t<F>, MoveOnlyTask>>>
    explicit MoveOnlyTask(F&& callable)
        : impl_(std::make_unique<Model<std::decay_t<F>>>(std::forward<F>(callable))) {
        static_assert(std::is_invocable_v<std::decay_t<F>&>,
                      "MoveOnlyTask requires a callable invocable with no arguments");
    }

    MoveOnlyTask(MoveOnlyTask&&) noexcept = default;
    MoveOnlyTask& operator=(MoveOnlyTask&&) noexcept = default;

    MoveOnlyTask(const MoveOnlyTask&) = delete;
    MoveOnlyTask& operator=(const MoveOnlyTask&) = delete;

    ~MoveOnlyTask() = default;

    /// Invokes the stored callable. Precondition: the task is not empty.
    void operator()() { impl_->invoke(); }

    [[nodiscard]] explicit operator bool() const noexcept { return impl_ != nullptr; }

private:
    struct Concept {
        virtual ~Concept() = default;
        virtual void invoke() = 0;
    };

    template <typename F>
    struct Model final : Concept {
        template <typename U>
        explicit Model(U&& fn) : callable(std::forward<U>(fn)) {}

        void invoke() override { std::invoke(callable); }

        F callable;
    };

    std::unique_ptr<Concept> impl_;
};

}  // namespace taskpool::detail

#endif  // TASKPOOL_DETAIL_MOVE_ONLY_TASK_HPP
