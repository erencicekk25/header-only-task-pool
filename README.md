# header-only-task-pool

[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![Header-only](https://img.shields.io/badge/header--only-yes-brightgreen.svg)](#quick-start)
[![Dependencies](https://img.shields.io/badge/dependencies-zero-brightgreen.svg)](#quick-start)
[![License: MIT](https://img.shields.io/badge/license-MIT-yellow.svg)](LICENSE)

A **header-only, zero-dependency, modern C++17 thread pool** with first-class
`std::future` support. Drop two headers into your project, `#include` one, and
start running work in parallel - no build step, no third-party libraries, no
leaks.

```cpp
#include <taskpool/TaskPool.hpp>

taskpool::TaskPool pool;                              // one worker per hardware thread
auto answer = pool.submit([](int a, int b) { return a * b; }, 6, 7);
std::cout << answer.get();                            // 42
```

> 🇹🇷 Türkçe dokümantasyon için [aşağıya kaydırın](#-türkçe).

---

## Contents

- [Features](#features)
- [Quick start](#quick-start)
- [Usage](#usage)
- [API reference](#api-reference)
- [Guarantees & design](#guarantees--design)
- [Performance](#performance)
- [Building tests, examples and benchmarks](#building-tests-examples-and-benchmarks)
- [Project layout](#project-layout)
- [Türkçe](#-türkçe)

## Features

- **Header-only & zero dependencies** - only the C++17 standard library.
- **RAII lifecycle** - workers start in the constructor; the destructor finishes
  every pending task and joins all threads. No `start()`, no `stop()`, no leaks.
- **`submit()` returns `std::future<T>`** - built on `std::packaged_task`, with
  the return type deduced for you (`std::future<void>` included).
- **Any callable, any arguments** - lambdas, free functions, functors and
  member-function pointers, with arguments forwarded like `std::thread`.
- **Move-only friendly** - tasks and arguments are *moved* end-to-end through a
  custom type-erased `MoveOnlyTask`, so `std::unique_ptr` captures just work.
- **Exception safe** - exceptions travel through the future to `get()`; a
  throwing task never kills a worker thread.
- **`post()` fire-and-forget path** plus `wait_idle()` to wait for everything.
- **Nested submission** - tasks can enqueue more tasks; shutdown waits for them.
- **CMake package** - `add_subdirectory`, `FetchContent` or `find_package`.

## Quick start

### Option 1 - copy the headers

Copy the `include/taskpool/` folder into your project and compile with C++17:

```bash
g++ -std=c++17 -O2 -pthread -Ipath/to/include main.cpp
```

### Option 2 - CMake `FetchContent`

```cmake
include(FetchContent)
FetchContent_Declare(
    taskpool
    GIT_REPOSITORY https://github.com/erencicekk25/header-only-task-pool.git
    GIT_TAG        main)
FetchContent_MakeAvailable(taskpool)

target_link_libraries(my_app PRIVATE taskpool::taskpool)
```

When consumed as a sub-project, the tests, examples and benchmarks are **not**
built, so the library adds nothing to your build time.

### Option 3 - install and `find_package`

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --install build --prefix /your/prefix
```

```cmake
find_package(taskpool CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE taskpool::taskpool)
```

## Usage

### Returning values with `std::future`

```cpp
taskpool::TaskPool pool(4);                           // exactly 4 workers

std::future<std::string> greeting = pool.submit([] { return std::string("hello"); });
std::future<void>        done     = pool.submit([] { /* side effects */ });

std::cout << greeting.get() << '\n';
done.get();
```

### Functions, functors and member functions

```cpp
int add(int a, int b) { return a + b; }

struct Account {
    int balance = 100;
    int deposit(int amount) { return balance += amount; }
};

Account account;
auto f1 = pool.submit(add, 40, 2);                          // free function
auto f2 = pool.submit(&Account::deposit, &account, 50);     // member function
auto f3 = pool.submit([](const std::vector<int>& v) { return v.size(); },
                      std::vector<int>{1, 2, 3});           // argument moved in
```

Arguments are copied or moved into the task, exactly like `std::thread`. Use
`std::ref` / `std::cref` when you really want to share a reference:

```cpp
int counter = 0;
pool.submit([](int& c) { ++c; }, std::ref(counter)).get();
```

### Move-only types

```cpp
auto buffer = std::make_unique<std::vector<float>>(1'000'000, 1.0f);

auto sum = pool.submit([](std::unique_ptr<std::vector<float>> data) {
    return std::accumulate(data->begin(), data->end(), 0.0f);
}, std::move(buffer));

auto handle = pool.submit([] { return std::make_unique<Widget>(); });  // move-only result
```

### Exceptions

```cpp
auto risky = pool.submit([]() -> int { throw std::runtime_error("oops"); });
try {
    risky.get();
} catch (const std::runtime_error& e) {
    std::cerr << "task failed: " << e.what() << '\n';   // the pool keeps running
}
```

### Fan-out / fan-in

```cpp
std::vector<std::future<std::uint64_t>> parts;
for (std::uint64_t begin = 0; begin < N; begin += chunk)
    parts.push_back(pool.submit(process_range, begin, std::min(begin + chunk, N)));

std::uint64_t total = 0;
for (auto& part : parts) total += part.get();
```

### Fire-and-forget

```cpp
for (const auto& file : files)
    pool.post(compress, file);        // no future allocated
pool.wait_idle();                     // block until every task has finished
```

A complete, runnable tour lives in [`examples/basic_usage.cpp`](examples/basic_usage.cpp).

## API reference

All symbols live in namespace `taskpool`.

| Member | Description |
| --- | --- |
| `explicit TaskPool(std::size_t threads = 0)` | Starts `threads` workers. `0` means `default_thread_count()`. |
| `~TaskPool()` | Waits for all queued and running tasks (including nested ones), then joins every worker. |
| `std::future<R> submit(F&& f, Args&&... args)` | Schedules `f(args...)` and returns its future. `R` is `std::invoke_result_t<F, Args...>`. Marked `[[nodiscard]]`. |
| `void post(F&& f, Args&&... args)` | Schedules `f(args...)` without a future. Exceptions are swallowed. |
| `void wait_idle()` | Blocks until the queue is empty and no task is running. |
| `std::size_t thread_count() const noexcept` | Number of worker threads. |
| `std::size_t pending_tasks() const` | Tasks queued but not started yet. |
| `std::size_t unfinished_tasks() const noexcept` | Tasks queued **or** currently running. |
| `static std::size_t default_thread_count() noexcept` | `std::thread::hardware_concurrency()`, or `1` if unknown. |

`TaskPool` is neither copyable nor movable: workers refer to the pool, so it
stays at a fixed address. Wrap it in `std::unique_ptr` if you need to transfer
ownership.

`taskpool::TaskQueue<T>` (the closable, thread-safe queue that powers the pool)
is also public and usable on its own: `push`, `wait_pop`, `try_pop`, `close`,
`size`, `empty`.

## Guarantees & design

```
 submit()/post()                    ┌──────────── worker 1 ──┐
      │   MoveOnlyTask              │ wait_pop() → run → ... │
      ▼                             ├──────────── worker 2 ──┤
 ┌──────────────────────────┐ ───►  │ wait_pop() → run → ... │
 │ TaskQueue                │       ├──────────── worker N ──┤
 │ std::queue + std::mutex  │ ───►  │ wait_pop() → run → ... │
 │ + std::condition_variable│       └────────────────────────┘
 └──────────────────────────┘
```

- **Thread safety** - every public member may be called concurrently from any
  thread, including from inside tasks.
- **No lost work** - the destructor first waits for the pool to become idle,
  then closes the queue and joins. Tasks posted by other tasks are completed too.
- **No lost wake-ups** - all predicate checks happen under the mutex; idle
  notification re-acquires the lock before signalling.
- **Zero leaks** - ownership is expressed with `std::unique_ptr`,
  `std::packaged_task` and `std::future` only; there is no raw `new`/`delete`.
  A task's captures are destroyed *before* it is reported complete, so after
  `wait_idle()` returns every captured resource has been released.
- **Fewer syscalls** - the queue only signals the condition variable when a
  worker is actually sleeping.
- **Exception-safe construction** - if creating a thread fails, the threads that
  already started are shut down and joined before the exception propagates.
- **Move semantics everywhere** - callables and arguments are perfectly
  forwarded into the task, and the task is moved (never copied) into the queue
  and out to the worker.

**Caveats**

- Do not call `wait_idle()` from inside a task of the same pool: the task would
  wait for itself.
- Blocking on `future.get()` inside a task is fine only if other workers can
  make progress; on a saturated pool it can deadlock (as with any fixed-size pool).
  Prefer returning futures to the caller, as shown in the tests.

## Performance

Sample results from the bundled benchmark (`taskpool_benchmark`), Release build,
MinGW-w64 GCC 16.2, Windows 11, 14 hardware threads, median of 5 runs. Your
numbers will differ, so run it on your own hardware.

**Dispatch overhead** - 200,000 trivial tasks (per-task cost):

| Strategy | ns / task | Relative |
| --- | ---: | ---: |
| `std::thread` per task | 50,503 | 1.0x |
| `std::async(std::launch::async)` | 51,720 | 1.0x |
| `TaskPool::submit` (with `std::future`) | 2,311 | **21.9x** |
| `TaskPool::post` | 7,539 | 6.7x |

**CPU-bound** - counting primes below 3,000,000 in 300 chunks:

| Strategy | Time (ms) | Speed-up |
| --- | ---: | ---: |
| Single thread | 559.2 | 1.0x |
| `std::async` per chunk | 55.4 | 10.1x |
| `TaskPool::submit` per chunk | 49.4 | **11.3x** |

**Medium tasks** - 4,000 floating-point jobs of roughly 80 µs each:

| Strategy | Time (ms) | Speed-up |
| --- | ---: | ---: |
| Single thread | 327.9 | 1.0x |
| `std::async` per task | 200.3 | 1.6x |
| `TaskPool::submit` per task | 55.9 | **5.9x** |

**How to read these numbers**

- Reusing threads is the big win: thread creation dominates whenever tasks are
  short, which is where `std::async` and raw `std::thread` fall behind.
- The pool uses a **single shared queue** guarded by one mutex. For *trivially*
  small tasks, all workers compete for that lock, so per-task cost grows with
  the thread count (on the same machine `post` costs about 340 ns per task with
  2 workers). In the table above, `post` tasks also all write to one shared
  atomic counter. Both effects are artifacts of tiny tasks.
- **Rule of thumb:** give each task at least a few microseconds of work. Batch
  tiny items into chunks (as in the prime-counting scenario) and the pool scales
  close to linearly.

## Building tests, examples and benchmarks

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure

./build/examples/basic_usage
./build/benchmarks/taskpool_benchmark        # optional arg: thread count
```

| CMake option | Default (top-level) | Purpose |
| --- | --- | --- |
| `TASKPOOL_BUILD_TESTS` | `ON` | Unit tests (built-in micro test harness, no dependencies). |
| `TASKPOOL_BUILD_EXAMPLES` | `ON` | `examples/basic_usage`. |
| `TASKPOOL_BUILD_BENCHMARKS` | `ON` | `benchmarks/taskpool_benchmark`. |
| `TASKPOOL_ENABLE_SANITIZERS` | `OFF` | AddressSanitizer + UBSan (GCC/Clang). |
| `TASKPOOL_ENABLE_TSAN` | `OFF` | ThreadSanitizer (GCC/Clang). |

Continuous integration builds and tests the project with GCC, Clang, AppleClang
and MSVC, and runs the suite under AddressSanitizer and ThreadSanitizer.

## Project layout

```
include/taskpool/
├── TaskPool.hpp              # the pool: submit(), post(), wait_idle()
├── TaskQueue.hpp             # thread-safe, closable FIFO queue
└── detail/MoveOnlyTask.hpp   # type-erased move-only void() callable
examples/basic_usage.cpp      # guided API tour
benchmarks/benchmark.cpp      # dispatch, CPU-bound and medium-task benchmarks
tests/                        # unit tests + tiny dependency-free harness
cmake/                        # package config for find_package()
```

## License

[MIT](LICENSE) - free for commercial and open-source use.

---

## 🇹🇷 Türkçe

**header-only-task-pool**, modern C++17 ile yazılmış, **yalnızca başlık
dosyalarından oluşan (header-only)** ve **hiçbir dış bağımlılığı olmayan** bir
thread pool (iş parçacığı havuzu) kütüphanesidir. Başlık dosyalarını projenize
kopyalayıp `#include` etmeniz yeterli. Ayrıca derlemeniz gereken bir şey yok,
üçüncü parti kütüphane kurmanız gerekmiyor.

### Neden bu kütüphane?

- **Sıfır bağımlılık:** Sadece C++17 standart kütüphanesi kullanılır.
- **RAII ile otomatik yönetim:** Worker thread'ler constructor'da başlar.
  Destructor kuyruktaki tüm işlerin bitmesini bekler ve thread'leri güvenle
  `join` eder. `start()` / `stop()` çağırmanıza gerek yok, bellek sızıntısı da yok.
- **`std::future` desteği:** `submit()` her görevi bir `std::packaged_task` ile
  sarar ve dönüş tipini otomatik çıkaran bir `std::future<T>` döndürür.
- **Her türlü çağrılabilir nesne:** Lambda, serbest fonksiyon, functor ve üye
  fonksiyon işaretçileri desteklenir.
- **Move semantiği:** Görevler ve argümanlar baştan sona *taşınır*, hiç
  kopyalanmaz. `std::unique_ptr` gibi yalnızca taşınabilir (move-only) tipler
  sorunsuz çalışır.
- **İstisna güvenliği:** Görev içinde fırlatılan istisna `future.get()` ile size
  ulaşır. Hata veren bir görev worker thread'i asla öldürmez.

### Kurulum

En kolay yol `include/taskpool/` klasörünü projenize kopyalamaktır:

```bash
g++ -std=c++17 -O2 -pthread -Iinclude main.cpp
```

CMake kullanıyorsanız `FetchContent` ile ekleyip
`target_link_libraries(uygulamam PRIVATE taskpool::taskpool)` satırını
yazmanız yeterli (ayrıntılar için yukarıdaki *Quick start* bölümüne bakın).

### Hızlı örnek

```cpp
#include <taskpool/TaskPool.hpp>
#include <iostream>

int topla(int a, int b) { return a + b; }

int main() {
    taskpool::TaskPool havuz;                       // donanım thread sayısı kadar worker

    auto sonuc   = havuz.submit(topla, 40, 2);      // std::future<int>
    auto mesaj   = havuz.submit([] { return std::string("merhaba"); });
    auto hatali  = havuz.submit([]() -> int { throw std::runtime_error("hata!"); });

    std::cout << sonuc.get() << ' ' << mesaj.get() << '\n';   // 42 merhaba

    try {
        hatali.get();
    } catch (const std::exception& e) {
        std::cout << "Yakalandı: " << e.what() << '\n';
    }

    havuz.post([] { /* sonucu beklenmeyen arka plan işi */ });
    havuz.wait_idle();                              // tüm işler bitene kadar bekle
}   // havuz burada kalan işleri tamamlar ve thread'leri güvenle kapatır
```

### API özeti

| Üye | Açıklama |
| --- | --- |
| `TaskPool(n = 0)` | `n` adet worker başlatır; `0` donanım thread sayısını kullanır. |
| `submit(f, args...)` | Görevi kuyruğa ekler, sonucu için `std::future` döndürür. |
| `post(f, args...)` | Sonucu beklenmeyen ("ateşle ve unut") görev ekler. |
| `wait_idle()` | Kuyruk boşalıp çalışan görev kalmayana kadar bekler. |
| `thread_count()` | Worker thread sayısı. |
| `pending_tasks()` / `unfinished_tasks()` | Bekleyen / henüz bitmemiş görev sayısı. |

### Performans notları

Benchmark sonuçları (yukarıdaki *Performance* bölümü), thread'leri yeniden
kullanmanın her görev için yeni `std::thread` ya da `std::async` açmaktan
**20 kat daha hızlı** olabildiğini gösteriyor. CPU yoğun işlerde ise 14 thread'li
bir makinede **11 kata yakın** hızlanma elde edildi.

Kütüphane, tek bir mutex ile korunan **ortak bir kuyruk** kullanır. Çok küçük
(nanosaniyeler süren) görevlerde tüm worker'lar aynı kilit için yarışır. Bu yüzden
en iyi sonuç için **her görevin en az birkaç mikrosaniyelik iş yapmasını**
sağlayın. Küçük işleri parçalar (chunk) halinde gruplamak, havuzun neredeyse
doğrusal ölçeklenmesini sağlar.

### Dikkat edilmesi gerekenler

- Bir görevin içinden aynı havuzun `wait_idle()` fonksiyonunu çağırmayın,
  görev kendini bekler ve kilitlenir.
- Dolu bir havuzda bir görevin başka bir görevin `future.get()` sonucunu
  beklemesi kilitlenmeye (deadlock) yol açabilir. Bunun yerine future'ları
  çağıran tarafa döndürün.

### Derleme ve test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
./build/benchmarks/taskpool_benchmark
```

Katkılarınızı bekliyoruz! Projeyi faydalı bulduysanız bir ⭐ bırakmayı unutmayın.

**Lisans:** [MIT](LICENSE)
