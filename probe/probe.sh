#!/bin/sh
# Diagnostic run (never scored): Team 67's entry against ours on generator data
# made here, at the practice size and the final's, with an output comparison.
exec 1>&2
S="${SCRATCH_DIR:-/scratch}"
START=$(date +%s)
run() {   # label out-dir data bin env... -> "label wall-ms rc" and, for their binary, its own timing line
  label="$1"; out="$S/$2"; data="$3"; bin="$4"; shift 4
  rm -rf "$out"; mkdir -p "$out"
  t0=$(date +%s%N)
  env "$@" NCPU=8 XL_TIMING=1 SS_MMAP=2 DATA_DIR="$data" OUTPUT_DIR="$out" "$bin" 2> "$S/err"; rc=$?
  t1=$(date +%s%N)
  echo "$label $(( (t1 - t0) / 1000000 )) rc$rc $(grep -m1 'ms: ' "$S/err" | sed 's/.*ms: //; s/peak_rss_kb/rss/' | cut -c1-110)$(awk '/^threads/{printf "parse %d total %d", $9*1000, $18*1000}' "$S/err")"
}
E="${PROBE_E:-20000000}"
/generate --seed 7 --events "$E" --clicks $(( E / 4 )) --output "$S/b" > /dev/null 2>&1; G=$(( $(date +%s) - START )); echo "gen rc$? ${G}s"
B="$S/b"
run xl o1 "$B" /xl
run xl o1 "$B" /xl
run xl o1 "$B" /xl
grep -m1 'rows: ' "$S/err" | cut -c1-200
run ours o2 "$B" /p
run ours o2 "$B" /p
echo "cmp20 $(python3 /cmp.py "$S/o1" "$S/o2" 2>&1 | tail -1 | cut -c1-150)"
run xl14 o3 "$B" /xl14
run xl16 o3 "$B" /xl16
run xl14 o3 "$B" /xl14
run xl16 o3 "$B" /xl16
echo "t=$(( $(date +%s) - START ))s"
rm -rf "$S/b" "$S/o1" "$S/o2" "$S/o3"
if [ "$G" -le 22 ]; then
E3=$(( E * 3 ))
/generate --seed 9 --events "$E3" --clicks $(( E3 / 4 )) --output "$S/c" > /dev/null 2>&1; echo "gen60 rc$? t=$(( $(date +%s) - START ))s"
B="$S/c"
run 60xl o1 "$B" /xl
run 60xl o1 "$B" /xl
run 60xl o1 "$B" /xl
run 60xl14 o3 "$B" /xl14
run 60xl16 o3 "$B" /xl16
run 60xl14 o3 "$B" /xl14
run 60xl16 o3 "$B" /xl16
run 60ours o2 "$B" /p
echo "cmp60 $(python3 /cmp.py "$S/o1" "$S/o2" 2>&1 | tail -1 | cut -c1-150)"
fi
echo "t=$(( $(date +%s) - START ))s"
exit 1
