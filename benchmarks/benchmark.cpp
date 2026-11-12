// SPDX-License-Identifier: MIT
//
// header-only-task-pool benchmark suite.
//
// Scenarios:
//   1. Dispatch overhead - many tiny tasks. Compares TaskPool::post,
//      TaskPool::submit, std::async(std::launch::async) and one std::thread
//      per task. Measures pure scheduling cost.
//   2. CPU-bound scaling - counting primes in fixed-size chunks. Compares a
//      single-threaded loop with TaskPool and std::async to show speed-up.
//   3. Medium-sized tasks - a few thousand ~tens-of-microsecond jobs, the
//      typical "parallelize a loop body" use case.
//
// Every measurement reports the median of several repetitions.
// Usage: benchmark [thread_count]

#include <taskpool/TaskPool.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <future>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kRepetitions = 5;

// Prevents the optimizer from discarding benchmark results.
std::atomic<std::uint64_t> g_sink{0};

void consume(std::uint64_t value) { g_sink.fetch_add(value, std::memory_order_relaxed); }

double median_ms(const std::function<void()>& body) {
    std::vector<double> samples;
    samples.reserve(kRepetitions);
    for (int i = 0; i < kRepetitions; ++i) {
        const auto start = Clock::now();
        body();
        const auto stop = Clock::now();
        samples.push_back(std::chrono::duration<double, std::milli>(stop - start).count());
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

bool is_prime(std::uint64_t n) {
    if (n < 2) {
        return false;
    }
    if (n % 2 == 0) {
        return n == 2;
    }
    for (std::uint64_t d = 3; d * d <= n; d += 2) {
        if (n % d == 0) {
            return false;
        }
    }
    return true;
}

std::uint64_t count_primes(std::uint64_t begin, std::uint64_t end) {
    std::uint64_t count = 0;
    for (std::uint64_t i = begin; i < end; ++i) {
        count += is_prime(i) ? 1U : 0U;
    }
    return count;
}

// Roughly tens of microseconds of floating-point work.
double medium_work(int seed) {
    double acc = 0.0;
    for (int i = 1; i <= 20000; ++i) {
        acc += std::sqrt(static_cast<double>(i + seed)) / static_cast<double>(i);
    }
    return acc;
}

void print_header(const std::string& title) {
    std::cout << "\n=== " << title << " ===\n"
              << std::left << std::setw(36) << "  strategy" << std::right << std::setw(12)
              << "time (ms)" << std::setw(16) << "ns / task" << std::setw(12) << "vs base"
              << '\n'
              << "  " << std::string(74, '-') << '\n';
}

double ns_per_task(double ms, std::size_t tasks) { return ms * 1e6 / static_cast<double>(tasks); }

// `baseline_ns` is the per-task cost of the reference strategy; the last
// column shows how many times faster (per task) this strategy is.
void print_row(const std::string& name, double ms, std::size_t tasks, double baseline_ns) {
    const double per_task = ns_per_task(ms, tasks);
    std::cout << "  " << std::left << std::setw(34) << name << std::right << std::fixed
              << std::setprecision(2) << std::setw(12) << ms << std::setw(16)
              << std::setprecision(0) << per_task << std::setw(11) << std::setprecision(2)
              << baseline_ns / per_task << "x\n";
}

// ---------------------------------------------------------------------------
// Scenario 1: dispatch overhead
// ---------------------------------------------------------------------------
void bench_dispatch(taskpool::TaskPool& pool) {
    constexpr std::size_t kTasks = 200'000;
    // Spawning an OS thread per task is orders of magnitude slower, so the
    // thread-based strategies run fewer tasks; ns/task keeps them comparable.
    constexpr std::size_t kThreadTasks = 5'000;

    print_header("Dispatch overhead: tiny tasks (" + std::to_string(kTasks) + ")");

    const double post_ms = median_ms([&] {
        std::atomic<std::uint64_t> counter{0};
        for (std::size_t i = 0; i < kTasks; ++i) {
            pool.post([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
        }
        pool.wait_idle();
        consume(counter.load());
    });

    const double submit_ms = median_ms([&] {
        std::vector<std::future<std::uint64_t>> futures;
        futures.reserve(kTasks);
        for (std::size_t i = 0; i < kTasks; ++i) {
            futures.push_back(pool.submit([i] { return static_cast<std::uint64_t>(i); }));
        }
        std::uint64_t sum = 0;
        for (auto& f : futures) {
            sum += f.get();
        }
        consume(sum);
    });

    const double async_ms = median_ms([&] {
        std::vector<std::future<std::uint64_t>> futures;
        futures.reserve(kThreadTasks);
        for (std::size_t i = 0; i < kThreadTasks; ++i) {
            futures.push_back(std::async(std::launch::async,
                                         [i] { return static_cast<std::uint64_t>(i); }));
        }
        std::uint64_t sum = 0;
        for (auto& f : futures) {
            sum += f.get();
        }
        consume(sum);
    });

    const double thread_ms = median_ms([&] {
        std::atomic<std::uint64_t> counter{0};
        std::vector<std::thread> threads;
        threads.reserve(kThreadTasks);
        for (std::size_t i = 0; i < kThreadTasks; ++i) {
            threads.emplace_back([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
        }
        for (auto& t : threads) {
            t.join();
        }
        consume(counter.load());
    });

    const double base = ns_per_task(thread_ms, kThreadTasks);
    print_row("std::thread per task", thread_ms, kThreadTasks, base);
    print_row("std::async(launch::async)", async_ms, kThreadTasks, base);
    print_row("TaskPool::submit (std::future)", submit_ms, kTasks, base);
    print_row("TaskPool::post (fire-and-forget)", post_ms, kTasks, base);
}

// ---------------------------------------------------------------------------
// Scenario 2: CPU-bound scaling
// ---------------------------------------------------------------------------
void bench_cpu_bound(taskpool::TaskPool& pool) {
    constexpr std::uint64_t kLimit = 3'000'000;
    constexpr std::uint64_t kChunk = 10'000;
    constexpr std::size_t kChunks = kLimit / kChunk;

    print_header("CPU-bound: count primes below " + std::to_string(kLimit) + " (" +
                 std::to_string(kChunks) + " chunks)");

    std::uint64_t serial_result = 0;
    const double serial_ms = median_ms([&] {
        serial_result = count_primes(0, kLimit);
        consume(serial_result);
    });

    std::uint64_t pool_result = 0;
    const double pool_ms = median_ms([&] {
        std::vector<std::future<std::uint64_t>> futures;
        futures.reserve(kChunks);
        for (std::uint64_t begin = 0; begin < kLimit; begin += kChunk) {
            futures.push_back(pool.submit(count_primes, begin, begin + kChunk));
        }
        pool_result = 0;
        for (auto& f : futures) {
            pool_result += f.get();
        }
        consume(pool_result);
    });

    std::uint64_t async_result = 0;
    const double async_ms = median_ms([&] {
        std::vector<std::future<std::uint64_t>> futures;
        futures.reserve(kChunks);
        for (std::uint64_t begin = 0; begin < kLimit; begin += kChunk) {
            futures.push_back(
                std::async(std::launch::async, count_primes, begin, begin + kChunk));
        }
        async_result = 0;
        for (auto& f : futures) {
            async_result += f.get();
        }
        consume(async_result);
    });

    const double base = ns_per_task(serial_ms, kChunks);
    print_row("single thread", serial_ms, kChunks, base);
    print_row("std::async per chunk", async_ms, kChunks, base);
    print_row("TaskPool::submit per chunk", pool_ms, kChunks, base);

    if (serial_result != pool_result || serial_result != async_result) {
        std::cerr << "  !! result mismatch: " << serial_result << " / " << pool_result << " / "
                  << async_result << '\n';
        std::exit(EXIT_FAILURE);
    }
    std::cout << "  (all strategies found " << serial_result << " primes)\n";
}

// ---------------------------------------------------------------------------
// Scenario 3: medium-sized tasks
// ---------------------------------------------------------------------------
void bench_medium_tasks(taskpool::TaskPool& pool) {
    constexpr std::size_t kTasks = 4'000;

    print_header("Medium tasks: " + std::to_string(kTasks) + " floating-point jobs");

    const double serial_ms = median_ms([&] {
        double acc = 0.0;
        for (std::size_t i = 0; i < kTasks; ++i) {
            acc += medium_work(static_cast<int>(i));
        }
        consume(static_cast<std::uint64_t>(acc));
    });

    const double async_ms = median_ms([&] {
        std::vector<std::future<double>> futures;
        futures.reserve(kTasks);
        for (std::size_t i = 0; i < kTasks; ++i) {
            futures.push_back(std::async(std::launch::async, medium_work, static_cast<int>(i)));
        }
        double acc = 0.0;
        for (auto& f : futures) {
            acc += f.get();
        }
        consume(static_cast<std::uint64_t>(acc));
    });

    const double pool_ms = median_ms([&] {
        std::vector<std::future<double>> futures;
        futures.reserve(kTasks);
        for (std::size_t i = 0; i < kTasks; ++i) {
            futures.push_back(pool.submit(medium_work, static_cast<int>(i)));
        }
        double acc = 0.0;
        for (auto& f : futures) {
            acc += f.get();
        }
        consume(static_cast<std::uint64_t>(acc));
    });

    const double base = ns_per_task(serial_ms, kTasks);
    print_row("single thread", serial_ms, kTasks, base);
    print_row("std::async per task", async_ms, kTasks, base);
    print_row("TaskPool::submit per task", pool_ms, kTasks, base);
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t threads = 0;
    if (argc > 1) {
        threads = static_cast<std::size_t>(std::strtoul(argv[1], nullptr, 10));
    }

    taskpool::TaskPool pool(threads);

    std::cout << "header-only-task-pool benchmark\n"
              << "  hardware threads : " << std::thread::hardware_concurrency() << '\n'
              << "  pool threads     : " << pool.thread_count() << '\n'
              << "  repetitions      : " << kRepetitions << " (median reported)\n";
#ifndef NDEBUG
    std::cout << "  WARNING: debug build - numbers are not representative. "
                 "Configure with -DCMAKE_BUILD_TYPE=Release.\n";
#endif

    bench_dispatch(pool);
    bench_cpu_bound(pool);
    bench_medium_tasks(pool);

    std::cout << "\n(sink: " << g_sink.load() << ")\n";
    return 0;
}
