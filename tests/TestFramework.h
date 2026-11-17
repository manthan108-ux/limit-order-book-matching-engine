#pragma once
// Minimal dependency-free unit-test harness. Tests self-register at static-init
// time; main() runs them all and returns non-zero if any check failed, which is
// what ctest keys on.
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
        t.fn();
        const bool ok = failureCount() == before;
        std::cout << (ok ? "[ PASS ] " : "[ FAIL ] ") << t.name << "\n";
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

#define CHECK_EQ(a, b)                                                    \
    do {                                                                  \
        if (!((a) == (b)))                                                \
            ::testfw::reportFailure(__FILE__, __LINE__,                   \
                                    std::string(#a " == " #b));           \
    } while (0)

#define TEST_MAIN() int main() { return ::testfw::runAll(); }
