#!/bin/bash
# Interleaved A/B benchmark comparison.  usage: ab.sh <A.exe> <B.exe> <outdir> <rounds> <bench args...>
# Runs A,B,B,A,A,B,B,A,... on the same pinned core and writes one CSV per run.
A="$1"; B="$2"; OUT="$3"; ROUNDS="$4"; shift 4
export PATH="$LOCALAPPDATA/Microsoft/WinGet/Packages/BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe/mingw64/bin:$PATH"
rm -rf "$OUT"; mkdir -p "$OUT"
for ((r=1; r<=ROUNDS; r++)); do
  if (( r % 2 == 1 )); then order="A B"; else order="B A"; fi
  for w in $order; do
    exe="$A"; [ "$w" = B ] && exe="$B"
    "$exe" --pin 2 --no-latency --reps 15 "$@" --csv "$OUT/${w}_$r.csv" > "$OUT/${w}_$r.out" 2>&1
  done
  echo "round $r done"
done
echo "AB RUNS DONE"
