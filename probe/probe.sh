#!/bin/sh
# Diagnostic run (never scored): where the parse time goes, and the same
# source under other compilers. Output stays under ~1,900 characters.
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
r() {     # best of 3 by wall
  label="$1"; shift
  best=""; bw=999999
  for i in 1 2 3; do
    line=$(one "$@"); w=${line%% *}
    if [ -n "$w" ] && [ "$w" -lt "$bw" ]; then bw=$w; best="$line"; fi
  done
  echo "$label $best"
}
p() {     # parse only: best of 3 by parse cpu -> "parse parse_cpu user sys"
  label="$1"; shift
  best=""; bc=999999
  for i in 1 2 3; do
    line=$(one "$@"); c=$(echo "$line" | awk '{print $6}')
    if [ -n "$c" ] && [ "$c" -lt "$bc" ]; then bc=$c; best=$(echo "$line" | awk '{print $3, $6, $8, $9}'); fi
  done
  echo "$label $best"
}
st() {    # steps inside dedup and people, ms of thread time, from the last of 2 runs
  label="$1"; shift
  one "$@" > /dev/null; one "$@" > /dev/null
  awk -v l="$label" '/^steps/{printf "%s tab %d mov %d | key %d ord %d clk %d walk %d fin %d\n", l, $5*1000, $7*1000, $11*1000, $13*1000, $15*1000, $17*1000, $19*1000}' "$S/err"
}
/expand "$D" "$S/b" "${PROBE_A:-20}"
B="$S/b"
CL=/pcl
if [ "$(wc -c < /pcl)" -lt 1000 ]; then echo "clang missing"; CL=/p; fi
r def "$B" /p
grep -E '^user' "$S/err" | cut -c1-70
r g15 "$B" /p15
r clang "$B" "$CL"
r def "$B" /p
r g15 "$B" /p15
r clang "$B" "$CL"
p scan "$B" /pl2
p fields "$B" /pl3
p ekey "$B" /pl4
p times "$B" /pl5
p rest "$B" /pl7
p noclicks "$B" /pnc
p full "$B" /p
st steps "$B" /p
st noclk "$B" /px5
st nowr "$B" /px6
st nosess "$B" /px9
rm -rf "$S/b"; /expand "$D" "$S/c" "${PROBE_B:-60}"; B="$S/c"
r 60def "$B" /p
grep -E '^user' "$S/err" | cut -c1-70
r 60g15 "$B" /p15
r 60clang "$B" "$CL"
st 60steps "$B" /p
echo "t=$(( $(date +%s) - START ))s"
exit 1
