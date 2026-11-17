# Architecture

> Status: Phases 1-2 implemented. Sections marked *(planned)* describe code that does not exist yet; see [PROGRESS.md](PROGRESS.md).

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

## Domain models (implemented, Phase 2)

### Order (`include/Order.h`, `src/Order.cpp`)
- The constructor is private. `Order::create(...)` returns `std::optional<Order>`, so an Order that exists is always valid: id > 0, price > 0, quantity > 0, `0 <= remaining <= original`, and `executed == original - remaining`. `Order::validate` reports the first failing check as a `RejectReason`, in the order id, price, quantity.
- Fields: id, side, type, price (ticks), original/remaining quantity, `sequence` (the time-priority counter assigned by the engine) and status. The model never reads a clock, so tests and replays are deterministic.
- Mutators are `fill(qty)`, `cancel()` and `reject()`. Each one validates the transition with `canTransition` before changing anything, so a failed call leaves the order untouched.
- Misuse (a bad fill quantity, or acting on a terminal order) throws `std::invalid_argument` / `std::logic_error`. External input is checked up front and reported through `create` / `validate`, so these exceptions mean a caller bug, not bad user data.

### Trade (`include/Trade.h`, `src/Trade.cpp`)
- Immutable: there are only getters, and construction goes through `Trade::create`, which returns `std::nullopt` unless the trade id, maker id and taker id are > 0, maker != taker, and price and quantity are > 0.
- It stores maker (resting), taker (incoming) and the **taker side**. `buyOrderId()` and `sellOrderId()` are derived from that, so every trade traces back to its buyer and seller.
- The resting-price execution rule is the matching engine's job (Phase 4). `Trade` only guarantees the price is valid.

### PriceLevel (`include/PriceLevel.h`, `src/PriceLevel.cpp`)
**Ownership.** The level owns its orders by value in a `std::list<Order>`. There are no raw pointers and no shared ownership.

**Iterator strategy.** `std::list` keeps iterators and references to other elements valid when one element is erased or others are appended. `add()` returns an `OrderHandle` (a `list::const_iterator`) that the OrderBook will store in its id -> location map. `remove(handle)` can then erase from the middle of the queue in O(1). A handle stays valid until the order it refers to is removed (by `remove`, `popFront`, or a `fill` that completes the order). Using a handle after that, or on a different level, is a precondition violation and is not detected, because a membership check would cost O(n). The OrderBook owns that guarantee.

**Aggregate integrity.** Orders are exposed read-only (`front()`, `begin()`/`end()`). The only way to change a resting order's quantity is `PriceLevel::fill`, which updates the cached `totalQuantity` in the same call. A fill that completes an order also removes it, so a filled order can never keep resting. `checkInvariants()` recomputes the sum and verifies every order is active, positive and at the level's price (for tests and the Phase 3 invariant checker).

**Failure behaviour.** Preconditions throw and leave the level unchanged. These are `add` with a wrong price or an inactive order, `front`/`popFront` on an empty level, `remove`/`fill` with `end()`, and a bad fill quantity.

| Operation | Time | Notes |
|---|---|---|
| `add` | O(1) | push_back, one node allocation |
| `front`, `empty`, `orderCount`, `totalQuantity`, `price` | O(1) | `std::list::size()` is O(1) since C++11 |
| `popFront` | O(1) | |
| `remove(handle)` | O(1) | the whole point of the list and stored-handle design |
| `fill(handle, qty)` | O(1) | erases the node only if the order completes |
| iteration | O(n) | n = orders at this level |
| `checkInvariants` | O(n) | test and debug use only |

Space is O(n) for n resting orders: one list node per order (the `Order` plus two links and allocator overhead), plus O(1) for the level itself. `std::list` has worse cache locality than a contiguous container. That is the accepted trade-off for O(1) middle removal and stable handles, and Phase 8 will measure it.

## Data structure plan *(planned, Phase 3)*

| Structure | Purpose | Why |
|---|---|---|
| `std::map<Price, PriceLevel, std::greater<>>` (bids) | Price levels, best bid first | Ordered iteration gives price priority. `begin()` is the best bid, O(log P) insert/erase. |
| `std::map<Price, PriceLevel>` (asks) | Price levels, best ask first | Same, ascending. |
| `std::list<Order>` inside each `PriceLevel` | FIFO queue at one price | List iterators stay valid when other elements are erased, so removing an order from the middle is O(1) with a stored iterator. `std::deque` was the suggested alternative, but erasing from the middle of a deque is O(n) and invalidates iterators. See the decision below. |
| `std::unordered_map<OrderId, OrderLocation>` | Order ID -> (side, price, list iterator) | Average O(1) cancel/modify lookup and O(1) duplicate-ID detection. |
| `std::vector<Trade>` | Append-only trade history | Cache-friendly sequential storage. `Trade` is immutable and copyable (not assignable), which is fine for an append-only log. |
| `std::optional` | Optional results (for example best bid on an empty side) | Avoids sentinel values. |

### Decision: `std::list` rather than `std::deque` for the level queue

The specification lists both. A deque gives better cache locality but cannot delete from the middle in O(1) and does not keep iterators valid across erase. Order cancellation at an arbitrary queue position is a core requirement, and the order lookup map must hold stable handles, so `std::list` is the correct primary choice. `std::deque` is not forced into the design where it would be wrong. If the benchmarks (Phase 8) justify it, a deque with lazy deletion can be evaluated and documented as an experiment.

### Priority rules *(to be implemented, Phase 5)*

- Price priority first, then arrival sequence.
- Trades execute at the resting order's price.
- Modify: a price change cancels and re-enters the order with a new sequence (loses priority). A quantity increase also loses priority. A quantity decrease keeps priority and the order stays in place. Modifying a terminal order is rejected.

## Testing approach

`tests/TestFramework.h` is a small self-registering harness (about 60 lines) so the repository builds offline with no dependency. Each `*Tests.cpp` is its own executable and ctest entry. Porting to GoogleTest later is mechanical (`TEST`, `CHECK`, `CHECK_EQ` map to `TEST`, `EXPECT_TRUE`, `EXPECT_EQ`).
