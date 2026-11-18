# Run-mode and machine-state evidence

Raw files behind [BENCHMARKS.md](../../BENCHMARKS.md) section 9.1. Every run here used the **unmodified
baseline binary** (the one called A in `../ab/README.md`), pinned to CPU 2, one benchmark process at a time
(no other benchmark was running; other activity on the machine was not controlled or recorded). The modes
below were alternated within each set of repetitions, not run as separate blocks.

| Files | Command line (after the executable) |
|---|---|
| `default_<1-3>.csv` | `--pin 2 --filter "cancel_in_queue layer=book"` (21 reps, 3 latency reps) |
| `nolat_reps15_<1-3>.csv` | same filter, `--no-latency --reps 15` (the A/B style) |
| `nolat_reps21_<1-3>.csv` | same filter, `--no-latency --reps 21` |
| `core_default_<1-2>.csv` | `--pin 2 --group core` |
| `core_nolat_<1-2>.csv` | `--pin 2 --group core --no-latency` |
| `unfilt_<1-3>.out.txt` | `--pin 2 --no-latency --reps 11`, no group or filter, **stopped by a 55 s timeout** (console output only; the first `cancel_in_queue` row is the one used) |
| `coreonly_<1-3>.out.txt` | same flags with `--group core`, also stopped after 55 s |

The three full 158-scenario default runs are in the parent directory: `../baseline_run1.csv`,
`../baseline.csv` (run 2) and `../baseline_run3.csv`. Runs 1 and 2 were taken earlier in the day; run 3 was
taken after the other runs listed here.

## What is and is not here

- The `.out.txt` files are console captures cut off by `timeout`, so they contain only the scenarios reached
  in 55 s and no CSV. They are not complete runs.
- Nothing about the CPU's frequency, temperature or the sibling hyperthread was recorded, so these files can
  show *that* the machine ran at two speeds but not *why*.
- The `# command_line` header of each CSV here, and of `../baseline_run3.csv`, had its output path replaced by
  `<path omitted>`; no measurement row was changed. `../baseline_run1.csv` needed no redaction; its header
  records the file name it had when it was written (`baseline.csv`), before it was renamed.
