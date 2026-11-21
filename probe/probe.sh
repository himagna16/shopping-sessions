#!/bin/sh
# Diagnostic run (never scored): the dense path against the general one on a
# dataset the organizers' generator writes here, at the practice size.
# Output stays under ~1,900 characters.
exec 1>&2
S="${SCRATCH_DIR:-/scratch}"
START=$(date +%s)
one() {   # one run -> "wall total parse dedup people parse_cpu links user sys"
  data="$1"; bin="$2"; shift 2
  rm -rf "$S/out"; mkdir -p "$S/out"
  t0=$(date +%s%N)
  env SS_MMAP=2 "$@" DATA_DIR="$data" OUTPUT_DIR="$S/out" "$bin" 2> "$S/err"; rc=$?
  t1=$(date +%s%N)
  awk -v w=$(( (t1 - t0) / 1000000 )) -v rc=$rc '/^user/{u=$2*1000; s=$4*1000} /^threads/{printf "%d %d %d %d %d %d %d %d %d", w, $18*1000, $9*1000, $11*1000, $13*1000, $23*1000, $NF*1000, u, s} END{if (rc) printf " rc%d", rc}' "$S/err"
}
r() {     # best of 3 by wall -> "wall total parse dedup people parse_cpu links user sys"
  label="$1"; shift
  best=""; bw=999999
  for i in 1 2 3; do
    line=$(one "$@"); w=${line%% *}
    if [ -n "$w" ] && [ "$w" -lt "$bw" ]; then bw=$w; best="$line"; fi
  done
  echo "$label $best"
}
dl() {    # the dense path's own line from the last run, numbers only (ms)
  awk -v l="$1" '/^dense \|/{printf "%s set %.1f hand %s cpu pl %d sw %d wk %d st %d wall se %.1f wk %.1f jn %.1f st %.1f\n", l, $4*1000, $27, $42*1000, $44*1000, $46*1000, $48*1000, $52*1000, $54*1000, $56*1000, $58*1000}' "$S/err"
}
sum() {   # one thread, so the files do not depend on timing: a digest of everything written
  label="$1"; shift
  one "$@" SS_THREADS=1 > /dev/null
  echo "$label $(cd "$S/out" && find . -type f | sort | xargs cat | sha256sum | cut -c1-12) $(find "$S/out" -type f | wc -l)"
}
E="${PROBE_E:-20000000}"
/generate --seed 7 --events "$E" --clicks $(( E / 4 )) --output "$S/b" > /dev/null 2>&1; G=$(( $(date +%s) - START )); echo "gen rc$? ${G}s $(du -sm "$S/b" | cut -f1)MB"
B="$S/b"
for round in 1 2; do
r dense "$B" /p
dl " "
r gen "$B" /p SS_DENSE=0
done
r dw13 "$B" /p13
dl " "
r dw15 "$B" /p15
dl " "
r cw8 "$B" /pc8
r cw12 "$B" /pc12
r pread "$B" /p SS_MMAP=0
r t7 "$B" /p SS_THREADS=7
r b8 "$B" /p SS_BBITS=8
echo "t=$(( $(date +%s) - START ))s"
rm -rf "$S/b"
if [ "$G" -le 22 ]; then
E3=$(( E * 3 ))
/generate --seed 9 --events "$E3" --clicks $(( E3 / 4 )) --output "$S/c" > /dev/null 2>&1; echo "gen60 rc$? t=$(( $(date +%s) - START ))s"
B="$S/c"
r 60dense "$B" /p
dl " "
r 60gen "$B" /p SS_DENSE=0
r 60dw15 "$B" /p15
dl " "
r 60dense "$B" /p
fi
echo "t=$(( $(date +%s) - START ))s"
exit 1
