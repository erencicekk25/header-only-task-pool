// SPDX-License-Identifier: MIT
//
// A guided tour of the header-only-task-pool API.
// Build: cmake -S . -B build && cmake --build build && ./build/examples/basic_usage

#include <taskpool/TaskPool.hpp>

#include <atomic>
#include <cstdint>
#include <future>
#include <iostream>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::uint64_t fibonacci(unsigned n) {
    std::uint64_t a = 0;
    std::uint64_t b = 1;
    for (unsigned i = 0; i < n; ++i) {
        const std::uint64_t next = a + b;
        a = b;
        b = next;
    }
    return a;
}

class Greeter {
public:
    explicit Greeter(std::string greeting) : greeting_(std::move(greeting)) {}
    std::string greet(const std::string& name) const { return greeting_ + ", " + name + "!"; }

private:
    std::string greeting_;
};

}  // namespace

int main() {
    // 1. RAII: workers start here and are joined automatically at scope exit.
    taskpool::TaskPool pool;  // defaults to std::thread::hardware_concurrency()
    std::cout << "Pool started with " << pool.thread_count() << " worker threads\n\n";

    // 2. submit() a lambda and fetch the result through a std::future.
    std::future<int> answer = pool.submit([] { return 6 * 7; });
    std::cout << "[lambda]          6 * 7        = " << answer.get() << '\n';

    // 3. Free functions with arguments.
    std::future<std::uint64_t> fib = pool.submit(fibonacci, 50u);
    std::cout << "[free function]   fibonacci(50) = " << fib.get() << '\n';

    // 4. Member functions: pass the object pointer as the first argument.
    Greeter greeter("Hello");
    auto greeting = pool.submit(&Greeter::greet, &greeter, std::string("world"));
    std::cout << "[member function] " << greeting.get() << '\n';

    // 5. Move-only types travel through the pool without copies.
    auto buffer = std::make_unique<std::vector<int>>(1000, 1);
    auto sum = pool.submit(
        [](std::unique_ptr<std::vector<int>> data) {
            return std::accumulate(data->begin(), data->end(), 0);
        },
        std::move(buffer));
    std::cout << "[move-only arg]   sum of 1000 ones = " << sum.get() << '\n';

    // 6. Exceptions are captured and rethrown on get().
    auto failing = pool.submit([]() -> int { throw std::runtime_error("something went wrong"); });
    try {
        failing.get();
    } catch (const std::exception& e) {
        std::cout << "[exception]       caught: " << e.what() << '\n';
    }

    // 7. Fan-out / fan-in: split work into chunks and combine the results.
    constexpr std::uint64_t kLimit = 10'000'000;
    const std::uint64_t chunks = pool.thread_count();
    std::vector<std::future<std::uint64_t>> partials;
    for (std::uint64_t c = 0; c < chunks; ++c) {
        const std::uint64_t begin = kLimit * c / chunks;
        const std::uint64_t end = kLimit * (c + 1) / chunks;
        partials.push_back(pool.submit([begin, end] {
            std::uint64_t local = 0;
            for (std::uint64_t i = begin; i < end; ++i) {
                local += i;
            }
            return local;
        }));
    }
    std::uint64_t total = 0;
    for (auto& partial : partials) {
        total += partial.get();
    }
    std::cout << "[fan-out/fan-in]  sum[0, 10M)   = " << total << '\n';

    // 8. Fire-and-forget with post() + wait_idle().
    std::atomic<int> processed{0};
    for (int i = 0; i < 100; ++i) {
        pool.post([&processed] { processed.fetch_add(1, std::memory_order_relaxed); });
    }
    pool.wait_idle();
    std::cout << "[post + wait]     processed " << processed.load() << " fire-and-forget tasks\n";

    std::cout << "\nLeaving scope: the pool drains and joins its workers.\n";
    return 0;
}
