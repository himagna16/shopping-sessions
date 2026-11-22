#!/bin/sh
# Diagnostic run (never scored): the CPU's own counters for our parse, step by
# step (each plN build stops the event row after step N), on generator data.
exec 1>&2
S="${SCRATCH_DIR:-/scratch}"
START=$(date +%s)
ps() {    # label bin env... -> "label parse-wall parse-cpu | counters (millions; t ms; pf thousands)"
  label="$1"; bin="$2"; shift 2
  best=""; bc=999999
  for i in 1 2; do
    rm -rf "$S/out"; mkdir -p "$S/out"
    /pstat env SS_MMAP=2 SS_STOP=1 "$@" DATA_DIR="$B" OUTPUT_DIR="$S/out" "$bin" 2> "$S/err"
    line="$(awk '/^threads/{printf "%d %d", $9*1000, $23*1000}' "$S/err") $(grep -m1 '^pstat' "$S/err" | cut -c7-)"
    c=$(echo "$line" | awk '{print $2}')
    if [ -n "$c" ] && [ "$c" -lt "$bc" ]; then bc=$c; best="$line"; fi
  done
  echo "$label $best"
}
E="${PROBE_E:-20000000}"
/generate --seed 7 --events "$E" --clicks $(( E / 4 )) --output "$S/b" > /dev/null 2>&1; echo "gen rc$? $(( $(date +%s) - START ))s"
sync
B="$S/b"
echo "rows $(cat "$B"/events/* | wc -l) $(cat "$B"/clicks/* | wc -l) paranoid $(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null)"
echo "$(grep -m1 -o 'avx512[a-z_0-9]*\|bmi2\|gfni\|vaes' /proc/cpuinfo | sort -u | tr '\n' ' ')" | cut -c1-200
ps warm /p
for v in pl1 pl2 pl3 pl4 pl5 pl6 pl7 p; do ps $v /$v; done
ps p-t4 /p SS_THREADS=4
ps p-t1 /p SS_THREADS=1
ps pl2-t1 /pl2 SS_THREADS=1
ps pl5-t1 /pl5 SS_THREADS=1
echo "t=$(( $(date +%s) - START ))s"
exit 1
