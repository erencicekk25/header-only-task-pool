// SPDX-License-Identifier: MIT
#include <taskpool/TaskGroup.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include "TestFramework.hpp"

using taskpool::Priority;
using taskpool::TaskGroup;
using taskpool::TaskPool;

TEST_CASE("TaskGroup::wait() waits for all of the group's tasks") {
    TaskPool pool(4);
    std::atomic<int> counter{0};
    TaskGroup group(pool);
    for (int i = 0; i < 1000; ++i) {
        group.post([&counter] { counter.fetch_add(1); });
    }
    group.wait();
    CHECK_EQ(counter.load(), 1000);
    CHECK_EQ(group.unfinished_tasks(), std::size_t{0});
}

TEST_CASE("TaskGroup destructor waits for its tasks (RAII)") {
    TaskPool pool(2);
    std::atomic<int> counter{0};
    {
        TaskGroup group(pool);
        for (int i = 0; i < 50; ++i) {
            group.post([&counter] {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                counter.fetch_add(1);
            });
        }
    }  // ~TaskGroup
    CHECK_EQ(counter.load(), 50);
}

TEST_CASE("TaskGroup waits only for its own tasks, not the whole pool") {
    TaskPool pool(2);
    std::promise<void> gate;
    std::shared_future<void> opened = gate.get_future().share();

    // An unrelated task blocks one worker until the end of the test.
    pool.post([opened] { opened.wait(); });

    std::atomic<int> counter{0};
    {
        TaskGroup group(pool);
        for (int i = 0; i < 20; ++i) {
            group.post([&counter] { counter.fetch_add(1); });
        }
        group.wait();  // must not wait for the blocked, unrelated task
    }
    CHECK_EQ(counter.load(), 20);
    // The pool's own counter may lag the group's by a moment, but the
    // blocked unrelated task is certainly still unfinished.
    CHECK(pool.unfinished_tasks() >= 1);

    gate.set_value();
    pool.wait_idle();
}

TEST_CASE("TaskGroup::submit() returns futures and propagates exceptions") {
    TaskPool pool(2);
    TaskGroup group(pool);
    auto value = group.submit([](int x) { return x * 2; }, 21);
    auto failing = group.submit([]() -> int { throw std::logic_error("bad"); });
    CHECK_EQ(value.get(), 42);
    CHECK_THROWS_AS(failing.get(), std::logic_error);
    group.wait();
    CHECK_EQ(group.unfinished_tasks(), std::size_t{0});
}

TEST_CASE("Throwing posted tasks are still accounted for by the group") {
    TaskPool pool(2);
    TaskGroup group(pool);
    for (int i = 0; i < 10; ++i) {
        group.post([] { throw std::runtime_error("ignored"); });
    }
    CHECK(group.wait_for(std::chrono::seconds(5)));
}

TEST_CASE("TaskGroup::wait_for() times out while a task is still running") {
    TaskPool pool(1);
    std::promise<void> gate;
    std::shared_future<void> opened = gate.get_future().share();

    TaskGroup group(pool, Priority::High);
    CHECK(group.priority() == Priority::High);
    group.post([opened] { opened.wait(); });

    CHECK(!group.wait_for(std::chrono::milliseconds(20)));
    CHECK_EQ(group.unfinished_tasks(), std::size_t{1});

    gate.set_value();
    CHECK(group.wait_for(std::chrono::seconds(5)));
}

TEST_CASE("TaskGroup releases captured resources before wait() returns") {
    TaskPool pool(2);
    auto resource = std::make_shared<int>(7);
    std::weak_ptr<int> watcher = resource;
    {
        TaskGroup group(pool);
        group.post([r = std::move(resource)] { (void)*r; });
    }
    CHECK(watcher.expired());
}

TEST_CASE("Independent groups can be used concurrently from many threads") {
    TaskPool pool(4);
    std::atomic<int> total{0};
    std::vector<std::thread> producers;
    for (int p = 0; p < 4; ++p) {
        producers.emplace_back([&pool, &total] {
            TaskGroup group(pool);
            std::atomic<int> local{0};
            for (int i = 0; i < 500; ++i) {
                group.post([&local] { local.fetch_add(1); });
            }
            group.wait();
            total.fetch_add(local.load());
        });
    }
    for (auto& t : producers) {
        t.join();
    }
    CHECK_EQ(total.load(), 2000);
}
