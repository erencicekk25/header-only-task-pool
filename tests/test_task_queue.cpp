// SPDX-License-Identifier: MIT
#include <taskpool/TaskQueue.hpp>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "TestFramework.hpp"

using taskpool::TaskQueue;

TEST_CASE("TaskQueue preserves FIFO order") {
    TaskQueue<int> queue;
    for (int i = 0; i < 5; ++i) {
        CHECK(queue.push(int{i}));
    }
    CHECK_EQ(queue.size(), std::size_t{5});
    for (int i = 0; i < 5; ++i) {
        auto item = queue.try_pop();
        CHECK(item.has_value());
        CHECK_EQ(*item, i);
    }
    CHECK(queue.empty());
    CHECK(!queue.try_pop().has_value());
}

TEST_CASE("TaskQueue supports move-only element types") {
    TaskQueue<std::unique_ptr<int>> queue;
    CHECK(queue.push(std::make_unique<int>(42)));
    auto item = queue.wait_pop();
    CHECK(item.has_value());
    CHECK_EQ(**item, 42);
}

TEST_CASE("TaskQueue rejects pushes after close but drains remaining items") {
    TaskQueue<int> queue;
    CHECK(queue.push(1));
    CHECK(queue.push(2));
    queue.close();
    CHECK(queue.closed());
    CHECK(!queue.push(3));

    CHECK_EQ(*queue.wait_pop(), 1);
    CHECK_EQ(*queue.wait_pop(), 2);
    CHECK(!queue.wait_pop().has_value());  // closed and drained: does not block
}

TEST_CASE("TaskQueue close() wakes blocked consumers") {
    TaskQueue<int> queue;
    std::atomic<int> woke{0};
    std::vector<std::thread> consumers;
    for (int i = 0; i < 4; ++i) {
        consumers.emplace_back([&] {
            if (!queue.wait_pop().has_value()) {
                woke.fetch_add(1);
            }
        });
    }
    queue.close();
    for (auto& t : consumers) {
        t.join();
    }
    CHECK_EQ(woke.load(), 4);
}

TEST_CASE("TaskQueue delivers every item exactly once under contention") {
    constexpr int kProducers = 4;
    constexpr int kItemsPerProducer = 10000;
    TaskQueue<int> queue;
    std::atomic<long long> sum{0};
    std::atomic<int> received{0};

    std::vector<std::thread> consumers;
    for (int i = 0; i < 4; ++i) {
        consumers.emplace_back([&] {
            while (auto item = queue.wait_pop()) {
                sum.fetch_add(*item);
                received.fetch_add(1);
            }
        });
    }

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&] {
            for (int i = 1; i <= kItemsPerProducer; ++i) {
                (void)queue.push(int{i});
            }
        });
    }
    for (auto& t : producers) {
        t.join();
    }
    queue.close();
    for (auto& t : consumers) {
        t.join();
    }

    const long long expected =
        static_cast<long long>(kProducers) * kItemsPerProducer * (kItemsPerProducer + 1) / 2;
    CHECK_EQ(received.load(), kProducers * kItemsPerProducer);
    CHECK_EQ(sum.load(), expected);
}
