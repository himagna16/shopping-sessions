#!/bin/sh
# Diagnostic run (never scored): compares build variants on the scoring
# machine. Output stays under ~1,900 characters.
exec 1>&2
D="${DATA_DIR:-/data}"; S="${SCRATCH_DIR:-/scratch}"
START=$(date +%s)
elapsed() { echo $(( $(date +%s) - START )); }
# r LABEL DATA BIN [VAR=VALUE ...] -> "LABEL wall total parse dedup people parse_cpu links" (all ms), best of 3
r() {
  label="$1"; data="$2"; bin="$3"; shift 3
  best=""; bw=999999
  for i in 1 2 3; do
    rm -rf "$S/out"; mkdir -p "$S/out"
    t0=$(date +%s%N)
    env SS_MMAP=2 "$@" DATA_DIR="$data" OUTPUT_DIR="$S/out" "$bin" 2> "$S/err"; rc=$?
    t1=$(date +%s%N)
    w=$(( (t1 - t0) / 1000000 ))
    line=$(awk -v w=$w -v rc=$rc '/^threads/{printf "%d %d %d %d %d %d %d", w, $18*1000, $9*1000, $11*1000, $13*1000, $23*1000, $NF*1000} END{if (rc) printf " rc%d", rc}' "$S/err")
    if [ "$w" -lt "$bw" ]; then bw=$w; best="$line"; fi
  done
  echo "$label $best"
}
/expand "$D" "$S/b" 20
B="$S/b"
r new "$B" /p
r old "$B" /pold
r mixhash "$B" /pmix
r noinline "$B" /pnoinl
r nopf "$B" /pnopf
r pin "$B" /p SS_PIN=1
r t7 "$B" /p SS_THREADS=7
r t9 "$B" /p SS_THREADS=9
r new "$B" /p
r old "$B" /pold
rm -rf "$S/b"; /expand "$D" "$S/c" 60; B="$S/c"
r 60new "$B" /p
r 60old "$B" /pold
r 60pin "$B" /p SS_PIN=1
echo "t=$(elapsed)s"
exit 1
