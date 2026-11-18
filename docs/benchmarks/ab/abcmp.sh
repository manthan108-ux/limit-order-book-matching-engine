#!/bin/bash
# Compares the A and B CSVs produced by ab.sh.  usage: abcmp.sh <outdir> [min_ratio_to_flag]
OUT="$1"
awk -F, '
FNR==1 { file=FILENAME; n=split(file, p, "/"); base=p[n]; side=substr(base,1,1) }
/^#/ || /^scenario,/ { next }
{
  key=$1 "|" $2 "|" $3
  med[side, key]  += $8; mn[side, key] += $9; cnt[side, key]++
  if (!(key in seen)) { seen[key]=1; order[++k]=key }
  if (cnt[side, key]==1 || $8 < bestmed[side, key]) bestmed[side, key]=$8
  # round-by-round win counting
  rmed[side, key, cnt[side, key]] = $8
}
END {
  printf "%-96s %10s %10s %7s %10s %10s %7s  %s\n", "scenario|variant|params", "A_med", "B_med", "B/A", "A_min", "B_min", "B/A", "wins(B faster median)"
  for (i=1;i<=k;i++) {
    key=order[i]
    if (cnt["A",key]==0 || cnt["B",key]==0) continue
    am=med["A",key]/cnt["A",key]; bm=med["B",key]/cnt["B",key]
    an=mn["A",key]/cnt["A",key];  bn=mn["B",key]/cnt["B",key]
    # per-round pairing: round index j of A vs round index j of B (same position)
    wins=0; tot=0
    for (j=1;j<=cnt["A",key] && j<=cnt["B",key];j++) { tot++; if (rmed["B",key,j] < rmed["A",key,j]) wins++ }
    printf "%-96s %10.1f %10.1f %7.3f %10.1f %10.1f %7.3f  %d/%d\n", substr(key,1,96), am, bm, bm/am, an, bn, bn/an, wins, tot
  }
}' "$OUT"/A_*.csv "$OUT"/B_*.csv
