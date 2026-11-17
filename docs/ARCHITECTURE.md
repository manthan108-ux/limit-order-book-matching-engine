# Architecture

> Status: Phase 1 design. Sections marked *(planned)* describe code that does not exist yet; see [PROGRESS.md](PROGRESS.md).

## Layering

```
CLI (src/main.cpp)            parsing + printing only, no business logic   (Phase 6)
        |
OrderManager                  validate, add / cancel / modify, reports     (Phase 5)
        |
MatchingEngine                price-time matching, trade generation        (Phase 4)
        |
OrderBook -> PriceLevel       storage, ordering, lookup, invariants        (Phases 2-3)
        |
OrderTypes / Order / Trade / ExecutionReport   plain domain types          (Phases 1-2)
```

Dependencies only point downward. The core library (`lob_core`) performs no I/O, so the CLI can be replaced by an API or a replay tool, and ExecutionReport is a plain value type.

## Core types (implemented, `include/OrderTypes.h`)

- `Price` and `Quantity` are signed 64-bit integers. Price is in integer ticks, so there is no floating-point error. They are signed so that bad input such as `-5` can be represented and rejected explicitly instead of wrapping around.
- Enums: `Side`, `OrderType` (LIMIT only), `OrderStatus`, `Operation`, `RejectReason`.
- `canTransition` encodes the legal lifecycle (below), so later phases enforce it in a single place.

### Order lifecycle

```
NEW --(partial fill)--> PARTIALLY_FILLED --(partial fill)--> PARTIALLY_FILLED
 |                              |
 |--(full fill)--> FILLED <-----|
 |--(cancel)-----> CANCELLED <--|
 '--(validation fails)--> REJECTED
```

FILLED, CANCELLED and REJECTED are terminal. A terminal order is never in the book or in the lookup map.

## Data structure plan *(planned, Phases 2-3)*

| Structure | Purpose | Why |
|---|---|---|
| `std::map<Price, PriceLevel, std::greater<>>` (bids) | Price levels, best bid first | Ordered iteration gives price priority. `begin()` is the best bid, O(log P) insert/erase. |
| `std::map<Price, PriceLevel>` (asks) | Price levels, best ask first | Same, ascending. |
| `std::list<Order>` inside each `PriceLevel` | FIFO queue at one price | List iterators stay valid when other elements are erased, so removing an order from the middle is O(1) with a stored iterator. `std::deque` was the suggested alternative, but erasing from the middle of a deque is O(n) and invalidates iterators. See the decision below. |
| `std::unordered_map<OrderId, OrderLocation>` | Order ID -> (side, price, list iterator) | Average O(1) cancel/modify lookup and O(1) duplicate-ID detection. |
| `std::vector<Trade>` | Append-only trade history | Cache-friendly sequential storage, and trade IDs are indices. |
| `std::optional` | Optional results (for example best bid on an empty side) | Avoids sentinel values. |

### Decision: `std::list` rather than `std::deque` for the level queue

The specification lists both. A deque gives better cache locality but cannot delete from the middle in O(1) and does not keep iterators valid across erase. Order cancellation at an arbitrary queue position is a core requirement, and the order lookup map must hold stable handles, so `std::list` is the correct primary choice. `std::deque` is not forced into the design where it would be wrong. If the benchmarks (Phase 8) justify it, a deque with lazy deletion can be evaluated and documented as an experiment.

### Priority rules *(to be implemented, Phase 5)*

- Price priority first, then arrival sequence.
- Trades execute at the resting order's price.
- Modify: a price change cancels and re-enters the order with a new sequence (loses priority). A quantity increase also loses priority. A quantity decrease keeps priority and the order stays in place. Modifying a terminal order is rejected.

## Testing approach

`tests/TestFramework.h` is a small self-registering harness (about 60 lines) so the repository builds offline with no dependency. Each `*Tests.cpp` is its own executable and ctest entry. Porting to GoogleTest later is mechanical (`TEST`, `CHECK`, `CHECK_EQ` map to `TEST`, `EXPECT_TRUE`, `EXPECT_EQ`).
