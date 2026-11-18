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
| 7 - Comprehensive testing and edge-case validation | Complete |
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

## Phase 7 - comprehensive testing and edge-case validation

**Objective:** establish reproducible evidence that the whole system (Order, Trade, PriceLevel, OrderBook, MatchingEngine, OrderManager, CommandParser, CLI, recovery) behaves correctly under normal, complex, adversarial, randomized and failure conditions, without redesigning anything. All new code is test-side except one narrow production fix (see "Bug found").

**Method** (details in [ARCHITECTURE.md](ARCHITECTURE.md), "Validation architecture"):
- an **independent reference model** (`ReferenceExchange.h`) of matching and the order lifecycle that shares no code with production, itself checked against hand-computed answers first;
- a **system-level invariant auditor** (`SystemAuditor.h`) that reads only the public API, checks lifecycle<->book agreement, quantity reconciliation, trade associations, book integrity, stale-history accuracy and transitions between snapshots, and is itself proven by corrupting its own snapshots;
- a **`Duel`** harness that runs every operation through production and the model and compares reports, touched orders, the whole book and the trade history (and runs the audit), printing a full reproduction on the first mismatch.

**New suites** (counts are individual test cases; operation counts are given separately):

| Suite | Tests | What it covers |
|---|---:|---|
| SystemInvariantTests | 24 | The auditor accepts healthy systems and every growing-history step, and detects each class of corruption (A1-A7, B1-B4, C1-C5, D1, E1-E6, T1-T4, stale-history handling) |
| ReferenceModelTests | 20 | 10 hand-computed checks of the model itself; 9 deterministic production-vs-model scenarios (price improvement, FIFO, multi-level sweeps, sell side, lifecycle, modify-into-book, id policy, ping-pong, hub order); 1 proof that the harness detects divergence |
| RandomizedWorkloadTests | 11 | Deterministic workloads vs the model with the auditor (see below), a test that they really reach the interesting states, a reproducibility test, and the CLI driven by random text and compared with the model |
| BoundaryTests | 44 | Numeric limits (min/max price and quantity, `INT64_MAX` mid-price, effective-quantity overflow, id and trade-id exhaustion), the level-overflow regression tests at every layer, matching boundaries (just above/below/at the touch, one-to-many, many-to-one, sweeps), lifecycle boundaries (duplicate/terminal/unknown ids, every modify variant, priority preservation and loss), book boundaries (empty, single, removals, queue positions, 4000 price levels), and parser/CLI boundaries including a 30,000-token numeric property test against an independent validator |
| AdversarialTests | 11 | Long structured sequences: churn at one level, empty/non-empty oscillation, best-level recreation, 1500 repeated modifications, partial-fill-then-cancel/modify loops, same-price crowds, a moving market, hub orders with hundreds of counterparties, 8002 recorded orders, scripted lifecycle combinations |
| SystemFaultTests | 11 | System invariants under injected allocation failures with exact allocation accounting (see below) |
| CliEndToEndTests | 16 | The real `lob_cli` executable as a subprocess: the full scripted session, execution at the resting price, empty stdin, EOF, `exit`/`quit`, `--help`, unknown flags (exit 2), `--quiet`, bad arguments, repeated commands, "never reports success for a failed operation", stdout/stderr separation, CRLF, overlong lines, a 2000-command script |

**Randomized workloads** (fixed seeds, `splitmix64` generator, so a seed reproduces the same workload on any platform; each step is one operation or a burst of 3-6 same-price submissions; mix: ~30% generated-id submits, 10% client-id submits, 3% duplicate ids, 5% invalid submissions, 15% cancels (mostly of live orders), 22% modifies, 8% same-price clusters, 7% aggressive sweeps):

| Size | Steps per workload | Seeds | Profiles | Checked |
|---|---:|---|---|---|
| small | 60 | 1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233 | narrow, wide, mixed | model comparison + full audit after **every** operation |
| medium | 500 | 7, 42, 2024, 99991, 314159, 271828 | narrow, wide, mixed | model comparison + full audit after **every** operation |
| large | 6000 | 20260101, 8675309 | narrow, wide | touched orders every operation; book comparison + full audit every 200 operations |

That is 2,160 + 9,000 + 24,000 = 35,160 workload steps against the model, plus 4,500 random CLI commands (seeds 11, 222, 3333), 800-step reproducibility workloads (seeds 123456, 123457), and 30,000 + 5,000 random numeric tokens and 6,000 random byte lines through the parser. A failure prints the seed, operation number, the operation (type, id, side, price, quantity) and expected vs actual state.

**Fault injection** (`SystemFaultTests`, own executable with an isolated `operator new` replacement, linked statically so every allocation is injectable and counted):
- 6 randomized workloads of 350 operations (2,100 operations), half of them with an injected failure (single, persistent or double, at allocation positions 1-10), audited after **every** operation and again after recovery. Measured over the run: 1,002 operations with an injected failure, 177 threw, 47 returned normally with history left pending, 141 failed submits were rolled back completely (strong), 29 orders were lost to a failure and reported as `SystemFailure` (basic), 27 starved recovery attempts. The test asserts each of these classes occurred.
- Beyond the invariants, it checks that no order vanishes or is cancelled except as a consequence of its own failed operation (including a lost order completed later by the next call), the trade list never shrinks, and an accepted order's id is never reusable.
- Recovery sweeps (every allocation position, in single, persistent and double-failure modes, with 3 starved recovery attempts and 2 healthy recoveries after each): crossing submit with a remainder (70 failure plans), cancel/replace that crosses after prior executions (28), quantity increase that loses priority (6), price change (9).
- All business rejections (duplicate/terminal/unknown ids, no-change, bad values) are shown to allocate **nothing**, to behave identically when every allocation fails, and to change nothing.
- Every kind of mutating call (cancel, modify, submit) heals pending history before acting; repeated recovery (5x) never duplicates a trade or a record.
- **Allocation accounting** shows the counter works and that a normal workload and two failure-injected workloads (600 operations) free exactly what they allocate (no leak, no double free on the failure and recovery paths).

**Bug found and fixed**
- *Level aggregate overflow.* Root cause: `PriceLevel::add` did `totalQuantity_ += remaining` with no bound, and nothing above it limited the sum. Reproduction: `submit(1, SELL, 100, INT64_MAX)` then `submit(2, SELL, 100, 1)`: the second was accepted, the level reported a total of `-2` (signed overflow, undefined behaviour), `checkConsistency()` flagged it, and the program trapped under UBSan. Found by auditing the arithmetic while designing the numeric-boundary tests, then confirmed empirically. Fix: an engine pre-check (rejects with the new `RejectReason::QuantityOverflow` before any trade), an `OrderBook::add` check and a `PriceLevel::add` precondition guard (see ARCHITECTURE.md). Regression tests: `LevelAggregateOverflowIsRejectedNotWrapped`, `LevelAggregateAtExactlyMaxIsAllowed`, `LevelOverflowOnlyAffectsTheSameSideAndPrice`, `LevelCapacityIsReleasedWhenOrdersLeave`, `ModifyingIntoAnOverflowingLevelIsRejectedAndLeavesTheOrderAlone`, `ModifyReplacingAnOrderInItsOwnLevelDoesNotCountItselfTwice`, `IncomingOrderTooLargeForItsOwnLevelIsRejectedBeforeAnyTrade`, `LevelAndBookRejectOverflowDirectly`, `BookRejectsLevelOverflowOnTheBidSideToo`.
- No other production defect was found. Every other discrepancy during development was a mistake in the new tests or tooling (for example an auditor sum that itself overflowed at extreme quantities, and substring checks that matched a failure message), fixed in the tests and not by weakening any assertion.

**Mutation check (scratch copies, not committed):** 14 deliberate production bugs (executing at the incoming price, FIFO broken, aggregate not updated on a fill, each of the three overflow guards removed, a cancel leaving a stale lookup entry, a wrong mid-price, a forgotten cancelled quantity, a trade applied to one side only, a quantity decrease losing priority, no-change accepted, `reduceTo` not adjusting the original quantity, a flipped taker side) were each caught by at least one of ReferenceModelTests, BoundaryTests and SystemInvariantTests. (One batch first showed three "survivors" because my script compiled BoundaryTests without the CLI sources; with that fixed all three were detected.)

## Test results (GCC 16.2.0 / Ninja, `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror`, zero warnings)

Figures are **individual test cases** (each `TEST(...)`), as printed by each suite. Randomized and stress tests run many operations inside one test case; those operation counts are in the Phase 7 section above.

| Suite | Tests | Category |
|---|---:|---|
| OrderTypesTests | 9 | existing regression |
| OrderTests | 18 | existing regression |
| TradeTests | 8 | existing regression |
| PriceLevelTests | 21 | existing regression |
| OrderBookTests | 48 | existing regression |
| MatchingEngineTests | 34 | existing regression |
| OrderManagerTests | 41 | existing regression |
| FaultInjectionTests | 20 | existing regression (fault injection, Phase 5.1) |
| CommandParserTests | 27 | existing regression |
| CliTests | 37 | existing regression |
| CliRecoveryTests | 11 | existing regression |
| SystemInvariantTests | 24 | system invariants (auditor meta-tests) |
| ReferenceModelTests | 20 | reference model |
| RandomizedWorkloadTests | 11 | randomized |
| BoundaryTests | 44 | boundary |
| AdversarialTests | 11 | adversarial |
| SystemFaultTests | 11 | fault injection (system level) |
| CliEndToEndTests | 16 | CLI end to end |
| **Total** | **411** | 274 existing + 137 new, in 18 suites |

All 411 pass in the default Release configuration and in every configuration below. Nothing in the 274 existing tests was modified; `TestFramework.h` gained an additive `CHECK_MSG` macro and slow-test timing annotations.

## Compiler, sanitizer and build coverage

All configurations use `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror` and compile with **zero warnings**; each was built in its own clean directory and run through CTest (18/18 suites).

| Configuration | Result |
|---|---|
| GCC 16.2.0 Release (`-O3`) | 411/411 tests pass (about 18 s) |
| GCC 16.2.0 Debug (`-O0`) | 411/411 tests pass (about 55 s) |
| Release + **UBSan, trap-on-error** (`-fsanitize=undefined -fsanitize-undefined-trap-on-error -fno-sanitize-recover=all`) | 411/411 pass, no trap |
| Debug + **`_GLIBCXX_DEBUG`** (checked containers/iterators, pedantic) **+ UBSan trap** | 411/411 pass, no abort and no trap |
| Allocation accounting (SystemFaultTests, statically linked libstdc++) | normal and failure-injected workloads free exactly what they allocate |

**What was not available, and what the above does and does not mean:**
- **AddressSanitizer: not run.** `libasan` is not shipped with this MinGW-w64 GCC (the link fails with "cannot find -lasan"). No memory-error claim is made on its basis.
- **UBSan runtime: not available** (`libubsan` missing). UBSan was run in **trap-on-error** mode, which needs no runtime: the checks are compiled in and a violation aborts the program with an illegal-instruction trap and no diagnostic message. I confirmed the instrumentation is live with a deliberate signed overflow, and it did trap on the level-overflow defect before the fix. A pass means no checked undefined behaviour was *executed* by these tests; it is not exhaustive.
- **Clang: not installed**, so no Clang build was made. **MSVC: not available.** Valgrind: not available. Nothing was installed.
- Leak and double-free evidence therefore comes from the allocation accounting in `SystemFaultTests` (every `operator new`/`delete` is counted, with the library statically linked so none bypass it), which covers the failure paths, not from a sanitizer.

**Line coverage (gcov, Debug, all 18 suites; informative, not a target).** Measured on the final code: see the table below. Uncovered lines are almost entirely code that cannot be reached through the public API: internal-corruption `logic_error` guards, the failure-message branches of `checkInvariants()` / `checkConsistency()` (they need a corrupt state), sequence-counter exhaustion (2^64 events), and defensive branches for orders the public API cannot construct. The coverage run did expose three genuine gaps, which were closed (bid-side overflow rejection at the book, trade-id exhaustion for an incoming sell, and the engine's direct `replace` rejections).

| Source file | Lines (gcov) | Executed |
|---|---:|---:|
| `Order.cpp` | 54 | 100.00% |
| `Trade.cpp` | 17 | 100.00% |
| `PriceLevel.cpp` | 54 | 98.15% |
| `MatchingEngine.cpp` | 139 | 97.84% |
| `OrderManager.cpp` | 324 | 92.28% |
| `OrderBook.cpp` | 352 | 88.35% |
| `CommandParser.cpp` | 225 | 98.67% |
| `Cli.cpp` | 251 | 93.63% |

(gcov counts each template instantiation's lines, so `OrderBook.cpp`, whose bid/ask code is templated, shows its unreachable guards several times over.)

## Known limitations

- **Not a proof.** The validation shows the code agrees with an independent model and keeps its invariants on everything exercised (fixed seeds, bounded sizes, hand-written boundaries); it cannot show the absence of bugs, and the reference model could share a misunderstanding of the specification with the code (it was written from the documented behaviour, not from the code, and checked by hand first).
- **Sanitizers:** no AddressSanitizer, no Clang, no MSVC (see above); UBSan only in trap mode.
- **Auditor visibility:** it sees only the public API. It needs the caller to supply the ids to audit, cannot see the internal lookup or list structure except through `OrderBook::checkInvariants()`, and the failure branches of the production diagnostics cannot be reached without a corrupt state, so they have no committed test.
- **Sequence-number exhaustion** (2^64 operations) is unreachable and untested.
- **Fault injection** models `std::bad_alloc` only (single, persistent and double failures; no triple failures, no crashes, no non-heap exhaustion). `FaultInjectionTests` and `CliRecoveryTests` use the shared libstdc++ DLL, so `std::string` allocations inside the DLL are not injectable there (production builds strings only on error paths); `SystemFaultTests` is static and covers them.
- **Basic, not strong, guarantee** remains for a submit that fails after it traded and for a cancel/replace that fails after the order left the book (the order can be lost and is reported as `SystemFailure`); making this strong needs a redesign judged disproportionate.
- **Overflow rule is conservative:** an incoming order is rejected with `QuantityOverflow` if, assuming it all rested, its own side's level at that price would overflow, even if it would in fact have matched. This only matters for quantities within the order's size of `INT64_MAX`.
- **The CLI end-to-end suite cannot produce stale history** in the real executable (it needs simulated memory exhaustion); recovery behaviour is covered in-process by `CliRecoveryTests` and, through the `recover` command on a healthy session, end to end.
- **Randomized workloads are bounded:** the largest is 6,000 steps per seed, kept small enough for routine runs (about 10 s of the 18 s Release total, 36 s of the 55 s Debug total).
- History is never pruned; orders are limit orders only; prices are whole ticks; execution timestamps are the taker's sequence. Tested with GCC only.
- `benchmarks/` is empty. No benchmark numbers exist, and no performance claim is made.
