# Benchmarks

Phase 8 measures the matching engine as it was at commit `f125d84` (production code unchanged by this
phase). This is measured performance on one machine, **not a production-readiness certification**.

## 1. What is measured

`lob_bench` (`benchmarks/`) is a standalone executable, separate from the unit tests and the CLI. No
external benchmarking library is used. Three layers are measured where it makes sense:

- `book`    - `OrderBook` alone (price levels, FIFO lists, id lookup);
- `engine`  - `MatchingEngine` (adds matching, trade-id allocation, trade history);
- `manager` - `OrderManager` (adds order records, in-flight recovery marker, trade-cursor sync).

The difference between layers is the cost of that layer; it is not an estimate, each layer is timed directly.

| Group | Scenarios |
|---|---|
| `core` | resting submit; cancel at best/middle/worst queue position; cancel at best/worst price level; random cancel; modify (qty decrease in place, qty increase, price change); matching one-to-one / one-to-many / many-to-one / multi-level; best bid/ask/spread/mid; depth(1/5/20/100); order lookup (book and manager); mixed workloads from the Phase 7 generator (narrow, wide, mixed profiles) |
| `scale-active` | resting submit, random cancel, lookup, bestBid, depth(10) at 1k / 10k / 100k / 250k active orders |
| `scale-levels` | the same at 100k orders spread over 1 / 10 / 100 / 1k / 10k / 50k price levels per side |
| `scale-perlevel` | cancel cost against queue length 100 / 1k / 10k / 50k at one level |
| `scale-matching` | mixed stream with 0-90% aggressive (crossing) orders |
| `scale-cancelmodify` | mixed stream with 0-50% cancels and 0-50% modifies |

158 scenarios in total (`lob_bench --list`). Workload sizes are in the `params` column of
`docs/benchmarks/baseline.csv` (for example `preload=100000;levels=1000;ops=50000`).

The mixed workloads reuse the Phase 7 seeded generator (`tests/WorkloadGenerator.h`). The generator was
extracted from `RandomizedWorkloadTests.cpp` without changing its output: twelve (digest, trades, records)
tuples are pinned by the test `SharedGeneratorOutputIsPinnedToKnownDigests`, and they were identical
before and after the extraction.

## 2. Methodology

- Clock: `std::chrono::steady_clock`. On this machine its tick is **100 ns** and one call costs about
  **185 ns**, so a single fast operation cannot be timed individually.
- **Throughput** (ops/s): one timed region per repetition containing only the operations, with no timing
  calls inside. Reported as median, min, max and coefficient of variation over **21 repetitions** after
  **2 discarded warm-up runs**. Each repetition rebuilds its state outside the timed region unless the
  scenario is read-only.
- **Latency**: per-operation timing only when an operation takes at least 2000 ns (otherwise the 185 ns
  timer cost and 100 ns quantization dominate). Cheaper operations are timed in batches of about 20 us
  and the table reports percentiles of **batch means**, labelled `bm`. A batch-mean p99 is not a
  per-operation p99; it hides single-operation outliers. Latency passes are pooled over 3 repetitions.
- Pre-generated operation streams: random workloads are recorded once (by running the shared generator
  against a scratch `OrderManager`) and replayed, so generator cost is never in a timed region.
- **Correctness is checked outside every timed region**: the book's `checkInvariants`, the manager's
  `checkConsistency`, and for replayed streams the trade count and digest against the generation pass,
  both before and after timing. A scenario that fails validation is reported and the run exits non-zero.
  All 158 scenarios validated in the final run.
- Release only. A non-`NDEBUG` build refuses to run (exit 2).
- Fixed seeds everywhere (`seed=20260601` for mixed workloads).
- The thread is pinned to logical CPU 2 (`--pin 2`).

## 3. Environment

| | |
|---|---|
| CPU | 13th Gen Intel Core i7-13620H (hybrid: P-cores and E-cores), 16 logical CPUs |
| RAM | 15.6 GB |
| OS | Windows 11 Home (10.0.26200) |
| Compiler | GCC 16.2.0 (WinLibs MinGW-w64), `-O3 -DNDEBUG`, build type Release |
| Build | CMake + Ninja |

The CPU is hybrid: E-cores ran roughly 2-3x slower than P-cores in early unpinned runs, which is why every
reported run is pinned to a P-core. Laptop power and thermal state were not controlled.

## 4. Raw data

- `docs/benchmarks/baseline.csv` - every scenario: median / min / max / cv, throughput, latency percentiles
  (machine information in `#` header lines).
- `docs/benchmarks/baseline_alloc.csv` - allocations per op, bytes per op, retained bytes per resting order
  (from `lob_alloc_profile`, a separate executable so counting never perturbs timing).

Reproduce:

```
cmake -S . -B build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel
build-rel/benchmarks/lob_bench --pin 2 --csv out.csv        # full run, about 7 minutes
build-rel/benchmarks/lob_bench --quick                      # 20x smaller smoke test, ~2 s
build-rel/benchmarks/lob_alloc_profile --csv alloc.csv
```

## 5. Baseline results (median ns/op, 100k resting orders over 1000 levels unless noted)

| Operation | book | engine | manager |
|---|---|---|---|
| resting submit | 183 | 548 | 843 |
| cancel, cache-hot queue (50k at one level) | 92-97 | ~93-98 | 112-126 |
| cancel, random order | 621 | 651 | 747 |
| modify, quantity decrease (in place) | - | - | 454 |
| modify, quantity increase (replace) | - | - | 1310 |
| modify, price change (replace) | - | - | 1190 |
| match, one-to-one | - | 288 | 745 |
| match, one-to-many (per trade) | - | 255 | 430 |
| match, many-to-one | - | 203 | 574 |
| mixed Phase 7 stream, narrow / wide / mixed profile | - | - | 676 / 736 / 705 |

Queries (manager/book as noted): `bestBid` 2.8 ns, `spread` 3.4 ns, `depth(10)` about 300 ns,
`depth(100)` 1.93 us, `book.find` 33 ns, `manager.order` 67 ns.

Throughput is the reciprocal: manager resting submit is about 1.2 Mop/s, random cancel about 1.3 Mop/s,
mixed Phase 7 stream about 1.4 Mop/s.

**Latency** (batch means, manager layer): resting submit p50 675 ns / p95 1.26 us / p99 5.38 us;
random cancel p50 726 ns / p95 804 ns / p99 1.43 us; price-change modify p50 1.16 us / p95 1.53 us /
p99 2.33 us; one-to-one match p50 548 ns / p95 878 ns / p99 3.60 us; mixed narrow p50 1.02 us / p95 1.51 us /
p99 2.36 us. The p99 of submit and match reflects batches that contain a rehash or allocator slow path.

## 6. Scaling observations

- **Active orders** (manager layer, levels = orders/10): resting submit 603 ns (1k orders) -> 805 ns (10k) ->
  1.08 us (100k) -> 1.26 us (250k). Random cancel 257 ns -> 482 ns -> 1.04 us -> 1.54 us. Both grow
  with the working set; `book.find` grows from 7 ns to 39 ns. This is consistent with cache misses on
  list nodes and hash/map nodes once the book no longer fits in cache.
- **Price levels** (100k orders): resting submit 368 ns at 1 level -> 713 ns at 1000 -> 1.44 us at 50k
  levels; random cancel 613 ns -> 740 ns -> 1.46 us. Deeper `std::map` trees and more distinct nodes
  per operation cost more.
- **Queue length at one level**: cancel cost is flat (about 112-118 ns at manager level from a queue of 100
  to 50000). Cancel does not traverse the list because the book stores list-iterator handles.
- **Cache-hot vs random cancel**: 100 ns for cancelling in queue order versus 750 ns for random order at
  100k orders. The difference is memory latency, not algorithmic work.
- **Matching intensity** (manager, mixed stream): 587 ns/op with no crossing orders, rising to
  about 800-870 ns/op at 30-90% crossing orders.
- **Cancel/modify frequency**: see the `scale-cancelmodify` rows in the CSV. These streams have a changing
  number of effective operations and a few entries (for example `cancel_pct=50`) are faster per operation
  because cancelled orders leave less resting state; they should be read as workload comparisons, not as the
  cost of one cancel.
- **Memory**: about 171 bytes retained per resting order at the book/engine layer and about 293 at the
  manager layer (which adds an order record). Resting submit allocates 2 blocks / 112 bytes per order at
  book/engine level and 3 / 248 at manager level. Cancels and lookups allocate nothing. A match allocates
  1-4 blocks per order (trade history, report, order record).

## 7. Bottlenecks identified

1. **Manager layer overhead on submit** is about 300-550 ns over the engine/book (record creation in an
   `unordered_map`, in-flight marker, trade-cursor sync). It is structural bookkeeping that provides the
   Phase 5.1 recovery guarantees.
2. **Replace-style modify** (price change, quantity increase) costs 2.6-2.9x an in-place decrease because it
   is a cancel plus a resubmit and loses time priority by design.
3. **Random-access cancel at large book sizes** is dominated by cache misses, not instructions.
4. **Allocation per resting order** (a list node and map/hash nodes) is the largest remaining cost
   per resting submit; cancels are allocation-free.
5. A cycle-counter breakdown (scratch instrumentation, not committed) of manager resting submit on the
   100k/1000-level scenario attributed roughly: record emplace ~450 cycles, engine submit ~1650 cycles of
   which the overflow pre-check ~360 cycles, `execute` ~940 cycles of which `book.add` ~770 cycles. These
   are TSC cycles from one run and are indicative only.

## 8. Optimization attempt (not retained)

**Hypothesis.** `MatchingEngine::submit` runs `restingWouldOverflow`, which looks the price level up in the
`std::map`, and then `book.add` looks it up again. For an order that cannot trade, the pre-check is
redundant with the check inside `OrderBook::add`.

**Change tried.** Skip the pre-check when the incoming order does not cross the best opposite price, and
turn an overflow refusal from `book.add` into the same `QuantityOverflow` rejection. All 412 tests passed.

**Measurement.** Interleaved A/B runs (baseline binary vs candidate, alternating order, same pinned core,
15 repetitions each, 4-8 rounds):

| Scenario | candidate / baseline (median) | rounds won |
|---|---|---|
| resting submit, engine layer | 0.83-0.84 (three independent runs: 0.84, 0.84, 0.83) | 4/4 and 6/6 |
| resting submit, manager layer | 0.86-0.87 | 4/4 and 6/6 |
| resting submit, book layer (control: code unchanged) | 0.99-1.00 | - |
| match one-to-one, engine layer | 1.05 | 1/8 |
| match many-to-one, engine layer | 1.07 | 4/8 |
| match one-to-one / many-to-one, manager layer | 1.00-1.01 | 4/8 |
| mixed Phase 7 stream (narrow / wide / mixed) | 1.02 / 0.93 / 0.96 | 1/8, 7/8, 2/8 |

**Decision: reverted.** The gain on resting submits is repeatable (about 13-17%), but it does not show up
end-to-end in the mixed workloads (differences of +2%, -7%, -4% with inconsistent rounds, so not
distinguishable from noise), it costs engine-layer matching about 5-7%, and it moves a rejection path into
`execute()`. That does not meet the bar of a repeatable, meaningful improvement without added complexity.
**No production code was changed in Phase 8; the baseline is the final measurement.**

Two A/B runs of the core group also showed the candidate uniformly 12-30% slower on scenarios it cannot
affect (for example `bestBid`, lookups). Re-running those scenarios in short isolated runs gave ratios of
about 1.0, so those uniform offsets were an artifact of long runs and are not used as evidence in either
direction.

## 9. Limitations and caveats

- One machine, one compiler, one OS. No Clang/MSVC/Linux numbers and no comparison across builds.
- Hybrid CPU with uncontrolled laptop power and thermal state; results were pinned to a P-core but not
  frequency-locked. Absolute numbers depend heavily on run mode: short filtered `--no-latency` A/B runs
  of the same binary reported lower absolute numbers (for example about 50 ns versus 92 ns for
  `cancel_in_queue`) than full-suite runs. Two full-suite runs of the unmodified binary agreed closely
  with each other (for example 92 vs 95 ns, and mixed narrow 705 vs 712 ns). Compare only numbers taken in
  the same run mode, and prefer interleaved A/B runs for any comparison. The cause of the run-mode
  difference was not determined.
- Some scenarios are intrinsically noisy (for example random cancel on a book that does not fit in cache,
  coefficients of variation of 10-150% in some runs); medians and minimums are given and conclusions are
  drawn only from repeatable differences.
- Batch-mean latency percentiles understate single-operation tail latency; per-operation percentiles are
  reported only for operations of 2 us or more.
- Single-threaded, in-memory, no I/O; the CLI and parsing are not benchmarked.
- Allocation byte counts are requested bytes, excluding allocator overhead.
- A Debug build of `lob_bench` was built and run once to confirm it refuses to produce numbers (it prints
  the Release-only error and exits with code 2). No Debug timings exist, by design.
