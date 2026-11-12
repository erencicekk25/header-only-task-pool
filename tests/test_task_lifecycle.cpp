// SPDX-License-Identifier: MIT
#include <taskpool/TaskPool.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "TestFramework.hpp"

using taskpool::Priority;
using taskpool::Task;
using taskpool::TaskCancelled;
using taskpool::TaskPool;
using taskpool::TaskState;

namespace {

/// Blocks a pool's worker(s) until `open()` is called.
class Gate {
public:
    Gate() : opened_(promise_.get_future().share()) {}

    /// Posts a task that occupies one worker; returns once it is running.
    void block(TaskPool& pool) {
        auto started = std::make_shared<std::promise<void>>();
        auto running = started->get_future();
        pool.post([started, opened = opened_] {
            started->set_value();
            opened.wait();
        });
        running.wait();
    }

    void open() { promise_.set_value(); }

private:
    std::promise<void> promise_;
    std::shared_future<void> opened_;
};

}  // namespace

TEST_CASE("to_string() and is_terminal() describe every state") {
    CHECK_EQ(std::string(taskpool::to_string(TaskState::Pending)), std::string("Pending"));
    CHECK_EQ(std::string(taskpool::to_string(TaskState::Running)), std::string("Running"));
    CHECK_EQ(std::string(taskpool::to_string(TaskState::Completed)), std::string("Completed"));
    CHECK_EQ(std::string(taskpool::to_string(TaskState::Failed)), std::string("Failed"));
    CHECK_EQ(std::string(taskpool::to_string(TaskState::Cancelled)), std::string("Cancelled"));
    CHECK(!taskpool::is_terminal(TaskState::Pending));
    CHECK(!taskpool::is_terminal(TaskState::Running));
    CHECK(taskpool::is_terminal(TaskState::Completed));
    CHECK(taskpool::is_terminal(TaskState::Failed));
    CHECK(taskpool::is_terminal(TaskState::Cancelled));
}

TEST_CASE("A default-constructed Task is empty") {
    Task<int> task;
    CHECK(!task.valid());
    CHECK_THROWS_AS(task.state(), std::future_error);
}

TEST_CASE("spawn() goes Pending -> Running -> Completed") {
    TaskPool pool(1);
    Gate gate;
    gate.block(pool);

    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
    std::promise<void> entered;

    Task<int> task = pool.spawn([&entered, released] {
        entered.set_value();
        released.wait();
        return 42;
    });
    static_assert(std::is_same_v<decltype(task), Task<int>>);

    CHECK(task.valid());
    CHECK(task.state() == TaskState::Pending);
    CHECK(!task.ready());

    gate.open();
    entered.get_future().wait();
    CHECK(task.state() == TaskState::Running);
    CHECK(task.wait_for(std::chrono::milliseconds(1)) == std::future_status::timeout);

    release.set_value();
    CHECK_EQ(task.get(), 42);
    CHECK(task.state() == TaskState::Completed);
    CHECK(!task.valid());  // result consumed, state still observable
}

TEST_CASE("A throwing task ends in Failed and rethrows from get()") {
    TaskPool pool(2);
    auto task = pool.spawn([]() -> int { throw std::domain_error("nope"); });
    task.wait();
    CHECK(task.state() == TaskState::Failed);
    CHECK(task.ready());
    CHECK_THROWS_AS(task.get(), std::domain_error);
}

TEST_CASE("cancel() on a pending task prevents it from ever running") {
    TaskPool pool(1);
    Gate gate;
    gate.block(pool);

    std::atomic<bool> ran{false};
    auto task = pool.spawn([&ran] { ran.store(true); });

    CHECK(task.cancel());
    CHECK(task.state() == TaskState::Cancelled);
    CHECK(task.ready());
    CHECK(task.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
    // Reported immediately, even though the worker is still blocked.
    CHECK_THROWS_AS(task.get(), TaskCancelled);
    CHECK(!task.cancel());  // idempotent: already cancelled

    gate.open();
    pool.wait_idle();
    CHECK(!ran.load());
    CHECK(task.state() == TaskState::Cancelled);
}

TEST_CASE("cancel() fails on running and finished tasks") {
    TaskPool pool(1);
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
    std::promise<void> entered;

    auto running = pool.spawn([&entered, released] {
        entered.set_value();
        released.wait();
        return std::string("done");
    });
    entered.get_future().wait();
    CHECK(!running.cancel());
    CHECK(running.state() == TaskState::Running);
    release.set_value();
    CHECK_EQ(running.get(), std::string("done"));

    auto finished = pool.spawn([] { return 1; });
    finished.wait();
    CHECK(!finished.cancel());
    CHECK(finished.state() == TaskState::Completed);
}

TEST_CASE("Task<void>, priorities and move-only results are supported") {
    TaskPool pool(2);
    std::atomic<int> hits{0};
    Task<void> unit = pool.spawn_with_priority(Priority::High, [&hits] { hits.fetch_add(1); });
    unit.get();
    CHECK(unit.state() == TaskState::Completed);
    CHECK_EQ(hits.load(), 1);

    auto boxed = pool.spawn([](int v) { return std::make_unique<int>(v); }, 9);
    const std::unique_ptr<int> value = boxed.get();
    CHECK_EQ(*value, 9);
}

TEST_CASE("Task handles can be moved and stored in containers") {
    TaskPool pool(4);
    std::vector<Task<int>> tasks;
    for (int i = 0; i < 100; ++i) {
        tasks.push_back(pool.spawn([i] { return i; }));
    }
    Task<int> moved = std::move(tasks.front());
    CHECK(!tasks.front().valid());
    int sum = moved.get();
    for (std::size_t i = 1; i < tasks.size(); ++i) {
        sum += tasks[i].get();
    }
    CHECK_EQ(sum, 4950);
}

TEST_CASE("Racing cancel() against workers: exactly one side wins") {
    TaskPool pool(4);
    constexpr int kIterations = 2000;
    std::atomic<int> executed{0};
    int cancelled = 0;

    std::vector<Task<void>> tasks;
    tasks.reserve(kIterations);
    for (int i = 0; i < kIterations; ++i) {
        tasks.push_back(pool.spawn([&executed] { executed.fetch_add(1); }));
        if (i % 2 == 0 && tasks.back().cancel()) {
            ++cancelled;
        }
    }
    pool.wait_idle();

    int completed = 0;
    for (auto& task : tasks) {
        const TaskState state = task.state();
        CHECK(state == TaskState::Completed || state == TaskState::Cancelled);
        if (state == TaskState::Completed) {
            ++completed;
            task.get();
        } else {
            CHECK_THROWS_AS(task.get(), TaskCancelled);
        }
    }
    CHECK_EQ(completed + cancelled, kIterations);
    CHECK_EQ(executed.load(), completed);
}
