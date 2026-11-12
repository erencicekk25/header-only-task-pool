// SPDX-License-Identifier: MIT
//
// Task lifecycle, cancellation, error handling and graceful shutdown.
// Build: cmake -S . -B build && cmake --build build && ./build/examples/lifecycle_and_shutdown

#include <taskpool/TaskPool.hpp>

#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

int main() {
    using namespace std::chrono_literals;
    using taskpool::ShutdownPolicy;
    using taskpool::TaskState;

    taskpool::TaskPool pool(2);

    // ------------------------------------------------------------------
    // 1. Errors from fire-and-forget tasks go to a handler, not to /dev/null.
    // ------------------------------------------------------------------
    pool.set_error_handler([](std::exception_ptr error) {
        try {
            std::rethrow_exception(error);
        } catch (const std::exception& e) {
            std::cout << "[error handler] background task failed: " << e.what() << '\n';
        }
    });
    pool.post([] { throw std::runtime_error("disk full"); });
    pool.wait_idle();

    // ------------------------------------------------------------------
    // 2. Observe the lifecycle: Pending -> Running -> Completed.
    // ------------------------------------------------------------------
    auto report = pool.spawn([] {
        std::this_thread::sleep_for(30ms);
        return std::string("quarterly report");
    });
    std::cout << "\nreport state: " << taskpool::to_string(report.state()) << '\n';
    std::this_thread::sleep_for(10ms);
    std::cout << "report state: " << taskpool::to_string(report.state()) << '\n';
    std::cout << "report result: " << report.get() << '\n';
    std::cout << "report state: " << taskpool::to_string(report.state()) << '\n';

    // ------------------------------------------------------------------
    // 3. Cancel work that has not started yet.
    // ------------------------------------------------------------------
    std::vector<taskpool::Task<int>> jobs;
    for (int i = 0; i < 6; ++i) {
        jobs.push_back(pool.spawn([i] {
            std::this_thread::sleep_for(20ms);
            return i * i;
        }));
    }
    // Two workers are busy with the first jobs; the rest are still Pending.
    int cancelled = 0;
    for (auto& job : jobs) {
        if (job.cancel()) {
            ++cancelled;
        }
    }
    std::cout << "\ncancelled " << cancelled << " of " << jobs.size() << " pending jobs\n";
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        try {
            const int value = jobs[i].get();
            std::cout << "  job " << i << ": " << value << '\n';
        } catch (const taskpool::TaskCancelled&) {
            std::cout << "  job " << i << ": " << taskpool::to_string(jobs[i].state()) << '\n';
        }
    }

    // ------------------------------------------------------------------
    // 4. A watchdog with timed waits.
    // ------------------------------------------------------------------
    pool.post([] { std::this_thread::sleep_for(50ms); });
    if (!pool.wait_idle_for(5ms)) {
        std::cout << "\n[watchdog] pool still busy after 5 ms, "
                  << pool.unfinished_tasks() << " task(s) unfinished\n";
    }
    pool.wait_idle();

    // ------------------------------------------------------------------
    // 5. Graceful shutdown that discards a backlog.
    // ------------------------------------------------------------------
    std::vector<std::future<void>> backlog;
    for (int i = 0; i < 100; ++i) {
        backlog.push_back(pool.submit([] { std::this_thread::sleep_for(5ms); }));
    }
    pool.shutdown(ShutdownPolicy::DiscardPending);

    int completed = 0;
    int discarded = 0;
    for (auto& future : backlog) {
        try {
            future.get();
            ++completed;
        } catch (const std::future_error&) {
            ++discarded;  // broken_promise: the task never ran
        }
    }
    std::cout << "\nshutdown(DiscardPending): " << completed << " finished, " << discarded
              << " discarded\n";

    try {
        pool.post([] {});
    } catch (const taskpool::PoolStoppedError& e) {
        std::cout << "after shutdown: " << e.what() << '\n';
    }
    return 0;
}
