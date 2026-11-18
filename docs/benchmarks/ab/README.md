# Preserved A/B data for the rejected Phase 8 optimization candidate

This directory holds the raw output of the interleaved A/B experiment described in
[BENCHMARKS.md](../../BENCHMARKS.md) section 8. The candidate was **reverted**; nothing here changes
production behaviour. The data is kept so the numbers quoted in BENCHMARKS.md can be checked.

## Baseline (A) and candidate (B)

| | |
|---|---|
| A, baseline | `lob_bench` built from the unmodified Phase 7 production code (`f125d84`; `src/` and `include/` are byte-identical to the Phase 8 commit `f697bc3`). SHA-256 `851f8767...b519f08` |
| B, candidate | the same harness plus one change to `src/MatchingEngine.cpp`. SHA-256 `e672412e...a55e2a24` |
| Control | a byte-identical copy of A (same SHA-256 as A), used to measure how far two runs of the same binary differ |

**The candidate change** (described, not reproduced as a patch; see "What is not preserved"):
`MatchingEngine::submit` skipped the `restingWouldOverflow` pre-check (a second `std::map` price-level
lookup) when the incoming order could not trade against the best opposite price, and
`MatchingEngine::execute` turned a `QuantityOverflow` refusal from `OrderBook::add` into the same
`QuantityOverflow` rejection when nothing had executed. All 412 tests passed with the change applied.

## Protocol (`ab.sh`, `abcmp.sh`)

- `ab.sh A.exe B.exe OUTDIR ROUNDS <lob_bench args>` runs A,B in round 1, B,A in round 2, A,B in round 3
  and so on, on the same pinned core (`--pin 2`), with `--no-latency --reps 15` and the default 2 warm-ups.
  One CSV per process: `A_<round>.csv`, `B_<round>.csv`.
- `abcmp.sh OUTDIR` prints, per scenario: the mean over rounds of each process's **median** ns/op (A_med,
  B_med) and of each process's **min** ns/op, the ratio B/A, and "wins" = in how many rounds B's median was
  lower than the A median of the same round. `comparison.txt` in each set is the output of `abcmp.sh` on that
  set (derived data; regenerate it with `bash abcmp.sh <set directory>`).
- `ab.sh` expects the MinGW runtime DLLs on `PATH`; it exports the path of the WinLibs install used on the
  development machine and must be edited for another machine. Both scripts are unchanged copies.

## Sets

| Directory | Selection | Rounds | What it shows | Caveat |
|---|---|---:|---|---|
| `submit_resting/` | `--filter submit_resting` | 4 | B/A by median across the 21 scenarios: engine 0.84 (one scenario), manager 0.73-1.02 (10 sizes; 0.86 at 100k orders / 1000 levels), **book layer, whose code did not change, 0.84-1.07** (min-based 0.87-1.01) | the unchanged book layer moves by up to ~15% in either direction, so differences of that size are not evidence |
| `core_run1_CONFOUNDED/` | `--group core` | 4 | submit gains reproduce (engine 0.84, manager 0.86), **but** nearly every other scenario, including ones the change cannot touch, is slower in B: `market_data` 1.07-1.27, `order_lookup` 1.21-1.27, `cancel_in_queue` 1.21-1.25, `match_` 1.19-1.32 | not usable as evidence either way |
| `core_run2/` | `--group core` | 6 | submit gains reproduce (engine 0.83, manager 0.87); a similar offset on later scenarios: `market_data` 1.10-1.32, `order_lookup` 1.12-1.20, `modify` 1.11-1.16, `match_` 1.05-1.22, but `cancel_in_queue` 0.91-1.02 | same |
| `control_baseline_vs_identical_copy/` | A vs a copy of A, `cancel_in_queue layer=book` | 4 | 9 scenarios, B/A by median 0.98-1.04 (min-based 0.98-1.03) | the spread two identical binaries show on a quiet scenario |
| `isolated_cancel_in_queue_book/` | A vs B, same filter | 4 | 9 scenarios, B/A by median 0.96-1.14 (min-based 0.98-1.03), no consistent direction | one scenario (`queue=1000 newest-first`) is a 1.14 outlier by median |
| `isolated_match/` | `--filter match_` | 4 | 14 scenarios (engine and manager), B/A by median 0.87-1.22 (min-based 0.90-1.10) | 4 rounds; the larger 8-round sets below are more reliable |
| `isolated_match_one_to_one/` | `--filter match_one_to_one` | 8 | engine 1.05, manager 1.01 | |
| `isolated_match_many_to_one/` | `--filter match_many_to_one` | 8 | engine 1.07, manager 1.00 | |
| `isolated_mixed_phase7/` | `--filter mixed_phase7` | 8 | narrow 1.02, wide 0.93, mixed 0.96 (1/8, 7/8, 2/8 rounds won) | no end-to-end gain distinguishable from noise |

`core_run1_CONFOUNDED` and `core_run2` are kept deliberately: they show that long A/B runs can carry a
uniform offset on code that did not change, which is why the isolated sets exist.

## What is not preserved (stated, not reconstructed)

- **Isolated `market_data`, `modify` and `cancel_modify_frequency` A/B runs.** The first isolated runs for
  these three selections wrote into one output directory that was deleted and reused for the next selection,
  so only the last (`isolated_match`) survived. For those three only my console readout survives, which was
  *not* saved as data: `market_data` B/A ratios ranged 0.92-1.21 with no consistent direction (for example
  `bestBid` 0.93, 1.09, 1.21, 1.03 across its listed rows); `modify` 0.96-1.06; `cancel_modify_frequency`
  0.87-0.97 with B faster in most rounds. Treat these as recollection of a console printout, not as data.
- **The candidate patch file and the candidate binary.** The source change was reverted with a copy of the
  baseline file and no diff was kept; the candidate binary was not committed (SHA-256 recorded above). The
  description above is the only record of the change.
- Console `.out` files of the A/B runs were not kept; the CSVs hold the measured rows.

## Limitations

- A/B runs used `--no-latency --reps 15`; their absolute ns/op are **not comparable** to `baseline.csv`
  (see BENCHMARKS.md section 9, which shows that absolute numbers differ between process invocations even
  for the same binary). Only the within-run B/A ratios are meaningful, and only where the control shows that
  two identical binaries agree.
- Ratios are noisy: the identical-binary control spans 0.98-1.04 on a quiet scenario, and the book-layer
  submit scenario (unchanged code) shows 0.99-1.07.
- 4-8 rounds per set is a small sample; no significance test was applied.
- The raw CSVs differ from the originals in one respect: the `# command_line` header line had its absolute
  output path (a local user directory) replaced by `<path omitted>`. No measurement row was altered.
