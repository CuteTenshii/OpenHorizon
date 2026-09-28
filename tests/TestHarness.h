#pragma once

#include <cstdio>
#include <exception>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

/// A minimal test runner: TEST(name) defines a test that registers itself,
/// CHECK-style macros record failures without stopping the test, and
/// runAllTests() runs every test and returns the process exit code. A test
/// that throws fails with the exception's message.
namespace fh::test {

struct TestCase {
    const char* name;
    std::function<void()> body;
};

inline std::vector<TestCase>& registry()
{
    static std::vector<TestCase> tests;
    return tests;
}

inline int& failureCount()
{
    static int failures = 0;
    return failures;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> body) { registry().push_back({name, std::move(body)}); }
};

inline void fail(const char* file, int line, const std::string& message)
{
    ++failureCount();
    std::fprintf(stderr, "%s:%d: FAIL: %s\n", file, line, message.c_str());
}

/// Formats a value for a failure message when std::format can, and says so
/// when it cannot.
template <typename T> std::string show(const T& value)
{
    if constexpr (std::is_arithmetic_v<T> || std::is_convertible_v<const T&, std::string_view>) {
        return std::format("{}", value);
    } else {
        return "(value)";
    }
}

/// Runs the tests whose names contain `filter` (all of them when empty).
inline int runAllTests(std::string_view filter = {})
{
    int failedTests = 0;
    int ran = 0;
    for (const TestCase& test : registry()) {
        if (!filter.empty() && std::string_view(test.name).find(filter) == std::string_view::npos) {
            continue;
        }
        ++ran;
        const int before = failureCount();
        try {
            test.body();
        } catch (const std::exception& e) {
            fail(test.name, 0, std::format("unexpected exception: {}", e.what()));
        }
        const bool passed = failureCount() == before;
        failedTests += passed ? 0 : 1;
        std::fprintf(stderr, "%s %s\n", passed ? "PASS" : "FAIL", test.name);
    }
    std::fprintf(stderr, "%d of %d tests passed\n", ran - failedTests, ran);
    return failedTests == 0 ? 0 : 1;
}

} // namespace fh::test

#define FH_TEST_CONCAT_INNER(a, b) a##b
#define FH_TEST_CONCAT(a, b) FH_TEST_CONCAT_INNER(a, b)

#define TEST(name)                                                                                                     \
    static void name();                                                                                                \
    static const ::fh::test::Registrar FH_TEST_CONCAT(registrar_, name)(#name, name);                                  \
    static void name()

#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            ::fh::test::fail(__FILE__, __LINE__, #condition);                                                          \
        }                                                                                                              \
    } while (false)

#define CHECK_EQ(actual, expected)                                                                                     \
    do {                                                                                                               \
        const auto& fhActual = (actual);                                                                               \
        const auto& fhExpected = (expected);                                                                           \
        if (!(fhActual == fhExpected)) {                                                                               \
            ::fh::test::fail(__FILE__, __LINE__,                                                                       \
                std::format("{} == {}: got {}, expected {}", #actual, #expected, ::fh::test::show(fhActual),           \
                    ::fh::test::show(fhExpected)));                                                                    \
        }                                                                                                              \
    } while (false)

#define CHECK_NEAR(actual, expected, tolerance)                                                                        \
    do {                                                                                                               \
        const double fhActual = (actual);                                                                              \
        const double fhExpected = (expected);                                                                          \
        if (!(fhActual >= fhExpected - (tolerance) && fhActual <= fhExpected + (tolerance))) {                         \
            ::fh::test::fail(__FILE__, __LINE__,                                                                       \
                std::format("{} ~ {}: got {}, expected {}", #actual, #expected, fhActual, fhExpected));                \
        }                                                                                                              \
    } while (false)

#define CHECK_THROWS(expression)                                                                                       \
    do {                                                                                                               \
        bool fhThrew = false;                                                                                          \
        try {                                                                                                          \
            static_cast<void>(expression);                                                                             \
        } catch (const std::exception&) {                                                                              \
            fhThrew = true;                                                                                            \
        }                                                                                                              \
        if (!fhThrew) {                                                                                                \
            ::fh::test::fail(__FILE__, __LINE__, std::format("{} did not throw", #expression));                        \
        }                                                                                                              \
    } while (false)
