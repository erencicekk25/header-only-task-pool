// SPDX-License-Identifier: MIT
#include <taskpool/TaskPool.hpp>
#include <taskpool/TaskQueue.hpp>

#include <future>
#include <mutex>
#include <string>
#include <vector>

#include "TestFramework.hpp"

using taskpool::Priority;
using taskpool::TaskPool;
using taskpool::TaskQueue;

TEST_CASE("TaskQueue serves the highest-priority lane first") {
    TaskQueue<int> queue;
    CHECK(queue.push(1, Priority::Low));
    CHECK(queue.push(2, Priority::Normal));
    CHECK(queue.push(3, Priority::High));
    CHECK(queue.push(4, Priority::Low));
    CHECK(queue.push(5, Priority::High));

    CHECK_EQ(queue.size(), std::size_t{5});
    CHECK_EQ(queue.size(Priority::High), std::size_t{2});
    CHECK_EQ(queue.size(Priority::Normal), std::size_t{1});
    CHECK_EQ(queue.size(Priority::Low), std::size_t{2});

    const std::vector<int> expected{3, 5, 2, 1, 4};
    for (int value : expected) {
        auto item = queue.try_pop();
        CHECK(item.has_value());
        CHECK_EQ(*item, value);
    }
    CHECK(queue.empty());
}

TEST_CASE("TaskQueue push() without a priority uses the Normal lane") {
    TaskQueue<int> queue;
    CHECK(queue.push(1));
    CHECK_EQ(queue.size(Priority::Normal), std::size_t{1});
    CHECK_EQ(queue.size(Priority::High), std::size_t{0});
}

TEST_CASE("TaskQueue close() still drains every lane in priority order") {
    TaskQueue<int> queue;
    CHECK(queue.push(10, Priority::Low));
    CHECK(queue.push(20, Priority::High));
    queue.close();
    CHECK(!queue.push(30, Priority::High));
    CHECK_EQ(*queue.wait_pop(), 20);
    CHECK_EQ(*queue.wait_pop(), 10);
    CHECK(!queue.wait_pop().has_value());
}

TEST_CASE("TaskPool runs queued tasks by priority, FIFO within a lane") {
    TaskPool pool(1);
    std::promise<void> gate;
    std::shared_future<void> opened = gate.get_future().share();

    // Occupy the only worker so that every following task stays queued.
    std::promise<void> started;
    pool.post([opened, &started] {
        started.set_value();
        opened.wait();
    });
    started.get_future().wait();

    std::mutex mutex;
    std::string order;
    auto record = [&](char tag) {
        std::lock_guard<std::mutex> lock(mutex);
        order.push_back(tag);
    };

    pool.post_with_priority(Priority::Low, record, 'a');
    pool.post_with_priority(Priority::Normal, record, 'b');
    pool.post_with_priority(Priority::High, record, 'c');
    pool.post_with_priority(Priority::Low, record, 'd');
    pool.post(record, 'e');  // Normal
    auto high = pool.submit_with_priority(Priority::High, [&] {
        record('f');
        return 42;
    });

    CHECK_EQ(pool.pending_tasks(Priority::High), std::size_t{2});
    CHECK_EQ(pool.pending_tasks(Priority::Normal), std::size_t{2});
    CHECK_EQ(pool.pending_tasks(Priority::Low), std::size_t{2});

    gate.set_value();
    pool.wait_idle();

    CHECK_EQ(high.get(), 42);
    CHECK_EQ(order, std::string("cfbead"));
}
