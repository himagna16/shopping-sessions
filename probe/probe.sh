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
  echo "$label $(( (t1 - t0) / 1000000 ))$([ $rc = 0 ] || echo " rc$rc")$(grep -m1 'ms: ' "$S/err" | sed 's/.*ms: //' | awk '{o="";for(i=1;i<=NF;i++){split($i,a,"=");if(a[1]=="peak_rss_kb")o=o" r"int(a[2]/1024);else if(a[1]=="total")o=o" t"int(a[2]+.5);else o=o (i>1?"/":" ") int(a[2]+.5)}print o}')$(awk '/^threads/{printf " p%d t%d", $9*1000, $18*1000}' "$S/err")"
}
E="${PROBE_E:-20000000}"
/generate --seed 7 --events "$E" --clicks $(( E / 4 )) --output "$S/b" > /dev/null 2>&1; G=$(( $(date +%s) - START )); echo "gen rc$? ${G}s"
B="$S/b"
run xl o1 "$B" /xl
run xl o1 "$B" /xl
run xl o1 "$B" /xl
run ours o2 "$B" /p
echo "cmp20 $(python3 /cmp.py "$S/o1" "$S/o2" 2>&1 | tail -1 | cut -c1-120)"
run xl14 o3 "$B" /xl14
run xl14 o3 "$B" /xl14
echo "t=$(( $(date +%s) - START ))s"
rm -rf "$S/b" "$S/o1" "$S/o2" "$S/o3"
if [ "$G" -le 22 ]; then
E3=$(( E * 3 ))
/generate --seed 9 --events "$E3" --clicks $(( E3 / 4 )) --output "$S/c" > /dev/null 2>&1; echo "gen60 rc$? t=$(( $(date +%s) - START ))s"
B="$S/c"
run 60xl o1 "$B" /xl
run 60xl o1 "$B" /xl
grep -m1 'rows: ' "$S/err" | sed 's/.*rows: //' | cut -c1-230
grep -m1 'prof ' "$S/err" | sed 's/.*: //' | cut -c1-170
run 60xl14 o3 "$B" /xl14
run 60c4 o3 "$B" /xl XL_CH=4096
run 60c16 o3 "$B" /xl XL_CH=16384
run 60xl o1 "$B" /xl
run 60xl14 o3 "$B" /xl14
run 60c4 o3 "$B" /xl XL_CH=4096
run 60c16 o3 "$B" /xl XL_CH=16384
run 60xl o1 "$B" /xl
run 60xl14 o3 "$B" /xl14
run 60nh o3 "$B" /xl XL_NOHUGE=1
run 60sp0 o3 "$B" /xl XL_SPIN=0
run 60sp2k o3 "$B" /xl XL_SPIN=2000
run 60xl o1 "$B" /xl
run 60ours o2 "$B" /p
echo "cmp60 $(python3 /cmp.py "$S/o1" "$S/o2" 2>&1 | tail -1 | cut -c1-120)"
fi
echo "t=$(( $(date +%s) - START ))s"
exit 1
