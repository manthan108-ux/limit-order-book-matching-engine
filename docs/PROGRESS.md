# Progress

| Phase | Status |
|---|---|
| 1 - Foundation and architecture | Complete |
| 2 - Domain models (Order, Trade, PriceLevel) | Complete |
| 3 - OrderBook state and validation | Complete |
| 4 - Matching engine and trade generation | Complete |
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

`OrderBook`: bid and ask maps of `PriceLevel`, a global `unordered_map` id -> (side, price, handle) lookup, `add`, `cancel`, market data (best bid/ask, spread, exact midpoint, top-N depth, counts, `find`/`contains`), and `checkInvariants()` returning a per-violation report.

## Phase 4

- `MatchingEngine`: price-time-priority matching of incoming limit orders, executing at the resting order's price, with partial and complete fills, one `Trade` per execution, and any remainder rested with its original sequence.
- `ExecutionReport` (header-only value type): accepted/rejected, reject reason, final status, original/executed/remaining quantity, and the trades generated.
- Trade ids: a deterministic counter that never wraps. If the id space cannot cover an order's executions, `submit` throws `std::overflow_error` before changing anything.
- `OrderBook` gained two narrow operations, `bestOrder(side)` and `fill(id, qty)`. `fill` keeps the lookup and levels consistent: a partial fill keeps position and lookup entry, a complete fill erases the entry and any emptied level in the same call.
- Phase 1-3 source files were not otherwise modified. `OrderBookTests.cpp` gained 5 tests for the new operations and its existing tests are unchanged. The two CMake lists gained the new files.

Verified from a clean build directory under GCC 16.2 (WinLibs, MinGW-w64, Ninja) with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror`: zero warnings, `ctest` 6/6 suites pass.

| Suite | Tests |
|---|---|
| OrderTypesTests (Phase 1) | 8 |
| OrderTests | 13 |
| TradeTests | 8 |
| PriceLevelTests | 18 |
| OrderBookTests | 43 (38 existing + 5 new) |
| MatchingEngineTests | 34 |
| **Total** | **124** |

MatchingEngineTests covers all 10 specified scenarios plus the edge cases (the trade-id overflow boundary, rejections that leave the book unchanged, large values, repeated partial fills, FIFO of rested remainders). Two 3000-order stress runs and a deep-book sweep compare every trade, every report and the final book against an independent naive reference model, with invariants checked after every operation. I also confirmed the tests catch bugs: scratch copies of the engine that execute at the incoming price, swap maker and taker, flip the taker side, ignore the limit price, or never rest the remainder each fail the suite. Those mutants are not committed.

## Known limitations

- Only GCC has been tried. MSVC and Clang are not yet verified.
- The engine only matches *incoming* orders. If resting orders already cross each other (possible only by adding to an `OrderBook` directly), an order that does not reach the cross leaves it in place. A test documents this.
- Execution timestamps are the taker's `sequence` (deterministic, no clock). There is no wall-clock time.
- If an allocation fails part-way through a match, the book stays valid but the incoming order is not rested (basic, not strong, guarantee). Validation rejections and trade-id exhaustion are strong.
- Id reuse: only resting ids are tracked. A filled or cancelled id can be submitted again. Rejecting that, if wanted, belongs to the Phase 5 order manager.
- No cancel or modify at the engine level yet (`OrderBook::cancel` exists). Both belong to Phase 5.
- Cancel and execution costs are O(log P) + average O(1) per order touched, not O(1), because the level is found by price in the map.
- The invariant checker's failure paths were exercised with an uncommitted scratch program. There is no committed negative test, because a corrupt book cannot be built through the public API.
- The CLI is still a placeholder.
- `benchmarks/` is empty. No benchmark numbers exist yet, and no performance claim is made.
