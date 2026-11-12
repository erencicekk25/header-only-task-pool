// SPDX-License-Identifier: MIT
//
// header-only-task-pool
// Priority: scheduling lanes understood by TaskQueue and TaskPool.

#ifndef TASKPOOL_PRIORITY_HPP
#define TASKPOOL_PRIORITY_HPP

#include <cstddef>
#include <cstdint>

namespace taskpool {

/// Scheduling priority of a task. Workers always serve the highest non-empty
/// lane first; tasks of equal priority run in FIFO order.
///
/// Priorities are strict: a continuous stream of `High` tasks can starve
/// `Low` ones. Reserve `High` for short, latency-sensitive work.
enum class Priority : std::uint8_t {
    Low = 0,
    Normal = 1,
    High = 2,
};

/// Number of distinct priority lanes.
inline constexpr std::size_t kPriorityCount = 3;

namespace detail {

[[nodiscard]] constexpr std::size_t lane_index(Priority priority) noexcept {
    return static_cast<std::size_t>(priority);
}

}  // namespace detail

}  // namespace taskpool

#endif  // TASKPOOL_PRIORITY_HPP
