#!/bin/sh
# Diagnostic run (never scored): times builds that each skip one step, to see
# where the time goes on the scoring machine. Output is kept under ~1,900
# characters because only the end of stderr is shown.
exec 1>&2
D="${DATA_DIR:-/data}"; S="${SCRATCH_DIR:-/scratch}"
START=$(date +%s)
elapsed() { echo $(( $(date +%s) - START )); }
# r LABEL DATA BIN [VAR=VALUE ...] -> "LABEL wall total parse dedup people cpu links" (ms; cpu in s), best of 2
r() {
  label="$1"; data="$2"; bin="$3"; shift 3
  best=""; bw=999999
  for i in 1 2; do
    rm -rf "$S/out"; mkdir -p "$S/out"
    t0=$(date +%s%N)
    env "$@" DATA_DIR="$data" OUTPUT_DIR="$S/out" "$bin" 2> "$S/err"; rc=$?
    t1=$(date +%s%N)
    w=$(( (t1 - t0) / 1000000 ))
    line=$(awk -v w=$w -v rc=$rc '/^user/{c=$2+$4} /^threads/{printf "%d %d %d %d %d", w, $18*1000, $9*1000, $11*1000, $13*1000; l=$NF*1000} END{printf " %.1f %d", c, l; if (rc) printf " rc%d", rc}' "$S/err")
    if [ "$w" -lt "$bw" ]; then bw=$w; best="$line"; fi
  done
  echo "$label $best"
}
/expand "$D" "$S/b" 20
B="$S/b"
r base "$B" /p
r mmap "$B" /p SS_MMAP=1
r read "$B" /q1
r readmm "$B" /q1 SS_MMAP=1
r scan "$B" /q2
r scanmm "$B" /q2 SS_MMAP=1
r conv "$B" /q3
r convmm "$B" /q3 SS_MMAP=1
r noclk "$B" /q5
r d1only "$B" /x3
r dnotab "$B" /x7
r dnoali "$B" /x4
r pcells "$B" /x2
r pno4 "$B" /x1
r pnoclk "$B" /x5
r pnowr "$B" /x6
r zap "$B" /p SS_ZAP=1
r t1 "$B" /p SS_THREADS=1
r t1mm "$B" /p SS_THREADS=1 SS_MMAP=1
r base "$B" /p
rm -rf "$S/b"; /expand "$D" "$S/c" 60; B="$S/c"
r 60base "$B" /p
r 60zap "$B" /p SS_ZAP=1
[ "$(elapsed)" -lt 140 ] && r 60mm "$B" /p SS_MMAP=1
[ "$(elapsed)" -lt 150 ] && r 60mmzap "$B" /p SS_MMAP=1 SS_ZAP=1
echo "t=$(elapsed)s"
exit 1
