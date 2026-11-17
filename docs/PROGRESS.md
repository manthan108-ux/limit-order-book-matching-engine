# Progress

| Phase | Status |
|---|---|
| 1 - Foundation and architecture | Complete |
| 2 - Domain models (Order, Trade, PriceLevel) | Complete |
| 3 - OrderBook state and validation | Complete |
| 4 - Matching engine | Not started |
| 5 - Order lifecycle (cancel / modify) | Not started |
| 6 - CLI | Not started |
| 7 - Testing and reliability | Not started |
| 8 - Benchmarking | Not started |
| 9 - Documentation and finalization | Not started |

## Phase 1

Build system, `OrderTypes.h` (types, enums, transition rules, validation helpers), test harness, and docs.

## Phase 2

`Order`, `Trade` and `PriceLevel` (see [ARCHITECTURE.md](ARCHITECTURE.md)). `lob_core` became a static library. Phase 1 changes were limited to adding `RejectReason::InvalidOrderId` and a `CHECK_THROWS` macro.

## Phase 3

`OrderBook`: bid and ask maps of `PriceLevel`, a global `unordered_map` id -> (side, price, handle) lookup, `add`, `cancel`, market data (best bid/ask, spread, exact midpoint, top-N depth, counts, `find`/`contains`), and `checkInvariants()` returning a per-violation report. No Phase 1 or 2 source files were modified; only `CMakeLists.txt` and `tests/CMakeLists.txt` gained the new files.

Verified from a clean build directory under GCC 16.2 (WinLibs, MinGW-w64, Ninja) with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror`: zero warnings, `ctest` 5/5 suites pass.

| Suite | Tests |
|---|---|
| OrderTypesTests (Phase 1) | 8 |
| OrderTests | 13 |
| TradeTests | 8 |
| PriceLevelTests | 18 |
| OrderBookTests | 38 |
| **Total** | **85** |

OrderBookTests includes a deterministic 6000-operation add/cancel run checked against an independent reference model, a move-safety test, an overflow-boundary midpoint test, and invariant checks after every kind of operation.

## Known limitations

- Only GCC has been tried. MSVC and Clang are not yet verified.
- The book has no matching and no fill API. It can hold a crossed book. Phase 4 must add a way to fill a resting order that keeps `lookup_` consistent, and `PriceLevel::fill` removes a completed order, so the lookup entry must be erased in the same step.
- Cancel is O(log P) + average O(1), not O(1): it finds the level by price in the map.
- The book accepts any `sequence` values. It checks FIFO against `sequence` only in `checkInvariants()` and does not reject out-of-order submissions.
- The book remembers only resting ids. Rejecting reuse of filled or cancelled ids, if wanted, belongs to the Phase 5 order manager.
- The invariant checker's failure paths were exercised with an uncommitted scratch program. There is no committed negative test, because a corrupt book cannot be built through the public API.
- The CLI is still a placeholder.
- `benchmarks/` is empty. No benchmark numbers exist yet.
