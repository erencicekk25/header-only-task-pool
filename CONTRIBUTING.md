# Contributing to header-only-task-pool

Thanks for your interest in improving the library! Bug reports, tests,
documentation fixes and features are all welcome.

## Finding something to work on

- Issues labelled
  [`good first issue`](https://github.com/erencicekk25/header-only-task-pool/labels/good%20first%20issue)
  are small, well-scoped and a great way to get started.
- Issues labelled
  [`help wanted`](https://github.com/erencicekk25/header-only-task-pool/labels/help%20wanted)
  are open for anyone to pick up.
- Before starting larger work, please open an issue (or comment on an existing
  one) so we can agree on the design first.

Comment on the issue you want to take so others know it is in progress.

## Development setup

You only need a C++17 compiler and CMake 3.14+. There are no other dependencies.

```bash
git clone https://github.com/<your-fork>/header-only-task-pool.git
cd header-only-task-pool
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Useful CMake options:

| Option | Purpose |
| --- | --- |
| `-DTASKPOOL_ENABLE_SANITIZERS=ON` | AddressSanitizer + UBSan (GCC/Clang) |
| `-DTASKPOOL_ENABLE_TSAN=ON` | ThreadSanitizer (GCC/Clang) |
| `-DTASKPOOL_BUILD_BENCHMARKS=OFF` | Skip the benchmark target |

## Writing tests

Tests live in `tests/` and use a tiny built-in harness (`tests/TestFramework.hpp`),
so the project stays dependency-free:

```cpp
#include <taskpool/TaskPool.hpp>
#include "TestFramework.hpp"

TEST_CASE("submit() returns the value of the task") {
    taskpool::TaskPool pool(2);
    CHECK_EQ(pool.submit([] { return 42; }).get(), 42);
}
```

Available assertions: `CHECK(expr)`, `CHECK_EQ(actual, expected)` and
`CHECK_THROWS_AS(expr, ExceptionType)`. Add a new `test_*.cpp` file to
`tests/CMakeLists.txt` if you create one.

**Concurrency tests must be deterministic.** Never rely on `sleep_for` to order
events. Use promises/futures or latches to make sure a worker is blocked
*before* asserting on queue contents (see the `Gate` helper in
`tests/test_shutdown.cpp`). Run the suite repeatedly before opening a PR:

```bash
for i in $(seq 100); do ./build/tests/taskpool_tests || break; done
```

## Code style

- Modern C++17; no compiler extensions, and the library itself stays
  header-only with zero dependencies.
- Code must compile warning-free with
  `-Wall -Wextra -Wpedantic -Wshadow -Wconversion` (and `/W4` on MSVC).
- RAII for every resource; no raw `new` / `delete`.
- 4-space indentation, `PascalCase` types, `snake_case` functions and
  variables, trailing underscore for private members (`queue_`).
- Public API gets a short `///` comment; explain *why* in comments, not *what*.
- Everything (code, comments, commit messages) is written in English.

## Commit messages

Use [Conventional Commits](https://www.conventionalcommits.org/):

```
feat: add bulk submit API (Closes #12)
fix: prevent lost wake-up in wait_idle_for()
test: cover zero-duration Task::wait_for()
docs: clarify shutdown policies
```

Reference the issue your change resolves with `Closes #N`.

## Pull requests

1. Fork the repository and create a branch from `main`.
2. Keep the change focused; add or update tests and docs alongside the code.
3. Make sure CI is green (GCC, Clang, AppleClang, MSVC, ASan, TSan).
4. Describe *what* changed and *why* in the PR description.

By contributing, you agree that your contributions are licensed under the
[MIT License](LICENSE).
