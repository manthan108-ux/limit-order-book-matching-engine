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
- `docs/benchmarks/baseline_run1.csv` and `docs/benchmarks/baseline_run3.csv` - the first and third full runs of
  the same unmodified binary with the same settings as `baseline.csv` (the second full run). Runs 1 and 2 are in
  one machine state and run 3 in another about 2.2x faster, see section 9.
- `docs/benchmarks/ab/` - the preserved raw A/B data (and scripts) for the rejected optimization candidate,
  with its own README listing exactly what is and is not preserved. Section 8 quotes from it.
- `docs/benchmarks/runmode/` - the runs behind the full-run versus filtered-run discrepancy, section 9.

**Evidence labels used below.** *Measured* means a number in one of the CSVs above that a reader can look up.
*Inferred* means an explanation of a measured number that no experiment in this repository tests (no hardware
counters, no per-structure profiler was used). Inferred statements are marked as such and should be treated
as hypotheses.

Reproduce:

```
cmake -S . -B build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel
build-rel/benchmarks/lob_bench --pin 2 --csv out.csv        # full run, about 7 minutes
build-rel/benchmarks/lob_bench --quick                      # 20x smaller smoke test, ~2 s
build-rel/benchmarks/lob_alloc_profile --csv alloc.csv
```

## 5. Baseline results (median ns/op, 100k resting orders over 1000 levels unless noted)

**Read section 9 before using any absolute number here.** The same unmodified binary, with the same command
line, measured about **2.2x slower** in two full runs (`baseline.csv` = run 2, `baseline_run1.csv` = run 1)
than in a third full run (`baseline_run3.csv`). Both states are shown; neither is "the" speed of the engine.
Latency percentiles, the scaling sections and the allocation data below are from `baseline.csv` (run 2) unless
stated.

| Operation (ns/op) | book: run 2 / run 3 | engine: run 2 / run 3 | manager: run 2 / run 3 |
|---|---|---|---|
| resting submit (100k orders / 1000 levels) | 183 / 182 (*) | 548 / 245 | 843 / 391 |
| cancel in queue order (50k at one level, oldest first) | 91.9 / 40.4 | 91.1 / 40.9 | 112 / 51.4 |
| cancel, random order (100k / 1000) | 620 / 262 | 650 / 266 | 747 / 411 |
| match one-to-one | - | 288 / 135 | 745 / 335 |
| match one-to-many (per trade) | - | 255 / 114 | 430 / 166 |
| match many-to-one | - | 202 / 89.6 | 574 / 241 |

(*) In run 2 this scenario is the first of the run and straddles the change of machine state (cv 40%; run 1
401 ns, cv 30%), so its run-2 value is not comparable with the other cells of run 2. Only run 3, where the
whole run was in one state, gives a usable layer split: book 182, engine 245, manager 391 ns.

| Manager layer (ns/op) | run 2 / run 3 |
|---|---|
| modify, quantity decrease (in place) | 454 / 203 |
| modify, quantity increase (replace) | 1310 / 655 |
| modify, price change (replace) | 1190 / 647 |
| mixed Phase 7 stream, narrow | 676 / 289 |
| mixed Phase 7 stream, wide | 736 / 317 |
| mixed Phase 7 stream, mixed | 705 / 308 |
| `bestBid` | 2.75 / 1.20 |
| `spread` | 3.38 / 1.48 |
| `depth(100)` | 1930 / 864 |
| `book.find` | 32.8 / 10.8 |
| `manager.order` | 67.4 / 28.0 |

Throughput is the reciprocal of the median: in run 2 manager resting submit is about 1.2 Mop/s, random cancel
about 1.3 Mop/s and the mixed Phase 7 stream about 1.4 Mop/s; in run 3 about 2.6, 2.4 and 3.2-3.5 Mop/s.
Ratios between rows move less than the absolute values do, but they are not constant either:
replace-modify / in-place modify is 2.6x in run 2 and 3.2x in run 3, and random / queue-order cancel at the
manager layer is 6.7x and 8.0x. Quote a ratio as a range across states, not as a single number.

**Latency** (manager layer; `lat_method` column of `baseline.csv`). Batch means (`batch_mean`): resting
submit p50 675 ns / p95 1.26 us / p99 5.38 us; random cancel p50 726 ns / p95 804 ns / p99 1.43 us;
price-change modify p50 1.16 us / p95 1.53 us / p99 2.33 us; one-to-one match p50 548 ns / p95 878 ns /
p99 3.60 us. The mixed Phase 7 streams are timed per operation (`per_op`, quantized to 100 ns) and reported per
operation kind (`lat_group` column); narrow profile:

| Kind | p50 | p95 | p99 |
|---|---:|---:|---:|
| submit | 600 ns | 1800 ns | 5000 ns |
| cancel | 300 ns | 500 ns | 800 ns |
| modify | 300 ns | 1000 ns | 1700 ns |

(wide: submit 700 / 1800 / 5500, cancel 300 / 600 / 800, modify 300 / 1300 / 1600 ns; mixed: submit
600 / 1800 / 5300, cancel 300 / 600 / 700, modify 300 / 1100 / 1500 ns.) An earlier revision of this file
quoted a single "mixed narrow" latency of 1.02 / 1.51 / 2.36 us; those figures were mis-attributed (they are the
resting-submit batch-mean percentiles at 100k orders over 10k levels) and are corrected here.
*Inferred, not measured:* the large p99 of submit and match is probably batches that include a hash-table
rehash or an allocator slow path; nothing here confirms it.

## 6. Scaling observations

Each bullet separates what the CSV shows (*measured*) from the explanation offered (*inferred*). Numbers are
from `baseline.csv` (run 2, mostly the slow machine state) unless a run is named; section 9 explains why
absolute values carry a state factor of about 2.2x and shows that the shapes described here hold in both states.

- **Active orders** (manager layer, levels = orders/10).
  *Measured:* resting submit 603 ns (1k orders) -> 805 ns (10k) -> 1.08 us (100k) -> 1.26 us (250k); random
  cancel 257 ns -> 482 ns -> 1.04 us -> 1.54 us; `book.find` 7.3 ns -> 8.6 -> 32 -> 39 ns and `manager.order`
  20 -> 29 -> 68 -> 86 ns.
  *Inferred:* cache misses on list, hash and map nodes once the book no longer fits in cache. The growth
  fits that explanation; no cache-miss counter was read to confirm it.
- **Price levels** (100k orders).
  *Measured:* resting submit 368 ns at 1 level -> 713 ns at 1000 -> 1.44 us at 50k levels; random cancel
  613 ns -> 740 ns -> 1.46 us.
  *Inferred:* deeper `std::map` trees and more distinct nodes touched per operation.
- **Queue length at one level.**
  *Measured:* cancel cost is flat, 105-118 ns at manager level from a queue of 100 to 50000
  (`cancel_in_queue` rows of the `scale-perlevel` group). *Design fact, from the code, not a measurement:*
  `OrderBook` stores a list iterator per order, so a cancel does not walk the list.
- **Cache-hot vs random cancel.**
  *Measured:* about 112 ns cancelling in queue order versus about 747 ns for random order at 100k orders.
  *Inferred:* the difference is memory latency, not algorithmic work. No experiment here separates the two.
- **Matching intensity** (manager, mixed stream).
  *Measured:* with no crossing orders the stream costs 587 ns/op in run 2 (cv 43%) and 292 ns/op in run 3;
  at 30-90% crossing orders it costs 1.36-1.48x that in run 2 (800-871 ns/op) and 1.28-1.34x in run 3
  (374-392 ns/op). The shape (a rise from 0% to 30% crossing, then a plateau) is the same in both.
- **Cancel/modify frequency** (`scale-cancelmodify`, manager, mixed stream).
  *Measured, within one machine state only.* Run 3 (all fast): 278 ns/op at 0% cancels and 0% modifies, 319 /
  353 / 319 at 10 / 30 / 50% cancels, and 341 / 379 / 418 at 10 / 30 / 50% modifies. Run 1 (all slow) shows the
  same ordering (613; 666 / 755 / 722; 703 / 801 / 926). **Do not compare the rows of run 2 with each other
   from `cancel_pct=50` onward**: its first three rows are in the slow state and the remaining six in the fast
  state (section 9), which is why `baseline.csv` appears to show 755 ns falling to 318 ns.
  *Inferred:* a stream with more cancels leaves less resting state, which would explain why 50% cancels is not
  costlier than 30%. The streams contain different mixes of operations, so read these rows as workload
  comparisons, not as the cost of one cancel.
- **Memory** (*measured*, `baseline_alloc.csv`). About 171 bytes retained per resting order at the
  book/engine layer and about 293 at the manager layer (which adds an order record). Resting submit allocates
  2 blocks / 112 bytes per order at book/engine level and 3 / 248 at manager level. Cancels and lookups
  allocate nothing. A match allocates 1-4 blocks per order. *Inferred:* which structures those blocks belong to
  (list node, map node, hash node, trade-history growth); the allocation profiler counts allocations, it
  does not attribute them.

## 7. Bottlenecks: measured versus inferred

No per-structure profiler was run, so **nothing in this section is a direct measurement of `std::list`,
`std::map` or hash-table cost.** What exists is (a) layer-by-layer timings, which isolate the cost of each
class (`book` < `engine` < `manager`), (b) scaling curves, and (c) one scratch cycle-counter run that is not
committed and cannot be reproduced from the repository.

**Directly measured (CSV rows):**

1. At 100k orders / 1000 levels, resting submit costs, in the one run where the whole run stayed in a single
   machine state (run 3), 182 ns at the book layer, 245 ns at the engine layer and 391 ns at the manager
   layer: the engine adds about 63 ns and the manager a further 146 ns. In run 2 the engine and manager
   figures are 548 and 843 ns (a further 295 ns for the manager, about 2.0x run 3, consistent with the global
   2.2x shift) but its book figure was measured in a different state and gives a misleading "365 ns" engine
   overhead; an earlier revision of this file quoted that. Other sizes are in the `scale-*` groups.
2. Replace-style modify (price change, quantity increase) costs 2.6-2.9x an in-place decrease in run 2
   (1189 / 1309 vs 454 ns) and 3.2x in run 3 (647 / 655 vs 203 ns).
3. Random-order cancel costs about 6.7x (run 2: 747 vs 112 ns) to 8.0x (run 3: 411 vs 51 ns) a cancel in queue
   order at the manager layer, 100k orders; the absolute gap widens with book size (run 2: 257 ns at 1k orders,
   1.54 us at 250k).
4. Cancels, lookups and in-place decreases make no allocations; a resting submit makes 2 (book/engine) or 3
   (manager) and a match 1-4.
5. `depth(N)` allocates two blocks and costs about 1.9 us at N=100.

**Inferred (hypotheses, not tested):**

- The manager-layer overhead is record creation in an `unordered_map`, the in-flight marker and the
  trade-cursor sync. This is read from the code, not from a profile of those steps.
- Replace-style modify loses its time to being a cancel plus a resubmit. This follows from the code (the
  modify is implemented that way) but the split of the 1.2-1.3 us was not measured.
- Large-book random cancel is dominated by cache misses rather than instructions (no hardware counters).
- Per-order allocation (list node, map and hash nodes) is the largest remaining cost of a resting submit.
  The allocation counts are measured; the claim that they dominate the time is **not** measured.

**One-off scratch measurement (not reproducible from the repository).** A TSC cycle-counter instrumentation of
scratch copies of `OrderManager`, `MatchingEngine` and `OrderBook` (never committed) on the manager
resting-submit scenario (100k orders, 1000 levels) printed, per call, in TSC cycles: manager submit 2306,
record emplace 448, engine submit 1645, overflow pre-check 360, `ensureTradeIds` 45, `execute` 938,
`book.add` 769. These are from a single run; they were not converted to nanoseconds; the instrumentation
itself adds overhead; and the numbers do not add up exactly (nested scopes overlap). They suggest where the
optimization candidate in section 8 was aimed, nothing more.

## 8. Optimization attempt (not retained)

**Hypothesis.** `MatchingEngine::submit` runs `restingWouldOverflow`, which looks the price level up in the
`std::map`, and then `book.add` looks it up again. For an order that cannot trade, the pre-check is
redundant with the check inside `OrderBook::add`.

**Change tried.** Skip the pre-check when the incoming order does not cross the best opposite price, and
turn an overflow refusal from `book.add` into the same `QuantityOverflow` rejection. All 412 tests passed.

**Measurement.** Interleaved A/B runs (baseline binary vs candidate, alternating order, same pinned core,
`--no-latency --reps 15`, 4-8 rounds). The raw CSVs, the comparison scripts, the binaries' SHA-256 hashes and a
list of what was *not* preserved are in [`docs/benchmarks/ab/`](benchmarks/ab/README.md); every ratio below
can be regenerated with `bash docs/benchmarks/ab/abcmp.sh <set directory>`. Ratios are candidate / baseline
of the mean of per-process medians; "rounds won" counts rounds in which the candidate's median was lower.

| Scenario (100k orders, 1000 levels) | candidate / baseline | rounds won | set |
|---|---|---|---|
| resting submit, engine layer | 0.84, 0.84, 0.83 in three independent runs | 4/4, 4/4, 6/6 | `submit_resting`, `core_run1_CONFOUNDED`, `core_run2` |
| resting submit, manager layer | 0.86, 0.86, 0.87 | 4/4, 4/4, 6/6 | same |
| resting submit, book layer (code unchanged, so a noise control) | 1.07, 1.01, 0.99 (min-based 1.00 in all three) | 2/4, 2/4, 5/6 | same |
| match one-to-one, engine / manager layer | 1.05 / 1.01 | 1/8, 4/8 | `isolated_match_one_to_one` |
| match many-to-one, engine / manager layer | 1.07 / 1.00 | 4/8, 4/8 | `isolated_match_many_to_one` |
| mixed Phase 7 stream (narrow / wide / mixed) | 1.02 / 0.93 / 0.96 | 1/8, 7/8, 2/8 | `isolated_mixed_phase7` |

How large is the noise these ratios must beat? In the `submit_resting` set the **unchanged** book layer
ranges 0.84-1.07 (min-based 0.87-1.01) across sizes, and an identical binary compared with itself
(`control_baseline_vs_identical_copy`) ranges 0.98-1.04 on a quiet scenario. Across the ten manager-layer
sizes of the `submit_resting` set the candidate ranges 0.73-1.02, so the 13-14% gain at 100k orders is not
uniform: at 1 price level it is gone (1.02). *Inferred, not tested:* with a single level the repeated lookup
is cheap, so there is little to save.

**Decision: reverted.** The gain on resting submits at 100k orders is repeatable across three independent
runs (about 13-14% at the manager layer, 16-17% at the engine layer), but it does not show up end-to-end in the
mixed workloads (+2%, -7%, -4% with inconsistent rounds, not distinguishable from noise), the engine-layer
matching scenarios measured 5-7% slower (8-round sets, 1 and 4 of 8 rounds won), and the change moves a
rejection path into `execute()`. That does not meet the bar of a repeatable, meaningful improvement without
added complexity. **No production code was changed in Phase 8; the baseline is the final measurement.**
This is a judgement call, not a result that settles the question: the gain is real in the scenario it targets.

**The two long `--group core` A/B runs are confounded and are not used as evidence.** In both, scenarios the
change cannot touch were slower with the candidate: in `core_run1_CONFOUNDED` `market_data` 1.07-1.27,
`order_lookup` 1.21-1.27, `cancel_in_queue` 1.21-1.25, `match_` 1.19-1.32; in `core_run2` `market_data`
1.10-1.32, `order_lookup` 1.12-1.20, `modify` 1.11-1.16, `match_` 1.05-1.22 (but `cancel_in_queue`
0.91-1.02). Short isolated reruns were then made. The isolated sets that survive show **no uniform offset but
substantial noise** (`isolated_cancel_in_queue_book` 0.96-1.14 by median, 0.98-1.03 by min;
`isolated_match` 0.87-1.22 by median, 0.90-1.10 by min). The isolated `market_data` / `modify` /
`cancel_modify_frequency` runs, which I had summarised as "about 1.0", were overwritten and survive only as a
console readout (market_data 0.92-1.21 with no consistent direction; modify 0.96-1.06; cancel_modify_frequency
0.87-0.97), so that summary overstated how clean they were. The cause of the offset in the long runs is
unknown (section 9).

## 9. Variability: the machine-state discrepancy and high-CV scenarios

### 9.1 The same binary runs at two speeds (measured; cause not determined)

An earlier revision of this file said the difference between short filtered runs and full-suite runs of the
same binary was a "run-mode" effect. **That was wrong.** Controlled tests (all on the unmodified baseline
binary, pinned to CPU 2, raw files in `docs/benchmarks/` and `docs/benchmarks/runmode/`) show:

| Invocation | `cancel_in_queue` book, queue 50k, oldest-first (ns/op, median) | `bestBid` (ns) | mixed narrow (ns/op) |
|---|---:|---:|---:|
| **full 158 scenarios, default flags**, run 1 (`baseline_run1.csv`) | 95.0 | 2.75 | 667 |
| **full 158 scenarios, default flags**, run 2 (`baseline.csv`) | 91.9 | 2.75 | 676 |
| **full 158 scenarios, default flags**, run 3 (`baseline_run3.csv`) | **40.4** | **1.20** | **289** |
| `--group core`, default flags (2 runs) | 48.5, 41.1 | 1.25, 1.43 | 410, 290 |
| `--group core --no-latency` (2 runs) | 41.5, 40.4 | 1.44, 1.36 | 295, 294 |
| unfiltered, `--no-latency --reps 11`, first 55 s only (3 runs, console output) | 41.9, 39.6, 40.7 | - | - |
| `--group core`, same flags (3 runs, console output) | 40.8, 40.4, 40.4 | - | - |
| `--filter "cancel_in_queue layer=book"`, default flags (3 runs) | 54.4, 55.4, 53.8 | - | - |
| same filter, `--no-latency --reps 15` (3 runs) | 53.3, 56.0, 55.9 | - | - |
| same filter, `--no-latency --reps 21` (3 runs) | 54.6, 55.8, 54.1 | - | - |

What this shows:

- **Not a run-mode effect.** A complete default run (run 3) is as fast as the filtered and group-only runs; the
  two slow full runs were slow for a reason that is not the flags, the filter, the latency pass, the
  repetition count or running the whole suite.
- **A uniform factor, not a software effect.** Across the 164 scenarios present in all three full runs,
  run 2 / run 3 has a median of 2.27 (10th-90th percentile 1.90-2.48, min 0.96, max 3.04), while run 1 / run 2
  has a median of 1.01 (0.96-1.12). Even a read-only, cache-resident query (`bestBid`) is 2.3x slower
  (2.75 vs 1.20 ns), as is `depth(100)` (1927 vs 864 ns). *Inferred:* a factor that touches code with no
  memory or allocator dependence is a property of the machine (clock speed or CPU sharing), not of the
  engine's data structures.
- **The state changes within a run.** In run 2 the first scenario (book resting submit, 183 ns) and the last six
  `cancel_modify_frequency` rows (318-401 ns) match run 3, while everything between them is about 2.2x slower;
  these are 7 of the 8 scenarios outside the 2.2x pattern. In run 1 the first scenario has cv 30%. So one
  process can contain both states, and rows within `baseline.csv` are not all comparable with each other.
- **The slow state looked steadier, the fast one noisier.** Run 3 has 19 rows with cv above 30% and 89 above
  10%; run 2 has 14 and 26.
- **A smaller, separate effect.** The nine filtered `cancel_in_queue` runs sit at 53-56 ns, about 1.3x the
  40-48 ns of the group-level and unfiltered runs for the same scenario. This is consistent across all nine
  and I did not investigate it.

*Not determined, not tested:* what causes the two states. Plausible candidates are laptop CPU frequency /
power-limit / thermal behaviour, activity on the SMT sibling hyperthread of CPU 2, and the Windows power plan
or scheduler. No frequency or performance counter was recorded, CPU 2's sibling was not isolated and other
cores were not tried, so none of these is confirmed.

Consequences for how to read this document:

1. Absolute numbers are state-dependent by about 2.2x on this machine. Run-to-run agreement between two runs
   (as between runs 1 and 2) proves nothing about the other state.
2. `baseline.csv` is mostly in the slow state; `baseline_run3.csv` is in the fast state. Neither is canonical.
3. Comparisons are only safe between rows measured in the same state. Interleaved A/B runs help but can still
   be confounded if the state changes between processes, which is a plausible reading of the two confounded
   `--group core` A/B runs in section 8 (it was not verified).
4. The scaling *shapes* in section 6 were re-checked in run 3 and hold, with growth factors that differ by up to
   about 25% between the states (run 2 / run 3): manager resting submit from 1k to 250k orders 2.09x / 2.03x;
   manager random cancel 6.0x / 7.4x; `book.find` 5.3x / 5.8x; resting submit from 1 to 50k price levels
   3.9x / 3.8x; random cancel over the same range 2.4x / 2.0x (run 3 is not monotonic: 381 ns at 1 level, 329 ns
   at 10); manager cancel from queue 100 to 50k only +12% / +13%; matching-intensity plateau in both.
   Conclusions that rest on absolute values or on small ratios should be read as ranges across states.

### 9.2 High-CV scenarios

`cv` is the standard deviation divided by the mean of the per-repetition ns/op, in percent. Of 164 scenario
rows, `baseline.csv` has **39 with cv above 5%, 26 above 10% and 14 above 30%**; `baseline_run1.csv` has 9 above
30%; `baseline_run3.csv` has 19 above 30% and 89 above 10%. The 14 rows of `baseline.csv` above 30%:

| cv | Scenario | Variant | Params | median / min / max (ns/op) |
|---:|---|---|---|---|
| 142% | cancel_in_queue | book, oldest-first | queue=1000 | 97.5 / 78.7 / 1000 |
| 135% | cancel_in_queue | manager, oldest-first | queue=1000 | 113.5 / 50.0 / 1073 |
| 133% | cancel_at_level | manager, worst | 10000 levels, 20 per level | 146.8 / 112.3 / 1387 |
| 105% | cancel_in_queue | manager, newest-first | queue=1000 | 118.2 / 50.8 / 889 |
| 85% | cancel_in_queue | manager, newest-first | queue=100 | 107.0 / 46.0 / 517 |
| 82% | cancel_in_queue | book, newest-first | queue=1000 | 97.7 / 42.6 / 518 |
| 47% | cancel_modify_frequency | cancel 0%, modify 50% | 100k ops | 400.9 / 366.6 / 1387 |
| 47% | cancel_modify_frequency | cancel 20%, modify 20% | 100k ops | 398.9 / 366.4 / 1377 |
| 43% | matching_intensity | crossing 0% | 100k ops | 587.3 / 557.1 / 1586 |
| 40% | submit_resting | book | 100k orders, 1000 levels | 183.3 / 174.8 / 434 |
| 39% | cancel_random | manager | 100k orders, 1000 levels | 747.0 / 669.9 / 2051 |
| 35% | cancel_random | engine | 100k orders, 1000 levels | 650.5 / 538.3 / 1404 |
| 34% | order_lookup | book.find | 100k orders, 10000 levels | 29.1 / 24.7 / 69.2 |
| 33% | cancel_in_queue | book, oldest-first | queue=10000 | 94.8 / 45.1 / 194 |

*Measured:* the small-queue `cancel_in_queue` rows (queue 100-10000) have a **minimum of roughly half the
median** (43-51 ns versus 95-118 ns), which means individual repetitions of one scenario ran at both speeds, the 2.2x state of 9.1,
rather than scattering around one value. The `cancel_modify_frequency` and first-scenario rows are the ones
section 9.1 shows straddling a state change. Others (`cancel_random`, `order_lookup` on a large book) have a
min close to the median and a large max, which looks like occasional slow repetitions.
*Inferred, not tested:* these are the same machine-state change and, for the large-book rows, ordinary cache and
allocator variation. How to use them: the medians and minimums are in the CSV, but do not quote any row above
as a precise number, do not compare two rows that differ by less than about 15% (the spread of identical
binaries in `docs/benchmarks/ab/control_baseline_vs_identical_copy`), and do not read a trend from a
sequence that includes a high-CV row.

## 10. Limitations and caveats

- One machine, one compiler, one OS. No Clang/MSVC/Linux numbers and no comparison across builds.
- Hybrid CPU with uncontrolled laptop power and thermal state; results were pinned to a P-core but not
  frequency-locked, and the machine showed two speeds about 2.2x apart (section 9.1) whose cause was not
  determined. Absolute numbers should be read as one state's values, not as properties of the engine.
- Several scenarios are noisy (section 9.2); conclusions are drawn only from repeatable differences, and the
  repeatability evidence for the optimization candidate is limited to the sets in `docs/benchmarks/ab/`.
- Batch-mean latency percentiles understate single-operation tail latency; per-operation percentiles are
  reported only for operations of 2 us or more.
- Single-threaded, in-memory, no I/O; the CLI and parsing are not benchmarked.
- Allocation byte counts are requested bytes, excluding allocator overhead.
- A Debug build of `lob_bench` was built and run once to confirm it refuses to produce numbers (it prints
  the Release-only error and exits with code 2). No Debug timings exist, by design.
