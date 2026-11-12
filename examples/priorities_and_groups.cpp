// SPDX-License-Identifier: MIT
//
// Priority lanes and RAII task groups.
// Build: cmake -S . -B build && cmake --build build && ./build/examples/priorities_and_groups

#include <taskpool/TaskGroup.hpp>
#include <taskpool/TaskPool.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

std::uint64_t checksum(const std::string& text) {
    std::uint64_t hash = 1469598103934665603ULL;  // FNV-1a
    for (unsigned char c : text) {
        hash = (hash ^ c) * 1099511628211ULL;
    }
    return hash;
}

}  // namespace

int main() {
    using taskpool::Priority;

    // ------------------------------------------------------------------
    // 1. Priority lanes: High overtakes Normal, which overtakes Low.
    // ------------------------------------------------------------------
    {
        taskpool::TaskPool pool(1);  // one worker makes the ordering visible

        // Keep the worker busy so the following tasks pile up in the queue.
        std::promise<void> release;
        std::shared_future<void> released = release.get_future().share();
        std::promise<void> busy;
        pool.post([&busy, released] {
            busy.set_value();
            released.wait();
        });
        busy.get_future().wait();

        std::mutex mutex;
        std::vector<std::string> order;
        auto log = [&](std::string name) {
            std::lock_guard<std::mutex> lock(mutex);
            order.push_back(std::move(name));
        };

        pool.post_with_priority(Priority::Low, log, "nightly report (Low)");
        pool.post(log, "resize thumbnail (Normal)");
        pool.post_with_priority(Priority::High, log, "user click (High)");
        pool.post_with_priority(Priority::Low, log, "cleanup cache (Low)");
        pool.post_with_priority(Priority::High, log, "heartbeat (High)");

        std::cout << "Queued per lane -> High: " << pool.pending_tasks(Priority::High)
                  << ", Normal: " << pool.pending_tasks(Priority::Normal)
                  << ", Low: " << pool.pending_tasks(Priority::Low) << "\n";

        release.set_value();
        pool.wait_idle();

        std::cout << "Execution order:\n";
        for (const auto& entry : order) {
            std::cout << "  - " << entry << '\n';
        }
    }

    // ------------------------------------------------------------------
    // 2. TaskGroup: wait for *this batch* only, automatically at scope exit.
    // ------------------------------------------------------------------
    // At least two workers: one is occupied by the background job below.
    taskpool::TaskPool pool(4);

    // A long-running background job that the group must NOT wait for.
    std::atomic<bool> stop_background{false};
    pool.post([&stop_background] {
        while (!stop_background.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    const std::vector<std::string> documents{"alpha", "bravo", "charlie", "delta", "echo"};
    std::vector<std::uint64_t> hashes(documents.size());

    const auto start = std::chrono::steady_clock::now();
    {
        taskpool::TaskGroup batch(pool, Priority::High);
        for (std::size_t i = 0; i < documents.size(); ++i) {
            batch.post([&hashes, &documents, i] { hashes[i] = checksum(documents[i]); });
        }
        auto total_chars = batch.submit([&documents] {
            std::size_t total = 0;
            for (const auto& doc : documents) {
                total += doc.size();
            }
            return total;
        });
        std::cout << "\nBatch total characters: " << total_chars.get() << '\n';
    }  // ~TaskGroup waits for the batch, not for the background job
    const auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start);

    std::cout << "Batch finished in " << elapsed.count()
              << " ms while the background job was still running\n";
    for (std::size_t i = 0; i < documents.size(); ++i) {
        std::cout << "  " << documents[i] << " -> " << std::hex << hashes[i] << std::dec << '\n';
    }

    stop_background.store(true);
    return 0;  // ~TaskPool drains and joins
}
