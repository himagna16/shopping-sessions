#!/bin/sh
# Diagnostic run: prints the machine's settings and times the pipeline on
# enlarged copies of the dev set under several configurations, then exits 1
# so the run is reported with its logs and never scored.
exec 1>&2
D="${DATA_DIR:-/data}"; S="${SCRATCH_DIR:-/scratch}"
START=$(date +%s)
elapsed() { echo $(( $(date +%s) - START )); }

echo "== machine"
uname -r
grep -m1 'model name' /proc/cpuinfo
echo "nproc $(nproc)  allowed $(grep Cpus_allowed_list /proc/self/status | cut -f2)"
echo "flags: $(grep -m1 '^flags' /proc/cpuinfo | tr ' ' '\n' | grep -E '^(avx|avx2|avx512[a-z0-9_]*|bmi1|bmi2|gfni|erms|fsrm|pclmulqdq|vaes|sha_ni)$' | tr '\n' ' ')"
echo "mhz: $(grep 'cpu MHz' /proc/cpuinfo | head -8 | cut -d: -f2 | tr '\n' ' ')"
echo "cache: $(grep -m1 'cache size' /proc/cpuinfo)"
for c in 0 1 2 3 4 5 6 7; do f=/sys/devices/system/cpu/cpu$c/topology/thread_siblings_list; [ -r $f ] && printf "cpu%s:%s " $c "$(cat $f)"; done; echo
echo "cpu.max $(cat /sys/fs/cgroup/cpu.max 2>&1)  cpuset $(cat /sys/fs/cgroup/cpuset.cpus.effective 2>&1)  mem.max $(cat /sys/fs/cgroup/memory.max 2>&1)"
echo "thp enabled: $(cat /sys/kernel/mm/transparent_hugepage/enabled 2>&1)  defrag: $(cat /sys/kernel/mm/transparent_hugepage/defrag 2>&1)"
grep -E 'MemTotal|MemFree|MemAvailable|^Cached|AnonHugePages' /proc/meminfo | tr -s ' ' | tr '\n' ';'; echo
grep -E ' (/data|/output|/scratch|/) ' /proc/self/mountinfo | awk '{print $5, $(NF-2), $(NF-1)}' | tr '\n' ';'; echo
df -h "$D" "$S" 2>&1 | tail -2 | tr -s ' ' | tr '\n' ';'; echo
echo "dev data: $(du -sh "$D" 2>/dev/null | cut -f1), files $(ls "$D"/events | wc -l)/$(ls "$D"/clicks | wc -l)/$(ls "$D"/links | wc -l)"

RES="$S/results.txt"; : > "$RES"
# run LABEL DATA BIN [VAR=VALUE ...]
run() {
  label="$1"; data="$2"; bin="$3"; shift 3
  rm -rf "$S/out"; mkdir -p "$S/out"
  t0=$(date +%s.%N)
  env "$@" DATA_DIR="$data" OUTPUT_DIR="$S/out" "$bin" 2> "$S/err"; rc=$?
  t1=$(date +%s.%N)
  wall=$(echo "$t1 $t0" | awk '{printf "%.3f", $1 - $2}')
  line="$label rc=$rc wall=$wall | $(grep -m1 '^threads' "$S/err" | sed 's/threads \([0-9]*\) buckets \([0-9]*\) | /t\1 b\2 /; s/ | user rows.*//') | $(grep -m1 '^user' "$S/err")"
  echo "$line"; echo "$line" >> "$RES"
}

echo "== dev set as given (start-up and exit overhead = wall - total)"
run dev "$D" /pipeline
run dev "$D" /pipeline
run dev-nohuge "$D" /pipeline SS_NOHUGE=1

echo "== expand x20 (about 20M events)"
t0=$(date +%s.%N); /expand "$D" "$S/big20" 20; t1=$(date +%s.%N)
echo "expand x20 took $(echo "$t1 $t0" | awk '{printf "%.1f", $1 - $2}') s, $(du -sh "$S/big20" | cut -f1)"
B="$S/big20"
run x20 "$B" /pipeline
run x20 "$B" /pipeline
run x20 "$B" /pipeline
run x20-nohuge "$B" /pipeline SS_NOHUGE=1
run x20-nohuge "$B" /pipeline SS_NOHUGE=1
run x20-t4 "$B" /pipeline SS_THREADS=4
run x20-t6 "$B" /pipeline SS_THREADS=6
run x20-t12 "$B" /pipeline SS_THREADS=12
run x20-t16 "$B" /pipeline SS_THREADS=16
run x20-c64k "$B" /pipeline SS_CHUNK=65536
run x20-c1m "$B" /pipeline SS_CHUNK=1048576
run x20-c4m "$B" /pipeline SS_CHUNK=4194304
run x20-avx512 "$B" /pipeline512
run x20-avx512 "$B" /pipeline512
run x20-t1 "$B" /pipeline SS_THREADS=1

if [ "$(elapsed)" -lt 95 ]; then
  echo "== expand x60 (about 60M events), at $(elapsed) s"
  rm -rf "$S/big20"
  t0=$(date +%s.%N); /expand "$D" "$S/big60" 60; t1=$(date +%s.%N)
  echo "expand x60 took $(echo "$t1 $t0" | awk '{printf "%.1f", $1 - $2}') s, $(du -sh "$S/big60" | cut -f1)"
  B="$S/big60"
  run x60 "$B" /pipeline
  [ "$(elapsed)" -lt 140 ] && run x60 "$B" /pipeline
  [ "$(elapsed)" -lt 145 ] && run x60-nohuge "$B" /pipeline SS_NOHUGE=1
  [ "$(elapsed)" -lt 150 ] && run x60-avx512 "$B" /pipeline512
fi
echo "== summary at $(elapsed) s"
cat "$RES"
exit 1
