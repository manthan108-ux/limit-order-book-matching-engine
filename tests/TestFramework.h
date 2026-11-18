#pragma once
// Minimal dependency-free unit-test harness. Tests self-register at static-init
// time; main() runs them all and returns non-zero if any check failed, which is
// what ctest keys on.
#include <chrono>
#include <cstddef>
#include <functional>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace testfw {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

inline int& failureCount() {
    static int n = 0;
    return n;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

inline void reportFailure(const char* file, int line, const std::string& msg) {
    ++failureCount();
    std::cerr << "    FAILED " << file << ":" << line << ": " << msg << "\n";
}

inline int runAll() {
    std::size_t failedTests = 0;
    for (const auto& t : registry()) {
        const int before = failureCount();
        const auto started = std::chrono::steady_clock::now();
        t.fn();
        const bool ok = failureCount() == before;
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - started)
                                 .count();
        std::cout << (ok ? "[ PASS ] " : "[ FAIL ] ") << t.name;
        if (ms >= 200) std::cout << "  (" << ms << " ms)";  // only slow tests are annotated
        std::cout << "\n";
        if (!ok) ++failedTests;
    }
    std::cout << registry().size() - failedTests << "/" << registry().size()
              << " tests passed\n";
    return failedTests == 0 ? 0 : 1;
}

}  // namespace testfw

#define TEST(name)                                                        \
    static void name();                                                   \
    static ::testfw::Registrar registrar_##name(#name, name);             \
    static void name()

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) ::testfw::reportFailure(__FILE__, __LINE__, #cond);  \
    } while (0)

// Like CHECK, but `msg` (a std::string expression, evaluated only on failure) is
// appended to the failure line: used by the randomized and system-level tests to
// print a reproduction (seed, operation number, expected vs actual state).
#define CHECK_MSG(cond, msg)                                              \
    do {                                                                  \
        if (!(cond))                                                      \
            ::testfw::reportFailure(__FILE__, __LINE__,                   \
                                    std::string(#cond) + " :: " + (msg)); \
    } while (0)

#define CHECK_EQ(a, b)                                                    \
    do {                                                                  \
        if (!((a) == (b)))                                                \
            ::testfw::reportFailure(__FILE__, __LINE__,                   \
                                    std::string(#a " == " #b));           \
    } while (0)

// Passes only if evaluating `expr` throws exactly an exception derived from `ex`.
#define CHECK_THROWS(expr, ex)                                            \
    do {                                                                  \
        bool thrown_ = false;                                             \
        try {                                                             \
            (void)(expr);                                                 \
        } catch (const ex&) {                                             \
            thrown_ = true;                                               \
        } catch (...) {                                                   \
        }                                                                 \
        if (!thrown_)                                                     \
            ::testfw::reportFailure(__FILE__, __LINE__,                   \
                                    std::string(#expr " throws " #ex));   \
    } while (0)

#define TEST_MAIN() int main() { return ::testfw::runAll(); }
