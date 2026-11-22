#!/bin/sh
# Diagnostic run (never scored): Team 67's two newest builds at 20M and 60M,
# and an output comparison with ours on the organizers' fixtures.
exec 1>&2
S="${SCRATCH_DIR:-/scratch}"
START=$(date +%s)
run() {   # label out-dir data bin env... -> "label wall-ms t<own total>"
  label="$1"; out="$S/$2"; data="$3"; bin="$4"; shift 4
  rm -rf "$out"; mkdir -p "$out"
  t0=$(date +%s%N)
  env NCPU=8 XL_TIMING=1 SS_MMAP=2 DATA_DIR="$data" OUTPUT_DIR="$out" "$@" "$bin" 2> "$S/err"; rc=$?
  t1=$(date +%s%N)
  echo "$label $(( (t1 - t0) / 1000000 ))$([ $rc = 0 ] || echo " rc$rc") t$(grep -m1 'ms: ' "$S/err" | sed 's/.*total=\([0-9]*\).*/\1/')"
}
ok=0; bad=""
for d in /fx/*/; do
  n=$(basename "$d")
  rm -rf "$S/a" "$S/b"; mkdir -p "$S/a" "$S/b"
  NCPU=8 DATA_DIR="$d" OUTPUT_DIR="$S/a" /xn > /dev/null 2>&1; ra=$?
  NCPU=8 DATA_DIR="$d" OUTPUT_DIR="$S/b" /p > /dev/null 2>&1; rb=$?
  r=$(python3 /cmp.py "$S/a" "$S/b" 2>&1 | tail -1 | cut -c1-4)
  if [ "$r" = SAME ] && [ $ra = 0 ] && [ $rb = 0 ]; then ok=$(( ok + 1 )); else bad="$bad $n:$r$ra$rb"; fi
done
echo "fx same $ok, not:$(echo "$bad" | cut -c1-420)"
E="${PROBE_E:-20000000}"
/generate --seed 13 --events "$E" --clicks $(( E / 4 )) --output "$S/c" > /dev/null 2>&1; echo "gen20 rc$?"
sync
B="$S/c"
run 20xn o1 "$B" /xn
run 20xl o1 "$B" /xl
run 20xn o1 "$B" /xn
run 20xl o1 "$B" /xl
run 20xn o1 "$B" /xn
run 20ours o2 "$B" /p
echo "cmp20 $(python3 /cmp.py "$S/o1" "$S/o2" 2>&1 | tail -1 | cut -c1-100)"
rm -rf "$S/c"
E3=$(( E * 3 ))
/generate --seed 21 --events "$E3" --clicks $(( E3 / 4 )) --output "$S/c" > /dev/null 2>&1; echo "gen60 rc$?"
sync; echo "t=$(( $(date +%s) - START ))s"
run w o1 "$B" /xn
run w o1 "$B" /xl
for i in 1 2 3 4 5; do
run xl o1 "$B" /xl
run xn o1 "$B" /xn
run xn13 o1 "$B" /xn13
run n12 o1 "$B" /xn NCPU=12
run p0 o1 "$B" /xn XL_PIN=0
done
run xn o1 "$B" /xn
run ours o2 "$B" /p
echo "cmp60 $(python3 /cmp.py "$S/o1" "$S/o2" 2>&1 | tail -1 | cut -c1-100)"
echo "t=$(( $(date +%s) - START ))s"
exit 1
