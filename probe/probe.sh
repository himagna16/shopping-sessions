#!/bin/sh
# Diagnostic run (never scored): Team 67's entry at the final's size, to see
# what its last milliseconds (memory release, process exit) cost.
exec 1>&2
S="${SCRATCH_DIR:-/scratch}"
START=$(date +%s)
run() {   # label out-dir data bin env... -> "label wall-ms t<own total> x<own exit>"
  label="$1"; out="$S/$2"; data="$3"; bin="$4"; shift 4
  rm -rf "$out"; mkdir -p "$out"
  t0=$(date +%s%N)
  env "$@" NCPU=8 XL_TIMING=1 SS_MMAP=2 DATA_DIR="$data" OUTPUT_DIR="$out" "$bin" 2> "$S/err"; rc=$?
  t1=$(date +%s%N)
  echo "$label $(( (t1 - t0) / 1000000 ))$([ $rc = 0 ] || echo " rc$rc") t$(grep -m1 'ms: ' "$S/err" | sed 's/.*total=\([0-9]*\).*/\1/') x$(grep -m1 'exit at' "$S/err" | awk '{print int($4)}')"
}
echo "$(awk '$2=="/data"||$2=="/scratch"||$2=="/output"||$2=="/tmp"{printf "%s=%s ", $2, $3}' /proc/mounts)$(awk '/MemTotal|MemAvailable/{printf "%s%dG ", substr($1,4,5), $2/1048576}' /proc/meminfo) max=$(cat /sys/fs/cgroup/memory.max 2>/dev/null | cut -c1-12) thp=$(sed 's/.*\[\(.*\)\].*/\1/' /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null) shm=$(sed 's/.*\[\(.*\)\].*/\1/' /sys/kernel/mm/transparent_hugepage/shmem_enabled 2>/dev/null)"
E="${PROBE_E:-20000000}"
E3=$(( E * 3 ))
/generate --seed 11 --events "$E3" --clicks $(( E3 / 4 )) --output "$S/c" > /dev/null 2>&1; echo "gen60 rc$? t=$(( $(date +%s) - START ))s"
sync; echo "sync t=$(( $(date +%s) - START ))s"
B="$S/c"
run w o1 "$B" /xl
run w o1 "$B" /xl
run w o1 "$B" /xl
for i in 1 2 3; do
run xl o1 "$B" /xl
run e0 o1 "$B" /xl XL_ENDREL=0
run e1 o1 "$B" /xl XL_ENDREL=1
run e2 o1 "$B" /xl XL_ENDREL=2
run r0 o1 "$B" /xl XL_REL=0
run rg0 o1 "$B" /xl XL_RGREL=0
run a0 o1 "$B" /xl XL_ALLOC=0
done
run ours o2 "$B" /p
echo "cmp60 $(python3 /cmp.py "$S/o1" "$S/o2" 2>&1 | tail -1 | cut -c1-120)"
echo "t=$(( $(date +%s) - START ))s"
exit 1
