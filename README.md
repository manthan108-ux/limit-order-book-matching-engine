# Limit Order Book & Matching Engine (C++17)

A modular limit order book and price-time-priority matching engine, built phase by phase with the STL.

**Status: Phase 4 of 9 (domain models, order book, and a price-time-priority matching engine that executes at the resting order's price and generates trades).** There is no CLI yet (Phase 6) and no order modification (Phase 5). See [docs/PROGRESS.md](docs/PROGRESS.md) and [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). The full README is written in Phase 9.

## Build

```
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Requires a C++17 compiler and CMake 3.14 or newer. No external dependencies.
