#!/bin/sh
# Diagnostic run (never scored): times builds that stop the event row handler
# after each step, and builds that skip one later step, on the scoring
# machine. Output stays under ~1,900 characters: only the end of stderr is shown.
exec 1>&2
D="${DATA_DIR:-/data}"; S="${SCRATCH_DIR:-/scratch}"
START=$(date +%s)
elapsed() { echo $(( $(date +%s) - START )); }
# r LABEL DATA BIN [VAR=VALUE ...] -> "LABEL wall total parse dedup people parse_cpu links" (all ms), best of 2
r() {
  label="$1"; data="$2"; bin="$3"; shift 3
  best=""; bw=999999
  for i in 1 2; do
    rm -rf "$S/out"; mkdir -p "$S/out"
    t0=$(date +%s%N)
    env "$@" DATA_DIR="$data" OUTPUT_DIR="$S/out" "$bin" 2> "$S/err"; rc=$?
    t1=$(date +%s%N)
    w=$(( (t1 - t0) / 1000000 ))
    line=$(awk -v w=$w -v rc=$rc '/^threads/{printf "%d %d %d %d %d %d %d", w, $18*1000, $9*1000, $11*1000, $13*1000, $23*1000, $NF*1000} END{if (rc) printf " rc%d", rc}' "$S/err")
    if [ "$w" -lt "$bw" ]; then bw=$w; best="$line"; fi
  done
  echo "$label $best"
}
/expand "$D" "$S/b" 20
B="$S/b"
r base "$B" /p
for l in 1 2 3 4 5 6 7; do r L$l "$B" /l$l; done
r L8 "$B" /q5
r d1only "$B" /x3
r dnotab "$B" /x7
r dnoali "$B" /x4
r pcells "$B" /x2
r pno4 "$B" /x1
r pnoclk "$B" /x5
r pnowr "$B" /x6
r pread "$B" /p SS_MMAP=0
r t1 "$B" /p SS_THREADS=1
r base "$B" /p
rm -rf "$S/b"; /expand "$D" "$S/c" 60; B="$S/c"
r 60base "$B" /p
[ "$(elapsed)" -lt 140 ] && r 60L8 "$B" /q5
grep -o 'mapped.*' "$S/err" | head -1
echo "t=$(elapsed)s"
exit 1
