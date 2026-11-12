// SPDX-License-Identifier: MIT
#include <taskpool/TaskGroup.hpp>
#include <taskpool/TaskPool.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "TestFramework.hpp"

using taskpool::PoolStoppedError;
using taskpool::ShutdownPolicy;
using taskpool::TaskCancelled;
using taskpool::TaskGroup;
using taskpool::TaskPool;
using taskpool::TaskState;

namespace {

/// Occupies one worker until `open()`; `block()` returns once it is running.
class Gate {
public:
    Gate() : opened_(promise_.get_future().share()) {}

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

/// Polls `predicate` for up to five seconds.
template <typename Predicate>
bool eventually(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

}  // namespace

TEST_CASE("shutdown(Drain) runs every queued and nested task") {
    TaskPool pool(2);
    std::atomic<int> counter{0};
    for (int i = 0; i < 200; ++i) {
        pool.post([&pool, &counter] {
            counter.fetch_add(1);
            pool.post([&counter] { counter.fetch_add(1); });
        });
    }
    pool.shutdown();
    CHECK_EQ(counter.load(), 400);
    CHECK(!pool.is_running());
    CHECK_EQ(pool.unfinished_tasks(), std::size_t{0});
}

TEST_CASE("Enqueueing after shutdown throws PoolStoppedError") {
    TaskPool pool(1);
    CHECK(pool.is_running());
    pool.shutdown();
    CHECK_THROWS_AS(pool.post([] {}), PoolStoppedError);
    CHECK_THROWS_AS((void)pool.submit([] { return 1; }), PoolStoppedError);
    CHECK_THROWS_AS((void)pool.spawn([] {}), PoolStoppedError);
    CHECK_THROWS_AS(pool.post([] {}), std::runtime_error);  // base class
    CHECK_EQ(pool.unfinished_tasks(), std::size_t{0});
}

TEST_CASE("shutdown(DiscardPending) drops queued tasks and finishes running ones") {
    TaskPool pool(1);
    Gate gate;
    gate.block(pool);

    auto running_result = std::make_shared<std::promise<void>>();
    std::atomic<int> discarded_bodies{0};

    std::vector<std::future<int>> futures;
    std::vector<taskpool::Task<int>> tasks;
    for (int i = 0; i < 10; ++i) {
        futures.push_back(pool.submit([&discarded_bodies] {
            discarded_bodies.fetch_add(1);
            return 0;
        }));
        tasks.push_back(pool.spawn([&discarded_bodies] {
            discarded_bodies.fetch_add(1);
            return 0;
        }));
        pool.post([&discarded_bodies] { discarded_bodies.fetch_add(1); });
    }
    CHECK_EQ(pool.pending_tasks(), std::size_t{30});

    std::thread stopper([&pool] { pool.shutdown(ShutdownPolicy::DiscardPending); });

    // New work is rejected and the backlog is gone while the gate task runs.
    CHECK(eventually([&] { return !pool.is_running() && pool.pending_tasks() == 0; }));
    CHECK_THROWS_AS(pool.post([] {}), PoolStoppedError);

    gate.open();  // let the running task finish so shutdown can join
    stopper.join();

    CHECK_EQ(discarded_bodies.load(), 0);
    CHECK_EQ(pool.unfinished_tasks(), std::size_t{0});

    for (auto& future : futures) {
        bool broken = false;
        try {
            future.get();
        } catch (const std::future_error& e) {
            broken = e.code() == std::make_error_code(std::future_errc::broken_promise);
        }
        CHECK(broken);
    }
    for (auto& task : tasks) {
        CHECK(task.state() == TaskState::Cancelled);
        CHECK_THROWS_AS(task.get(), TaskCancelled);
    }
}

TEST_CASE("Discarded TaskGroup tasks still release the group") {
    TaskPool pool(1);
    Gate gate;
    gate.block(pool);

    std::atomic<int> ran{0};
    TaskGroup group(pool);
    for (int i = 0; i < 5; ++i) {
        group.post([&ran] { ran.fetch_add(1); });
    }

    std::thread stopper([&pool] { pool.shutdown(ShutdownPolicy::DiscardPending); });
    CHECK(eventually([&] { return pool.pending_tasks() == 0 && !pool.is_running(); }));
    CHECK(group.wait_for(std::chrono::seconds(5)));
    gate.open();
    stopper.join();
    CHECK_EQ(ran.load(), 0);
}

TEST_CASE("shutdown() is idempotent and safe to call concurrently") {
    auto pool = std::make_unique<TaskPool>(4);
    std::atomic<int> counter{0};
    for (int i = 0; i < 1000; ++i) {
        pool->post([&counter] { counter.fetch_add(1); });
    }
    std::vector<std::thread> stoppers;
    for (int i = 0; i < 4; ++i) {
        stoppers.emplace_back([&pool] { pool->shutdown(); });
    }
    for (auto& t : stoppers) {
        t.join();
    }
    pool->shutdown();      // again: no-op
    CHECK_EQ(counter.load(), 1000);
    pool.reset();          // destructor after explicit shutdown: no-op
}

TEST_CASE("shutdown() from a worker throws std::logic_error instead of deadlocking") {
    TaskPool pool(2);
    auto attempt = pool.submit([&pool] {
        try {
            pool.shutdown();
        } catch (const std::logic_error&) {
            return true;
        }
        return false;
    });
    CHECK(attempt.get());
    CHECK(pool.is_running());
}

TEST_CASE("is_worker_thread() distinguishes pool threads") {
    TaskPool pool(1);
    TaskPool other(1);
    CHECK(!pool.is_worker_thread());
    CHECK(pool.submit([&pool] { return pool.is_worker_thread(); }).get());
    CHECK(!other.submit([&pool] { return pool.is_worker_thread(); }).get());
}

TEST_CASE("The error handler receives exceptions from post() tasks") {
    TaskPool pool(2);
    std::mutex mutex;
    std::vector<std::string> messages;
    pool.set_error_handler([&](std::exception_ptr error) {
        try {
            std::rethrow_exception(error);
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(mutex);
            messages.emplace_back(e.what());
        }
    });

    pool.post([] { throw std::runtime_error("first"); });
    pool.wait_idle();

    // Replace the handler at runtime.
    std::atomic<int> second_handler_calls{0};
    pool.set_error_handler([&](std::exception_ptr) { second_handler_calls.fetch_add(1); });
    pool.post([] { throw std::invalid_argument("second"); });
    pool.wait_idle();

    CHECK_EQ(messages.size(), std::size_t{1});
    CHECK_EQ(messages.front(), std::string("first"));
    CHECK_EQ(second_handler_calls.load(), 1);

    // submit() exceptions go to the future, never to the handler.
    auto failing = pool.submit([]() -> int { throw std::runtime_error("future"); });
    CHECK_THROWS_AS(failing.get(), std::runtime_error);
    pool.wait_idle();
    CHECK_EQ(second_handler_calls.load(), 1);
}

TEST_CASE("A throwing error handler cannot take a worker down") {
    TaskPool pool(1);
    pool.set_error_handler([](std::exception_ptr) { throw std::runtime_error("handler bug"); });
    pool.post([] { throw 42; });
    pool.wait_idle();
    pool.set_error_handler({});  // removing the handler is allowed too
    pool.post([] { throw 43; });
    CHECK_EQ(pool.submit([] { return 7; }).get(), 7);
}

TEST_CASE("wait_idle_for() reports timeouts and later success") {
    TaskPool pool(1);
    Gate gate;
    gate.block(pool);
    CHECK(!pool.wait_idle_for(std::chrono::milliseconds(20)));
    gate.open();
    CHECK(pool.wait_idle_for(std::chrono::seconds(5)));
    CHECK(pool.wait_idle_for(std::chrono::milliseconds(0)));
}
