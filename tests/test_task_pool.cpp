// SPDX-License-Identifier: MIT
#include <taskpool/TaskPool.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

#include "TestFramework.hpp"

using taskpool::TaskPool;

TEST_CASE("TaskPool starts the requested number of workers") {
    TaskPool pool(3);
    CHECK_EQ(pool.thread_count(), std::size_t{3});
}

TEST_CASE("TaskPool with zero threads falls back to hardware concurrency") {
    TaskPool pool(0);
    CHECK_EQ(pool.thread_count(), TaskPool::default_thread_count());
    CHECK(pool.thread_count() >= 1);
}

TEST_CASE("post() runs every task and wait_idle() waits for all of them") {
    TaskPool pool(4);
    std::atomic<int> counter{0};
    for (int i = 0; i < 10000; ++i) {
        pool.post([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
    }
    pool.wait_idle();
    CHECK_EQ(counter.load(), 10000);
    CHECK_EQ(pool.unfinished_tasks(), std::size_t{0});
    CHECK_EQ(pool.pending_tasks(), std::size_t{0});
}

TEST_CASE("post() forwards arguments to the callable") {
    TaskPool pool(2);
    std::atomic<int> total{0};
    pool.post([&total](int a, int b) { total.fetch_add(a + b); }, 20, 22);
    pool.wait_idle();
    CHECK_EQ(total.load(), 42);
}

TEST_CASE("post() accepts move-only callables and arguments") {
    TaskPool pool(2);
    std::atomic<int> from_capture{0};
    std::atomic<int> from_argument{0};
    auto owned = std::make_unique<int>(7);
    pool.post([&from_capture, p = std::move(owned)] { from_capture.store(*p); });
    pool.post([&from_argument](std::unique_ptr<int> p) { from_argument.store(*p); },
              std::make_unique<int>(3));
    pool.wait_idle();
    CHECK(owned == nullptr);
    CHECK_EQ(from_capture.load(), 7);
    CHECK_EQ(from_argument.load(), 3);
}

TEST_CASE("Tasks are distributed across multiple worker threads") {
    TaskPool pool(4);
    std::mutex mutex;
    std::set<std::thread::id> ids;
    for (int i = 0; i < 64; ++i) {
        pool.post([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            std::lock_guard<std::mutex> lock(mutex);
            ids.insert(std::this_thread::get_id());
        });
    }
    pool.wait_idle();
    CHECK(ids.size() > 1);
    CHECK(ids.count(std::this_thread::get_id()) == 0);
}

TEST_CASE("An exception thrown by a posted task does not kill the worker") {
    TaskPool pool(1);
    std::atomic<int> after{0};
    pool.post([] { throw std::runtime_error("boom"); });
    pool.post([&after] { after.store(1); });
    pool.wait_idle();
    CHECK_EQ(after.load(), 1);
}

TEST_CASE("Destructor completes queued work before joining (RAII)") {
    std::atomic<int> counter{0};
    {
        TaskPool pool(2);
        for (int i = 0; i < 500; ++i) {
            pool.post([&counter] {
                std::this_thread::yield();
                counter.fetch_add(1);
            });
        }
    }  // ~TaskPool
    CHECK_EQ(counter.load(), 500);
}

TEST_CASE("Tasks may post follow-up tasks; destructor waits for them too") {
    std::atomic<int> counter{0};
    {
        TaskPool pool(2);
        for (int i = 0; i < 100; ++i) {
            pool.post([&pool, &counter] {
                counter.fetch_add(1);
                pool.post([&counter] { counter.fetch_add(1); });
            });
        }
    }
    CHECK_EQ(counter.load(), 200);
}

TEST_CASE("Captured state is released before wait_idle() returns") {
    TaskPool pool(2);
    auto resource = std::make_shared<int>(1);
    std::weak_ptr<int> watcher = resource;
    pool.post([r = std::move(resource)] { (void)*r; });
    pool.wait_idle();
    CHECK(watcher.expired());
}
