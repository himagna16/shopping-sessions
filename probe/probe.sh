#!/bin/sh
# Diagnostic run (never scored): compares build variants on the scoring
# machine. Output stays under ~1,900 characters.
exec 1>&2
D="${DATA_DIR:-/data}"; S="${SCRATCH_DIR:-/scratch}"
START=$(date +%s)
elapsed() { echo $(( $(date +%s) - START )); }
one() {   # one run -> "wall total parse dedup people parse_cpu links linkparse linkunion"
  data="$1"; bin="$2"; shift 2
  rm -rf "$S/out"; mkdir -p "$S/out"
  t0=$(date +%s%N)
  env SS_MMAP=2 "$@" DATA_DIR="$data" OUTPUT_DIR="$S/out" "$bin" 2> "$S/err"; rc=$?
  t1=$(date +%s%N)
  awk -v w=$(( (t1 - t0) / 1000000 )) -v rc=$rc '/^threads/{printf "%d %d %d %d %d %d %d %d %d", w, $18*1000, $9*1000, $11*1000, $13*1000, $23*1000, $NF*1000, $(NF-5)*1000, $(NF-3)*1000} END{if (rc) printf " rc%d", rc}' "$S/err"
}
r() {     # best of 3 by wall
  label="$1"; shift
  best=""; bw=999999
  for i in 1 2 3; do
    line=$(one "$@"); w=${line%% *}
    if [ "$w" -lt "$bw" ]; then bw=$w; best="$line"; fi
  done
  echo "$label $best"
}
w3() {    # three runs, wall/total each: shows what happens outside the timed phases
  label="$1"; shift
  out="$label"
  for i in 1 2 3; do line=$(one "$@"); out="$out $(echo "$line" | awk '{print $1"/"$2}')"; done
  echo "$out"
}
/expand "$D" "$S/b" 20
B="$S/b"
r new "$B" /p
r people1 "$B" /pp1
r nohugetab "$B" /p SS_NOHUGE=1
r release "$B" /p SS_RELEASE=1
r zap "$B" /p SS_ZAP=1
r new "$B" /p
w3 20new "$B" /p
w3 20rel+zap "$B" /p SS_RELEASE=1 SS_ZAP=1
rm -rf "$S/b"; /expand "$D" "$S/c" 60; B="$S/c"
r 60new "$B" /p
r 60people1 "$B" /pp1
w3 60new "$B" /p
w3 60rel "$B" /p SS_RELEASE=1
w3 60zap "$B" /p SS_ZAP=1
w3 60rel+zap "$B" /p SS_RELEASE=1 SS_ZAP=1
w3 60pread "$B" /p SS_MMAP=0
echo "t=$(elapsed)s"
exit 1
