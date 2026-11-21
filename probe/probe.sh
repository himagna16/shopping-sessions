#!/bin/sh
# Diagnostic run (never scored): inlining variants, the click parser by stage,
# resolver distance, and whether a GCC 15 build writes the same bytes.
# Output stays under ~1,900 characters.
exec 1>&2
D="${DATA_DIR:-/data}"; S="${SCRATCH_DIR:-/scratch}"
START=$(date +%s)
one() {   # one run -> "wall total parse dedup people parse_cpu links user sys"
  data="$1"; bin="$2"; shift 2
  rm -rf "$S/out"; mkdir -p "$S/out"
  t0=$(date +%s%N)
  env SS_MMAP=2 "$@" DATA_DIR="$data" OUTPUT_DIR="$S/out" "$bin" 2> "$S/err"; rc=$?
  t1=$(date +%s%N)
  awk -v w=$(( (t1 - t0) / 1000000 )) -v rc=$rc '/^user/{u=$2*1000; s=$4*1000} /^threads/{printf "%d %d %d %d %d %d %d %d %d", w, $18*1000, $9*1000, $11*1000, $13*1000, $23*1000, $NF*1000, u, s} END{if (rc) printf " rc%d", rc}' "$S/err"
}
r() {     # best of 3 by wall -> "wall total parse dedup people parse_cpu"
  label="$1"; shift
  best=""; bw=999999
  for i in 1 2 3; do
    line=$(one "$@"); w=${line%% *}
    if [ -n "$w" ] && [ "$w" -lt "$bw" ]; then bw=$w; best="$line"; fi
  done
  echo "$label $(echo "$best" | cut -d' ' -f1-6)"
}
p() {     # parse only: best of 3 by parse cpu -> "parse parse_cpu"
  label="$1"; shift
  best=""; bc=999999
  for i in 1 2 3; do
    line=$(one "$@"); c=$(echo "$line" | awk '{print $6}')
    if [ -n "$c" ] && [ "$c" -lt "$bc" ]; then bc=$c; best=$(echo "$line" | awk '{print $3, $6}'); fi
  done
  echo "$label $best"
}
st() {    # steps inside dedup and people, ms of thread time, from the last of 2 runs
  label="$1"; shift
  one "$@" > /dev/null; one "$@" > /dev/null
  awk -v l="$label" '/^steps/{printf "%s tab %d mov %d | key %d ord %d clk %d walk %d\n", l, $5*1000, $7*1000, $11*1000, $13*1000, $15*1000, $17*1000}' "$S/err"
}
sum() {   # one thread, so the files do not depend on timing: a digest of everything written
  label="$1"; shift
  one "$@" SS_THREADS=1 > /dev/null
  echo "$label $(cd "$S/out" && find . -type f | sort | xargs cat | sha256sum | cut -c1-12) $(find "$S/out" -type f | wc -l)"
}
/expand "$D" "$S/b" "${PROBE_A:-20}"
B="$S/b"
r def "$B" /p
r nohot "$B" /pnh
r nowalk "$B" /pnw
r g15 "$B" /p15
r id2 "$B" /pid2
r def "$B" /p
r nohot "$B" /pnh
r nowalk "$B" /pnw
r g15 "$B" /p15
r id2 "$B" /pid2
st steps "$B" /p
st nowalk "$B" /pnw
st assist1 "$B" /pa1
st steps "$B" /p
st assist1 "$B" /pa1
st scan0 "$B" /ps0
st scan6 "$B" /ps6
st d8 "$B" /pd8
st d32 "$B" /pd32
st d64 "$B" /pd64
p cscan "$B" /pc2
p cts "$B" /pc3
p ccamp "$B" /pc4
p cids "$B" /pc5
p cnoalias "$B" /pc6
p full "$B" /p
sum sha14 "$B" /p
sum sha15 "$B" /p15
sum sha14 "$B" /p
rm -rf "$S/b"; /expand "$D" "$S/c" "${PROBE_B:-60}"; B="$S/c"
r 60def "$B" /p
r 60nohot "$B" /pnh
r 60g15 "$B" /p15
r 60id2 "$B" /pid2
sum 60sha14 "$B" /p
sum 60sha15 "$B" /p15
echo "t=$(( $(date +%s) - START ))s"
exit 1
