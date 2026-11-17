# Limit Order Book & Matching Engine (C++17)

A modular limit order book and price-time-priority matching engine, built phase by phase with the STL.

**Status: Phase 2 of 9 (domain models: Order, Trade, PriceLevel).** The order book and order matching are not implemented yet. See [docs/PROGRESS.md](docs/PROGRESS.md) and [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). The full README is written in Phase 9.

## Build

```
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Requires a C++17 compiler and CMake 3.14 or newer. No external dependencies.
