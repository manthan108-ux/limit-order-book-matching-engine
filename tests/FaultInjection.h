#pragma once
// Test-only deterministic allocation-failure injection.
//
// Design (see docs/ARCHITECTURE.md, "Fault injection"):
//  * The library is untouched: no hooks, no allocator parameters, nothing in the
//    public API. The only moving part is a replacement of the global
//    `operator new`, defined in exactly one test executable
//    (FaultInjectionTests.cpp) and therefore invisible to every other binary.
//  * It is inert unless a `FailAllocation` guard is alive. The guard is created
//    only around the single call under test, so the test harness (iostream,
//    std::function, std::string, CHECK) never runs while it is armed.
//  * Deterministic: allocations made while armed are numbered 1, 2, 3, ... in
//    program order. The guard fails the Nth one (single-shot), or the Nth and
//    every later one (persistent, modelling real memory exhaustion). Nothing is
//    random or timing dependent.
//  * Tests sweep N = 1, 2, ... until the operation completes without hitting
//    the failure, so EVERY allocation the operation makes is failed once.
#include <cstddef>

namespace fault {

struct State {
    bool armed = false;
    long count = 0;        // allocations seen while armed
    long failAt = 0;       // 1-based index to fail; <= 0 means count only
    long second = 0;       // optional second index to fail (0 = none): a failure DURING recovery
    bool persistent = false;
};

inline State& state() {
    static State s;
    return s;
}

// RAII scope. failAt <= 0 only counts allocations (used to prove "no allocation").
class FailAllocation {
public:
    // `second` (0 = none) fails one more, later allocation: a second failure while
    // the operation is already recovering from the first.
    explicit FailAllocation(long failAt, bool persistent = false, long second = 0) {
        State& s = state();
        s = State{};
        s.failAt = failAt;
        s.persistent = persistent;
        s.second = second;
        s.armed = true;
    }
    ~FailAllocation() { state() = State{}; }
    FailAllocation(const FailAllocation&) = delete;
    FailAllocation& operator=(const FailAllocation&) = delete;
    long allocations() const { return state().count; }
};

// Called from operator new; returns true if this allocation must fail.
inline bool shouldFail() {
    State& s = state();
    if (!s.armed) return false;
    ++s.count;
    if (s.failAt <= 0) return false;
    return s.count == s.failAt || (s.second > 0 && s.count == s.second) ||
           (s.persistent && s.count > s.failAt);
}

}  // namespace fault
