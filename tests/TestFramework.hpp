// SPDX-License-Identifier: MIT
//
// A deliberately tiny, dependency-free test harness. It keeps the project
// "zero external dependencies" even for contributors running the test suite.

#ifndef TASKPOOL_TESTS_TEST_FRAMEWORK_HPP
#define TASKPOOL_TESTS_TEST_FRAMEWORK_HPP

#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace testing {

struct TestCase {
    std::string name;
    std::function<void()> body;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> body) {
        registry().push_back({name, std::move(body)});
    }
};

class AssertionFailure : public std::exception {
public:
    explicit AssertionFailure(std::string message) : message_(std::move(message)) {}
    const char* what() const noexcept override { return message_.c_str(); }

private:
    std::string message_;
};

template <typename A, typename E>
void check_eq(const A& actual, const E& expected, const char* actual_expr,
              const char* expected_expr, const char* file, int line) {
    if (!(actual == expected)) {
        std::ostringstream oss;
        oss << file << ':' << line << ": CHECK_EQ(" << actual_expr << ", " << expected_expr
            << ") failed: " << actual << " != " << expected;
        throw AssertionFailure(oss.str());
    }
}

inline int run_all() {
    int failed = 0;
    for (const TestCase& test : registry()) {
        try {
            test.body();
            std::cout << "[  PASS  ] " << test.name << '\n';
        } catch (const std::exception& e) {
            ++failed;
            std::cout << "[  FAIL  ] " << test.name << "\n           " << e.what() << '\n';
        } catch (...) {
            ++failed;
            std::cout << "[  FAIL  ] " << test.name << "\n           unknown exception\n";
        }
    }
    std::cout << "\n" << registry().size() - static_cast<std::size_t>(failed) << "/"
              << registry().size() << " tests passed\n";
    return failed == 0 ? 0 : 1;
}

}  // namespace testing

#define TP_CONCAT_IMPL(a, b) a##b
#define TP_CONCAT(a, b) TP_CONCAT_IMPL(a, b)

#define TEST_CASE(name)                                                              \
    static void TP_CONCAT(test_fn_, __LINE__)();                                     \
    static const ::testing::Registrar TP_CONCAT(test_reg_, __LINE__)(                \
        name, &TP_CONCAT(test_fn_, __LINE__));                                       \
    static void TP_CONCAT(test_fn_, __LINE__)()

#define CHECK(expr)                                                                  \
    do {                                                                             \
        if (!(expr)) {                                                               \
            std::ostringstream tp_oss_;                                              \
            tp_oss_ << __FILE__ << ':' << __LINE__ << ": CHECK(" #expr ") failed";   \
            throw ::testing::AssertionFailure(tp_oss_.str());                        \
        }                                                                            \
    } while (false)

// Evaluated as one full-expression, so references into temporaries
// (e.g. `*future.get()` or `*queue.wait_pop()`) stay valid during comparison.
#define CHECK_EQ(actual, expected)                                                   \
    ::testing::check_eq((actual), (expected), #actual, #expected, __FILE__, __LINE__)

#define CHECK_THROWS_AS(expr, exception_type)                                        \
    do {                                                                             \
        bool tp_caught_ = false;                                                     \
        try {                                                                        \
            (void)(expr);                                                            \
        } catch (const exception_type&) {                                           \
            tp_caught_ = true;                                                       \
        }                                                                            \
        if (!tp_caught_) {                                                           \
            std::ostringstream tp_oss_;                                              \
            tp_oss_ << __FILE__ << ':' << __LINE__                                   \
                    << ": expected " #exception_type " from " #expr;                 \
            throw ::testing::AssertionFailure(tp_oss_.str());                        \
        }                                                                            \
    } while (false)

#endif  // TASKPOOL_TESTS_TEST_FRAMEWORK_HPP
