# Verification

What was tested, how, what the results were, and what was **not** verified. Every figure below was either
reproduced when this document was written or is marked otherwise. The design being verified is described in
[ARCHITECTURE.md](ARCHITECTURE.md); performance is covered separately in [BENCHMARKS.md](BENCHMARKS.md).

These results show that the code behaves correctly on everything exercised. They are not a proof of correctness
and not a production-readiness certification.

## Reproducing the results (Windows 11 verified, Windows 10 not verified; PowerShell or `cmd.exe`)

```
cmake -S . -B build -G Ninja -DLOB_WARNINGS_AS_ERRORS=ON
cmake --build build
cd build
ctest --output-on-failure
cd ..
```

Each suite is its own executable in `build\tests\` and can be run directly. `CliEndToEndTests` takes the path of
`lob_cli.exe` as its first argument (CTest supplies it). `ctest` is run from inside `build` instead of using
`ctest --test-dir build`, which needs CMake 3.20 (the project file declares 3.14). The MinGW runtime DLLs must be
found through `PATH`; see "Missing GCC runtime DLLs" in the [README](../README.md#missing-gcc-runtime-dlls).

## Test suites: 420 tests in 18 suites

Counts are individual `TEST(...)` cases, re-counted per suite from a fresh clone. Randomized and stress tests run
many operations inside one test case; operation counts are given separately below.

| Suite | Tests | Category | What it covers |
|---|---:|---|---|
| OrderTypesTests | 9 | unit | enums, lifecycle transitions, validation helpers |
| OrderTests | 18 | unit | `Order` construction, fills, cancel, amendments, invalid transitions |
| TradeTests | 8 | unit | `Trade` construction and buyer/seller derivation |
| PriceLevelTests | 21 | unit | FIFO queue, handles, aggregate quantity, `reduce`, `fill`, invariants |
| OrderBookTests | 48 | unit | add/cancel/extract/fill/reduce, market data, depth, invariant checker, move semantics |
| MatchingEngineTests | 34 | unit | price-time priority, resting-price execution, sweeps, trade ids, cancel/replace |
| OrderManagerTests | 41 | unit | id policy, lifecycle records, modify rules, trade history, consistency audit |
| FaultInjectionTests | 20 | fault injection | every allocation of each operation failed in turn (see below) |
| CommandParserTests | 27 | unit | tokenizing, strict number parsing, arity, limits |
| CliTests | 44 | unit | the real `Cli` and manager through in-memory streams |
| CliRecoveryTests | 11 | fault injection | stale history, warnings, `recover`, failed trading commands |
| SystemInvariantTests | 24 | validation | the auditor accepts healthy states and detects each class of corruption |
| ReferenceModelTests | 20 | validation | the reference model against hand-computed answers, then production against the model |
| RandomizedWorkloadTests | 12 | randomized | seeded workloads vs the model, the CLI under random text, pinned generator digests |
| BoundaryTests | 44 | boundary | numeric limits, overflow, matching boundaries, lifecycle boundaries, parser limits |
| AdversarialTests | 11 | adversarial | long structured sequences (churn, hub orders, 1500 modifications, 8002 orders) |
| SystemFaultTests | 11 | fault injection | system invariants under injected failures, with allocation accounting |
| CliEndToEndTests | 17 | end to end | the real `lob_cli.exe` as a subprocess |
| **Total** | **420** | | |

How the validation layer works (reference model, system auditor, duel harness, fault injector) is documented in
[ARCHITECTURE.md](ARCHITECTURE.md).

## Results by configuration

All configurations use `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion`; those marked
`-Werror` were built with `-DLOB_WARNINGS_AS_ERRORS=ON`. Compiler: GCC 16.2.0 (WinLibs MinGW-w64, UCRT);
CMake 4.4.4; Ninja 1.13.2; Windows 11. Each configuration was built in a clean directory.

| Configuration | Result |
|---|---|
| Release, `-Werror` | 0 warnings; 18/18 suites; **420/420 tests** (about 18 s) |
| Debug, `-Werror` | 0 warnings; 18/18 suites; **420/420 tests** (about 56 s) |
| Release + UBSan in trap-on-error mode (`-fsanitize=undefined -fsanitize-undefined-trap-on-error -fno-sanitize-recover=all`), `-Werror` | 0 warnings; 18/18 suites, no trap |
| Debug + `_GLIBCXX_DEBUG` and `_GLIBCXX_DEBUG_PEDANTIC` (checked containers and iterators) | 18/18 suites, no abort |

The 420-test count was taken per suite in Release and in Debug. The UBSan and `_GLIBCXX_DEBUG` rows were last run
when the suite had 412 tests (before the byte-order-mark and duplicate-id changes added 8) and were **not** re-run
since; they are not evidence about the newer tests. Release-without-`-Werror` is the default build and also
produces 0 warnings.

## Line coverage (gcov, Debug, all 18 suites)

Reproduced from a clean build with `--coverage`. Informative, not a target.

| Source file | Lines | Executed |
|---|---:|---:|
| `Order.cpp` | 54 | 100.00% |
| `Trade.cpp` | 17 | 100.00% |
| `PriceLevel.cpp` | 54 | 98.15% |
| `MatchingEngine.cpp` | 139 | 97.84% |
| `OrderManager.cpp` | 324 | 92.28% |
| `OrderBook.cpp` | 352 | 88.35% |
| `CommandParser.cpp` | 225 | 98.67% |
| `Cli.cpp` | 251 | 93.63% |

gcov counts each template instantiation's lines, so `OrderBook.cpp` (templated over the bid and ask maps) shows
its unreachable guards several times. The uncovered lines are mostly code that cannot be reached through the
public API: internal-corruption `logic_error` guards, the failure-message branches of `checkInvariants()` and
`checkConsistency()` (they need a corrupt state), sequence-counter exhaustion (2^64 events), and defensive
branches for orders the public API cannot construct.

## What the randomized and adversarial tests run

Fixed seeds, `splitmix64` generator, so a seed reproduces the same workload on any platform. Each step is one
operation or a burst of 3-6 same-price submissions. Mix: about 30% generated-id submits, 10% client-id submits,
3% duplicate ids, 5% invalid submissions, 15% cancels (mostly of live orders), 22% modifies, 8% same-price
clusters, 7% aggressive sweeps.

| Size | Steps per workload | Seeds | Profiles | Checked |
|---|---:|---:|---|---|
| small | 60 | 12 | narrow, wide, mixed | model comparison + full audit after **every** operation |
| medium | 500 | 6 | narrow, wide, mixed | same |
| large | 6000 | 2 | narrow, wide | touched orders every operation; book comparison + full audit every 200 operations |

That is 2,160 + 9,000 + 24,000 = 35,160 workload steps against the model, plus 4,500 random CLI commands,
30,000 + 5,000 random numeric tokens and 6,000 random byte lines through the parser. A failure prints the seed,
operation number, the operation and expected versus actual state. Twelve workload outcomes are pinned by digest
(`SharedGeneratorOutputIsPinnedToKnownDigests`) because the benchmarks reuse the same generator.

## Fault injection

A test-only replacement of the global `operator new` fails the Nth allocation made under a guard (details in
[ARCHITECTURE.md](ARCHITECTURE.md)). Three modes: a single failure, a persistent failure (memory stays
exhausted), and a second failure during recovery.

- `FaultInjectionTests` (20) sweeps every allocation of `OrderBook::add` (books of 0 to 30 orders),
  `MatchingEngine::submit`, manager submit (resting, generated id, rejected record, crossing, crossing then
  rest), history application (deferred, interrupted, resumed), modify (with and without crossing) and the
  queries, and proves that `cancel`, in-place quantity reduction and `OrderBook::extract`/`cancel`/`reduce`/`fill`
  allocate nothing.
- `SystemFaultTests` (11) runs six randomized workloads of 350 operations under injected failures, auditing after
  **every** operation and again after recovery. Output of the run when this document was written: 2,100
  operations, 1,002 with an injected failure, 177 threw, 47 returned normally with history left pending, 141
  failed submits rolled back completely, 29 orders reported as lost (`SystemFailure`), 27 starved recovery
  attempts; recovery sweeps of 70, 28, 6 and 9 failure plans. The test asserts that each class occurred.
- It also shows that normal and failure-injected workloads free exactly what they allocate (no leak, no double
  free), using allocation accounting with a statically linked standard library.

After every failure point the book passes `checkInvariants()`, no order is active twice, no lifecycle record
claims an order is active while it is absent from the book, each trade is applied exactly once to its buyer and
seller, none already committed is lost, an accepted order's id is never reused, and executed quantity never
decreases.

## Defects found by verification (all fixed)

1. **Recovery gave up silently** (`OrderManager`). Recovery after a failure ran inside a `catch` block; if memory
   was still exhausted it stopped and nothing retried it, leaving a record ACTIVE while the order was absent from
   the book and trades unapplied.
2. **A failure after the engine finished threw anyway.** A `bad_alloc` while applying trades to the records, after
   the engine had completed, made `submit`/`modify` throw although the order was resting and its trades had
   executed; a retry would have been rejected as a duplicate id.
3. **Unnecessary id burn.** A submit that failed before executing anything left a CANCELLED record and burned its
   id although a clean rollback was possible.
4. **Level aggregate overflow** (`PriceLevel`/`MatchingEngine`). `PriceLevel::add` did `totalQuantity_ += remaining`
   with no bound. Reproduction: `submit(1, SELL, 100, INT64_MAX)` then `submit(2, SELL, 100, 1)` was accepted, the
   level reported a total of `-2` (signed overflow, undefined behaviour), `checkConsistency()` flagged it and the
   program trapped under UBSan. Fixed with an engine pre-check (`RejectReason::QuantityOverflow`, before any
   trade), an `OrderBook::add` check and a `PriceLevel::add` precondition guard. Regression tests (all present in
   `tests/BoundaryTests.cpp`): `LevelAggregateOverflowIsRejectedNotWrapped`, `LevelAggregateAtExactlyMaxIsAllowed`,
   `LevelOverflowOnlyAffectsTheSameSideAndPrice`, `LevelCapacityIsReleasedWhenOrdersLeave`,
   `ModifyingIntoAnOverflowingLevelIsRejectedAndLeavesTheOrderAlone`,
   `ModifyReplacingAnOrderInItsOwnLevelDoesNotCountItselfTwice`,
   `IncomingOrderTooLargeForItsOwnLevelIsRejectedBeforeAnyTrade`, `LevelAndBookRejectOverflowDirectly`,
   `BookRejectsLevelOverflowOnTheBidSideToo`.

Separately, a latent undefined-behaviour bug in a new test (dereferencing an empty `std::optional`) passed
silently in Release and was caught by a Debug build with libstdc++ checks; it was fixed. Every other
discrepancy found while writing the tests was a mistake in the tests or tooling, fixed without weakening an
assertion.

## Mutation check (performed during development, not repeated)

Fourteen deliberate production bugs were applied to scratch copies and each was caught by at least one of
`ReferenceModelTests`, `BoundaryTests` and `SystemInvariantTests`: executing at the incoming price, FIFO broken,
aggregate not updated on a fill, each of the three overflow guards removed, a cancel leaving a stale lookup entry,
a wrong mid-price, a forgotten cancelled quantity, a trade applied to one side only, a quantity decrease losing
priority, no-change accepted, `reduceTo` not adjusting the original quantity, and a flipped taker side. A
separate set of scratch copies of the manager with the recovery fixes disabled or broken also failed the fault
suites. **The scratch copies were not committed and this check was not re-run for this document**, so it is
recorded as a development-time result, not a reproducible one.

## Not verified, and known limitations

- **Platforms and toolchains.** Only Windows 11 (build 26200) with GCC 16.2.0 / MinGW-w64, CMake 4.4.4 and Ninja
  1.13.2 was used. Windows 10, other compilers (Clang, MSVC), other CMake versions (the project file declares 3.14;
  the documented commands avoid `ctest --test-dir`, which needs 3.20, but were run only with 4.4.x) and other
  operating systems were **not tested**.
- **Runtime DLLs and `PATH`.** `lob_cli.exe` and 17 of the 18 test executables link `libstdc++-6.dll` and
  `libgcc_s_seh-1.dll` dynamically (both import `libwinpthread-1.dll`), so results depend on the right MinGW runtime being found through `PATH`. When
  the test executables were run from a shell without the WinLibs directory on `PATH`, `CliEndToEndTests`,
  `CliRecoveryTests` and `FaultInjectionTests` printed nothing (a count of 365 instead of 412 tests). All results in
  this document were produced with the WinLibs `bin` directory first on `PATH`. `SystemFaultTests` is linked
  statically and has no such dependency.
- **Sanitizers.** AddressSanitizer is unavailable with this toolchain (`cannot find -lasan`, re-confirmed), so no
  memory-error claim rests on it. The UBSan runtime library is also missing, so UBSan ran in trap-on-error mode:
  the checks are compiled in and a violation aborts with an illegal-instruction trap and no message. A pass means
  no *checked* undefined behaviour was executed; it is not exhaustive. Leak and double-free evidence comes from
  the allocation accounting in `SystemFaultTests`, which covers the failure paths. Valgrind is not available.
- **Auditor visibility.** `SystemAuditor` sees only the public API. It needs the caller to supply the ids to audit
  (the manager cannot enumerate its records), cannot see the internal lookup or list structure except through
  `OrderBook::checkInvariants()`, and the failure branches of the production diagnostics cannot be reached
  without a corrupt state, so they have no committed test.
- **Sequence-number exhaustion** (2^64 operations) is unreachable and untested.
- **Fault injection** models `std::bad_alloc` only (single, persistent and double failures; no triple failures, no
  crashes, no non-heap exhaustion). `FaultInjectionTests` and `CliRecoveryTests` link the shared libstdc++ DLL,
  where `std::string` allocates through the DLL's own `operator new` and bypasses the injected replacement;
  production code builds strings only on error paths, never on the swept operation paths. `SystemFaultTests` is
  linked statically and covers every allocation.
- **Basic, not strong, guarantee** for a submit that fails after it traded and for a cancel/replace that fails
  after the order left the book (the order can be lost and is reported as `SystemFailure`). Making this strong needs
  a redesign that was judged disproportionate.
- **Overflow rule is conservative.** An incoming order is rejected with `QuantityOverflow` if, assuming it all
  rested, its own side's level at that price would overflow, even if it would in fact have matched. This only
  matters within the order's size of `INT64_MAX`.
- **End-to-end limits.** The CLI end-to-end suite cannot produce stale history in the real executable (it needs
  simulated memory exhaustion); recovery is covered in-process by `CliRecoveryTests` and, through the `recover`
  command on a healthy session, end to end.
- **Input encoding.** The CLI ignores one UTF-8 byte-order mark at the very start of its input (as piped by
  Windows PowerShell 5.1) and nothing else; marks elsewhere are rejected as part of an unknown command. This is
  tested in `CliTests` and `CliEndToEndTests`, and was also checked once with a real PowerShell 5.1 pipe.
- **Bounded workloads.** The largest randomized workload is 6,000 steps per seed, kept small enough for routine
  runs.
- **No performance claim** beyond the measurements in [BENCHMARKS.md](BENCHMARKS.md), which come from one
  machine whose speed varied by about 2.2x between runs.
