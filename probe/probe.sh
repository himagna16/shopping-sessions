#!/bin/sh
# Diagnostic run (never scored): compares scanner and timestamp variants on
# the scoring machine. Output stays under ~1,900 characters.
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
r base "$B" /p
r scan2 "$B" /ps2
r scan3 "$B" /ps3
r ing2 "$B" /pi2
r both "$B" /pc
r both+id2 "$B" /pcd
r both-pread "$B" /pc SS_MMAP=0
for l in 2 3 5 7; do r cL$l "$B" /cl$l; done
r cL8 "$B" /cq5
r base "$B" /p
r both "$B" /pc
rm -rf "$S/b"; /expand "$D" "$S/c" 60; B="$S/c"
r 60base "$B" /p
r 60both "$B" /pc
echo "t=$(elapsed)s"
exit 1
