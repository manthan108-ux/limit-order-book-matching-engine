#pragma once
// Deterministic pseudo-random numbers for tests: splitmix64. Fully specified, so the
// same seed gives the same sequence on every compiler, standard library and platform
// (unlike std::uniform_int_distribution, whose output is implementation-defined).
#include <cstdint>

class TestRng {
public:
    explicit TestRng(std::uint64_t seed) : state_(seed) {}
    std::uint64_t next() {
        std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    // Uniform in [0, bound); bound must be > 0.
    std::uint64_t below(std::uint64_t bound) { return next() % bound; }
    // Uniform in [lo, hi].
    std::int64_t between(std::int64_t lo, std::int64_t hi) {
        return lo + static_cast<std::int64_t>(below(static_cast<std::uint64_t>(hi - lo) + 1));
    }
    bool chance(unsigned percent) { return below(100) < percent; }

private:
    std::uint64_t state_;
};
