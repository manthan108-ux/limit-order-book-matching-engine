# Progress

| Phase | Status |
|---|---|
| 1 - Foundation and architecture | Complete |
| 2 - Domain models (Order, Trade, PriceLevel) | Complete |
| 3 - OrderBook state and validation | Complete |
| 4 - Matching engine and trade generation | Complete |
| 5 - Order lifecycle management (OrderManager) | Complete |
| 5.1 - Reliability hardening (fault injection) | Complete |
| 6 - CLI (interactive command line) | Complete |
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

`OrderManager`: the lifecycle layer above the engine. It owns the order-id policy (ids unique for the life of the manager, generated or client-supplied, no wrap), one value-based `OrderRecord` per order, `submit` / `cancel` / `modify`, per-order trade history (`tradesFor`), and `checkConsistency()`. Modification rules: a price change or quantity increase loses time priority (cancel/replace, which may trade if it crosses), a quantity decrease keeps priority, no change is rejected as `NoChange`; quantity means *remaining* quantity and cumulative executed quantity is never altered. Trade history: the engine owns the global list, and records are updated only by a cursor-based `syncTrades()`, so each trade is applied exactly once to its buyer and seller. Lower layers gained additive operations (`Order::reduceTo` / `replaced`, `PriceLevel::reduce`, `OrderBook::extract` / `reduce`, `MatchingEngine::cancel` / `reduce` / `replace`). See [ARCHITECTURE.md](ARCHITECTURE.md).

## Phase 5.1 - reliability hardening

Audit of the Phase 5 exception-safety limitations, driven by a deterministic allocation-failure injector (test-only; details and the full guarantee table are in [ARCHITECTURE.md](ARCHITECTURE.md)).

**Defects found and fixed** (all in `OrderManager`):
1. Recovery after a failure ran inside a `catch` block. If memory was still exhausted it gave up silently and nothing retried it, leaving a lifecycle record ACTIVE while the order was absent from the book, with trades unapplied.
2. A `bad_alloc` while applying trades to the records, after the engine had already finished, made `submit` / `modify` throw even though the order was resting and its trades had executed. A retry would have been rejected as a duplicate id.
3. A submit that failed before executing anything left a CANCELLED record and burned its id, when a clean rollback was possible.

**Changes:** a plain-data in-flight marker, completed at the start of the next mutating call; deferral of a failed history update after a successful engine operation (the engine's report is returned); rollback of a submit that never traded; a pending-aware `checkConsistency()`; and two small public methods, `recover()` and `isHistoryCurrent()`. No Phase 1-4 behaviour changed, and no existing test was modified.

**Also found while verifying:** a latent undefined-behaviour bug in my own new test (dereferencing an empty `std::optional`). It passed silently in Release and was caught by running the suite in a Debug build with libstdc++ checks. It is fixed.

**Tests:** `FaultInjectionTests` (20 tests) fails every allocation of each of these operations, one at a time (single failure), with memory staying exhausted (persistent), and with a second failure during recovery (double): `OrderBook::add` (books of 0..30 orders), `MatchingEngine::submit`, manager submit (resting, generated id, rejected record, crossing and fully filled, crossing then rest), history application (deferred, interrupted part-way, resumed), modify (replace without and with crossing, keeping executed quantity), and the queries. It also proves that `cancel`, in-place quantity reduction and `OrderBook::extract` / `cancel` / `reduce` / `fill` perform zero allocations. After every failure point it checks the book invariants and the manager consistency audit, that no order is active twice, no record lies about an absent order, each trade is applied exactly once per side and none is lost, ids are not reused after an accepted order, and executed quantity never decreases.

**Not changed (by decision):** history is retained for the manager's lifetime and there is no pruning API; `order()` returns a copy; `NoChange` and zero-quantity modify are rejections; priority rules are as before. These contracts are now documented in ARCHITECTURE.md.

## Phase 6 - command line interface

The interactive terminal application (see [ARCHITECTURE.md](ARCHITECTURE.md), "Command line interface", and the README for usage).

- **`CommandParser`** (pure, no I/O): tokenizes a line and validates syntax and numbers with `std::from_chars` (strict whole tokens; no rounding, truncation or wrap-around; decimals, `+5`, `1e3`, trailing junk and out-of-range values are rejected with specific messages). Case-insensitive commands and sides, whitespace-tolerant, line length limit 1024, depth limit 1000.
- **`Cli`**: dispatches parsed commands to `OrderManager` and prints results as plain fixed-width tables (no ANSI, no terminal assumptions). It holds no order state, never touches book containers, never matches, and never duplicates lifecycle rules. It never throws; `run()` ends cleanly on `exit`, `quit` or end of input.
- **Commands**: `submit`, `cancel`, `modify`, `book`, `depth`, `best`, `order`, `trades`, `recover`, `help`, `exit`, `quit`.
- **Phase 5.1 integration**: `order` and `trades` warn and label their output "possibly out of date" while `isHistoryCurrent()` is false; `recover` calls `OrderManager::recover()` once and reports complete / already current / incomplete (retryable); a trading command that throws is reported as "may already have executed, do NOT resubmit, run recover", and the CLI never retries anything.
- **Build**: a new static library `lob_ui` (parser + CLI) links `lob_core`; `lob_cli` and the tests link the libraries, so no source is compiled twice. `FaultInjection.h` gained a `Pause` guard (test-only) used by the recovery tests.
- **No Phase 1-5.1 source file was modified.** (Only the CMake lists and `tests/FaultInjection.h` were extended.)

**Tests** (75 new): `CommandParserTests` (27) parses every command and rejects every malformed form, including overflow, decimals, `+`, exponent, whitespace, case, extreme values and a 5000-character token; `CliTests` (37) drive the real CLI and manager through in-memory streams (submit with generated/client/duplicate ids, crossing and partial fills, cancel incl. partial, modify priority semantics, book/depth/best including empty and one-sided books and the half-tick mid-price, `order`/`trades` for resting/filled/cancelled/rejected orders, help, `exit`/`quit`/EOF, malformed input not ending the session, CRLF, prompt on/off) and audit the manager after operations; `CliRecoveryTests` (11, own executable with the isolated fault-injection `operator new`) creates genuinely stale history (4 distinct stale states, including part-way ones) and covers stale warnings, successful recovery with no duplicate trade associations, incomplete recovery under still-exhausted memory, retry, idempotence, no re-execution, auto-healing by a mutating command, and failed `submit`/`modify` being reported and not retried. I also confirmed the tests catch bugs: scratch copies of the CLI/parser with no stale warning, a fake `recover`, truncated decimals, accepted zero price, wrapped overflow, inverted priority message, reversed ask order and a wrong cancelled quantity each fail the relevant suite (not committed).

## Test results (clean build, GCC 16.2 / Ninja, `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror`, zero warnings)

| Suite | Tests |
|---|---|
| OrderTypesTests | 9 |
| OrderTests | 18 |
| TradeTests | 8 |
| PriceLevelTests | 21 |
| OrderBookTests | 48 |
| MatchingEngineTests | 34 |
| OrderManagerTests | 41 |
| FaultInjectionTests | 20 |
| CommandParserTests | 27 |
| CliTests | 37 |
| CliRecoveryTests | 11 |
| **Total** | **274** (199 before Phase 6 + 75 new) |

All 274 pass in the default Release configuration (`-O3`) and in a separate Debug configuration (`-O0`, libstdc++ assertions), both with warnings as errors.

## Compiler coverage

- **GCC 16.2.0** (WinLibs, MinGW-w64, UCRT, POSIX threads) with Ninja: Release and Debug, all tests pass.
- **Clang: not installed**, so it was not tested. Nothing was installed to change that.
- **MSVC: not available** in this environment (the Visual Studio directories contain no compiler).
- The pre-installed MinGW GCC 6.3 cannot compile C++17 and is not used.
- No compiler-specific constructs are used apart from the `noinline` attribute on the test-only allocation operators (guarded for GCC/Clang and MSVC).

## Known limitations

- Allocation-failure behaviour is verified for `std::bad_alloc` injected through `operator new` (single, persistent and double failures). Other failure types, interleavings of three or more failures, non-heap resource exhaustion and crashes are not modelled.
- Where a failure happens after trades have executed, or after a modify has taken the order out of the book, the guarantee is **basic**: the order may be lost (marked CANCELLED with `SystemFailure`, never restored) and the exception is rethrown. Making this strong would need a redesign (for example allocation-free node moves between price levels) and was judged disproportionate.
- After a failure, trade ids can have gaps, and `isHistoryCurrent()` may be false until the next mutating call or `recover()`.
- History is never pruned: one `OrderRecord` per id ever used, plus all trades, grow without bound.
- `OrderManager::order()` returns a copy of the record, including its trade-index vector.
- Orders are limit orders only; execution timestamps are the taker's `sequence` (no wall clock); a cross between resting orders added directly to an `OrderBook` is not uncrossed.
- Cancel and execution costs are O(log P) + average O(1) per order, not O(1).
- Only GCC has been tried (see Compiler coverage).
- The CLI reads a whole input line before checking its length (a very long line is rejected but still buffered).
- `benchmarks/` is empty. No benchmark numbers exist yet, and no performance claim is made.
- CLI syntax limits: `modify` always takes both a price and a remaining quantity (the manager also supports changing only one); prices are whole ticks only (no decimal prices or tick-size conversion); there is no command history, line editing or script file option (use shell redirection); a generated id cannot be shown if a `submit` fails with an exception.
- CLI tests cover stale-history behaviour by simulating allocation failure (the only way to produce it). The CLI formatting itself is not pinned by exact-layout tests, by design.
