#!/bin/sh
# Diagnostic run (never scored): vectorization and alignment options, and
# bucket counts at 60M events. Built with GCC 15.
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
  awk -v l="$label" '/^steps/{printf "%s tab %d mov %d | key %d ord %d clk %d walk %d fin %d\n", l, $5*1000, $7*1000, $11*1000, $13*1000, $15*1000, $17*1000, $19*1000}' "$S/err"
}
sum() {   # one thread, so the files do not depend on timing: a digest of everything written
  label="$1"; shift
  one "$@" SS_THREADS=1 > /dev/null
  echo "$label $(cd "$S/out" && find . -type f | sort | xargs cat | sha256sum | cut -c1-12) $(find "$S/out" -type f | wc -l)"
}
/expand "$D" "$S/b" "${PROBE_A:-20}"
B="$S/b"
for round in 1 2; do
r def "$B" /p
r novec "$B" /pnv
r noslp "$B" /pns
r noloop "$B" /pnl
r align "$B" /pal
r nounr "$B" /pnu
done
r as150 "$B" /pnv SS_ASPACE=150
r as125 "$B" /pnv SS_ASPACE=125
r as300 "$B" /pnv SS_ASPACE=300
st steps "$B" /pnv
st as125 "$B" /pnv SS_ASPACE=125
r a10 "$B" /pnv SS_ABITS=10
r b10 "$B" /pnv SS_BBITS=10
r a10b10 "$B" /pnv SS_ABITS=10 SS_BBITS=10
echo "t=$(( $(date +%s) - START ))s"
rm -rf "$S/b"; /expand "$D" "$S/c" "${PROBE_B:-60}"; B="$S/c"
for round in 1 2; do
r 60def "$B" /p
r 60novec "$B" /pnv
done
r 60a11 "$B" /pnv SS_ABITS=11
r 60b11 "$B" /pnv SS_BBITS=11
r 60b10 "$B" /pnv SS_BBITS=10
r 60a11b11 "$B" /pnv SS_ABITS=11 SS_BBITS=11
r 60a11b10 "$B" /pnv SS_ABITS=11 SS_BBITS=10
r 60as150 "$B" /pnv SS_ASPACE=150
r 60as125 "$B" /pnv SS_ASPACE=125
st 60steps "$B" /pnv
st 60b10 "$B" /pnv SS_BBITS=10
st 60as125 "$B" /pnv SS_ASPACE=125
echo "t=$(( $(date +%s) - START ))s"
exit 1
