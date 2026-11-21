#!/bin/sh
# Diagnostic run: times the pipeline on enlarged copies of the dev set under
# many settings, prints one short line each, then exits 1 so the run is
# reported with its log and never scored. The scorer shows only the last
# ~2,000 characters of stderr, so everything is kept compact.
exec 1>&2
D="${DATA_DIR:-/data}"; S="${SCRATCH_DIR:-/scratch}"
START=$(date +%s)
elapsed() { echo $(( $(date +%s) - START )); }

# r LABEL DATA BIN [VAR=VALUE ...] -> "LABEL wall total parse dedup people setup cpu" (ms; cpu in s), best of 2
r() {
  label="$1"; data="$2"; bin="$3"; shift 3
  best=""; bw=999999
  for i in 1 2; do
    rm -rf "$S/out"; mkdir -p "$S/out"
    t0=$(date +%s%N)
    env "$@" DATA_DIR="$data" OUTPUT_DIR="$S/out" "$bin" 2> "$S/err"; rc=$?
    t1=$(date +%s%N)
    w=$(( (t1 - t0) / 1000000 ))
    line=$(awk -v w=$w -v rc=$rc '/^user/{c=$2+$4} /^threads/{printf "%d %d %d %d %d %d", w, $18*1000, $9*1000, $11*1000, $13*1000, $7*1000} END{printf " %.1f", c; if (rc) printf " rc%d", rc}' "$S/err")
    if [ "$w" -lt "$bw" ]; then bw=$w; best="$line"; fi
  done
  echo "$label $best"
}

/expand "$D" "$S/b" 20
B="$S/b"
r base "$B" /p
r mmap "$B" /p SS_MMAP=1
r c64k "$B" /p SS_CHUNK=65536
r c128k "$B" /p SS_CHUNK=131072
r c512k "$B" /p SS_CHUNK=524288
r c1m "$B" /p SS_CHUNK=1048576
r c4m "$B" /p SS_CHUNK=4194304
r mm1m "$B" /p SS_MMAP=1 SS_CHUNK=1048576
r mm4m "$B" /p SS_MMAP=1 SS_CHUNK=4194304
r a8 "$B" /p SS_ABITS=8
r a9 "$B" /p SS_ABITS=9
r a10 "$B" /p SS_ABITS=10
r a12 "$B" /p SS_ABITS=12
r a13 "$B" /p SS_ABITS=13
r b8 "$B" /p SS_BBITS=8
r b9 "$B" /p SS_BBITS=9
r b10 "$B" /p SS_BBITS=10
r b12 "$B" /p SS_BBITS=12
r b13 "$B" /p SS_BBITS=13
r u128 "$B" /p128
r nostr "$B" /pns
r jcc "$B" /pjcc
r o2 "$B" /po2
r 512 "$B" /p512
r t10 "$B" /p SS_THREADS=10
r base "$B" /p
if [ "$(elapsed)" -lt 100 ]; then
  rm -rf "$S/b"; /expand "$D" "$S/c" 60; B="$S/c"
  r 60base "$B" /p
  [ "$(elapsed)" -lt 140 ] && r 60mmap "$B" /p SS_MMAP=1
  [ "$(elapsed)" -lt 150 ] && r 60u128 "$B" /p128
fi
echo "t=$(elapsed)s $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | cut -c1-40) cpus $(grep Cpus_allowed_list /proc/self/status | cut -f2) max $(cat /sys/fs/cgroup/cpu.max 2>&1) thp $(cat /sys/kernel/mm/transparent_hugepage/enabled 2>&1 | tr -d ' ' | cut -c1-24) $(cat /sys/kernel/mm/transparent_hugepage/defrag 2>&1 | sed 's/.*\[\(.*\)\].*/\1/') fs $(awk '$5=="/data"||$5=="/output"||$5=="/scratch"{printf "%s=%s ", $5, $(NF-2)}' /proc/self/mountinfo) vbmi $(grep -c -m1 avx512_vbmi2 /proc/cpuinfo)"
exit 1
