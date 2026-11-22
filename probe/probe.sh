#!/bin/sh
# Diagnostic run (never scored): what the public dev set mounted at /data looks
# like under scorer version 5 (long IDs), and how our two paths take it.
exec 1>&2
S="${SCRATCH_DIR:-/scratch}"
D="${DATA_DIR:-/data}"
ls "$D" | tr '\n' ' '; echo
for k in events clicks links; do f=$(ls "$D/$k"/* | head -1); echo "$k $(ls "$D/$k" | wc -l) files $(cat "$D/$k"/* | wc -l) lines"; sed -n '2,7p' "$f" | cut -c1-150; done
E=$(ls "$D/events"/* | head -1)
awk -F, 'NR>1 && NR<200000 {a=$1; gsub(/"/,"",a); b=$2; gsub(/"/,"",b); la[length(a)]++; lb[length(b)]++} END{printf "event id lengths:"; for(k in la) printf " %s:%d", k, la[k]; printf "\nuser id lengths:"; for(k in lb) printf " %s:%d", k, lb[k]; print ""}' "$E" | cut -c1-400
one() { label="$1"; shift; rm -rf "$S/o"; mkdir -p "$S/o"; t0=$(date +%s%N); env "$@" DATA_DIR="$D" OUTPUT_DIR="$S/o" /p 2> "$S/err"; rc=$?; t1=$(date +%s%N); echo "$label $(( (t1 - t0) / 1000000 ))ms rc$rc"; grep -E '^threads|^dense' "$S/err" | cut -c1-330; }
one dense
one general SS_DENSE=0
one general1 SS_DENSE=0 SS_THREADS=1
exit 1
