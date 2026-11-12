// SPDX-License-Identifier: MIT
#include <taskpool/TaskPool.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "TestFramework.hpp"

using taskpool::TaskPool;

namespace {

int add(int a, int b) { return a + b; }

struct Accumulator {
    int base = 100;
    int plus(int x) const { return base + x; }
};

struct MultiplyBy {
    int factor;
    int operator()(int x) const { return x * factor; }
};

}  // namespace

TEST_CASE("submit() returns the result through a std::future") {
    TaskPool pool(2);
    std::future<int> future = pool.submit([] { return 6 * 7; });
    CHECK_EQ(future.get(), 42);
}

TEST_CASE("submit() deduces the correct future type") {
    TaskPool pool(1);
    auto f1 = pool.submit([] { return std::string("hi"); });
    auto f2 = pool.submit([] {});
    auto f3 = pool.submit(add, 1, 2);
    static_assert(std::is_same_v<decltype(f1), std::future<std::string>>);
    static_assert(std::is_same_v<decltype(f2), std::future<void>>);
    static_assert(std::is_same_v<decltype(f3), std::future<int>>);
    CHECK_EQ(f1.get(), std::string("hi"));
    f2.get();
    CHECK_EQ(f3.get(), 3);
}

TEST_CASE("submit() accepts free functions, functors and member functions") {
    TaskPool pool(2);
    Accumulator acc;
    auto free_fn = pool.submit(add, 40, 2);
    auto functor = pool.submit(MultiplyBy{3}, 14);
    auto member = pool.submit(&Accumulator::plus, &acc, 5);
    auto with_ref = pool.submit(&Accumulator::plus, std::cref(acc), 6);
    CHECK_EQ(free_fn.get(), 42);
    CHECK_EQ(functor.get(), 42);
    CHECK_EQ(member.get(), 105);
    CHECK_EQ(with_ref.get(), 106);
}

TEST_CASE("submit() supports move-only callables and arguments") {
    TaskPool pool(2);
    auto owned = std::make_unique<int>(41);
    auto from_capture = pool.submit([p = std::move(owned)] { return *p + 1; });
    auto from_argument =
        pool.submit([](std::unique_ptr<int> p) { return *p * 2; }, std::make_unique<int>(21));
    auto returns_move_only = pool.submit([] { return std::make_unique<int>(7); });
    CHECK_EQ(from_capture.get(), 42);
    CHECK_EQ(from_argument.get(), 42);
    const std::unique_ptr<int> result = returns_move_only.get();
    CHECK(result != nullptr);
    CHECK_EQ(*result, 7);
}

TEST_CASE("submit() copies arguments by default and shares them with std::ref") {
    TaskPool pool(1);
    int value = 1;
    pool.submit([](int& v) { v = 99; }, std::ref(value)).get();
    CHECK_EQ(value, 99);

    std::vector<int> data{1, 2, 3};
    auto future = pool.submit(
        [](std::vector<int> copy) {
            copy.push_back(4);
            return copy.size();
        },
        data);
    CHECK_EQ(future.get(), std::size_t{4});
    CHECK_EQ(data.size(), std::size_t{3});  // original untouched
}

TEST_CASE("Exceptions propagate to future.get() and keep the pool alive") {
    TaskPool pool(1);
    auto failing = pool.submit([]() -> int { throw std::invalid_argument("bad input"); });
    CHECK_THROWS_AS(failing.get(), std::invalid_argument);

    auto healthy = pool.submit([] { return 1; });
    CHECK_EQ(healthy.get(), 1);
}

TEST_CASE("Many concurrent submissions produce correct results") {
    TaskPool pool(4);
    constexpr int kTasks = 5000;
    std::vector<std::future<long long>> futures;
    futures.reserve(kTasks);
    for (int i = 1; i <= kTasks; ++i) {
        futures.push_back(pool.submit([i] { return static_cast<long long>(i) * i; }));
    }
    long long sum = 0;
    for (auto& f : futures) {
        sum += f.get();
    }
    const long long n = kTasks;
    CHECK_EQ(sum, n * (n + 1) * (2 * n + 1) / 6);
}

TEST_CASE("Multiple producer threads can submit concurrently") {
    TaskPool pool(4);
    std::atomic<int> executed{0};
    std::vector<std::thread> producers;
    for (int p = 0; p < 4; ++p) {
        producers.emplace_back([&] {
            std::vector<std::future<void>> local;
            for (int i = 0; i < 1000; ++i) {
                local.push_back(pool.submit([&executed] { executed.fetch_add(1); }));
            }
            for (auto& f : local) {
                f.get();
            }
        });
    }
    for (auto& t : producers) {
        t.join();
    }
    CHECK_EQ(executed.load(), 4000);
}

TEST_CASE("Futures outlive the pool and remain valid after destruction") {
    std::future<int> future;
    {
        TaskPool pool(2);
        future = pool.submit([] {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return 123;
        });
    }
    CHECK(future.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
    CHECK_EQ(future.get(), 123);
}

TEST_CASE("A task can submit sub-tasks and combine their futures") {
    TaskPool pool(4);
    auto parent = pool.submit([&pool] {
        std::vector<std::future<int>> children;
        for (int i = 0; i < 8; ++i) {
            children.push_back(pool.submit([i] { return i; }));
        }
        // Do not block on children here when the pool might be saturated;
        // returning the futures lets the caller wait instead.
        return children;
    });
    std::vector<std::future<int>> children = parent.get();
    int total = 0;
    for (auto& child : children) {
        total += child.get();
    }
    CHECK_EQ(total, 28);
}
