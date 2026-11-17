# Progress

| Phase | Status |
|---|---|
| 1 - Foundation and architecture | Complete |
| 2 - Domain models (Order, Trade, PriceLevel) | Complete |
| 3 - OrderBook state and validation | Complete |
| 4 - Matching engine and trade generation | Complete |
| 5 - Order lifecycle management (OrderManager) | Complete |
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

`MatchingEngine` (price-time priority, resting-price execution, partial/complete fills, one `Trade` per execution, deterministic non-wrapping trade ids), `ExecutionReport`, and `OrderBook::bestOrder` / `fill`.

## Phase 5

- **`OrderManager`**: the lifecycle layer above the engine. It owns the order-id policy (ids unique for the life of the manager, generated or client-supplied, no wrap), one value-based `OrderRecord` per order (state, quantities, prices, sequences, reasons, trade indices), `submit` / `cancel` / `modify`, per-order trade history (`tradesFor`), and `checkConsistency()`.
- **Modification rules**: price change or quantity increase loses time priority (cancel/replace, may trade if it crosses); quantity decrease keeps priority (in place); no change is rejected as `NoChange`. Quantity means *remaining* quantity, and cumulative executed quantity is never altered.
- **Trade history**: the engine owns the global list; records are updated only by a cursor-based `syncTrades()`, so each trade is applied exactly once to its buyer and seller.
- **Lower-layer additions** (all additive): `Order::reduceTo` / `replaced`, `PriceLevel::reduce`, `OrderBook::extract` / `reduce` (`cancel` is now `extract` plus marking CANCELLED), `MatchingEngine::cancel` / `reduce` / `replace`, `ExecutionReport::cancelReason`, and the `OrderTypes.h` enum values `RejectReason::{InvalidSide, IdSpaceExhausted, TradeIdsExhausted}` and `CancelReason`.
- Phase 1-4 source files changed only by the additions above. No existing test was changed or removed; tests were appended to `OrderTypesTests`, `OrderTests`, `PriceLevelTests` and `OrderBookTests`.

Verified from a clean build directory under GCC 16.2 (WinLibs, MinGW-w64, Ninja) with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror`: zero warnings, `ctest` 7/7 suites pass.

| Suite | Tests |
|---|---|
| OrderTypesTests | 9 (8 existing + 1 new) |
| OrderTests | 18 (13 existing + 5 new) |
| TradeTests | 8 |
| PriceLevelTests | 21 (18 existing + 3 new) |
| OrderBookTests | 48 (43 existing + 5 new) |
| MatchingEngineTests | 34 |
| OrderManagerTests | 41 |
| **Total** | **179** (124 existing + 55 new) |

OrderManagerTests covers id policy (including generator overflow and the rejected-id policy), submission, cancellation (including after a partial fill), modification (price, quantity up/down, no change, invalid values, crossing, executed-quantity semantics, FIFO reset/preservation, trade-id exhaustion leaving the order untouched), per-order trade history, and three 3000-step lifecycle stress runs. The stress runs compare every report, every record and every order's trade list against an independent naive model, and run `checkConsistency()` after every operation. I also confirmed the tests catch bugs: scratch copies of the manager that allow id reuse, let a quantity decrease lose priority, forget the cancelled quantity, apply trades to only one side, accept no-change modifications, wrap the id generator, or skip the effective-quantity update each fail the suite. Those mutants are not committed.

## Known limitations

- Only GCC has been tried. MSVC and Clang are not yet verified.
- **Allocation failure mid-operation is only a basic guarantee** (details in ARCHITECTURE.md). The `SystemFailure` recovery path exists but is **not tested**, because there is no allocation-failure injection.
- History is never pruned: one `OrderRecord` per id ever used, plus all trades, grow without bound.
- `OrderManager::order()` returns a copy of the record, including its trade-index vector.
- A no-change `modify` is rejected (`NoChange`) rather than accepted as a no-op.
- Modifying to zero quantity is rejected; use `cancel`.
- Execution timestamps are the taker's `sequence` (deterministic, no wall clock).
- Orders are limit orders only; the engine matches only incoming orders (a cross between resting orders added directly to an `OrderBook` is not uncrossed).
- Cancel and execution costs are O(log P) + average O(1) per order, not O(1), because the level is found by price in the map.
- The CLI is still a placeholder (Phase 6).
- `benchmarks/` is empty. No benchmark numbers exist yet, and no performance claim is made.
