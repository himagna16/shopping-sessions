#!/usr/bin/env bash
# Idempotently generate seed datasets (data/seeds/<name>) and reference expectations
# (data/expected/<name>/*.csv). Skips any dataset whose expected output already exists.
# Usage: make_data.sh [-j JOBS]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PY="$ROOT/build/venv/bin/python"; [ -x "$PY" ] || PY=python3
REF="$ROOT/reference/ref_impl.py"
SEEDS="$ROOT/data/seeds"; EXP="$ROOT/data/expected"
JOBS=6; [ "${1:-}" = "-j" ] && JOBS="$2"
mkdir -p "$SEEDS" "$EXP"

# name seed events clicks
specs() {
  echo "big7 7 3000000 750000"   # slowest first
  for s in $(seq 2 21);   do echo "seed$s $s 200000 50000"; done
  for s in $(seq 100 119); do echo "small$s $s 1000 250"; done
  for s in $(seq 200 204); do echo "clicky$s $s 20000 30000"; done
}

one() {  # name seed events clicks
  local name=$1 seed=$2 ev=$3 cl=$4
  local out="$EXP/$name"
  [ -f "$out/user_daily.csv" ] && [ -f "$out/campaign_daily.csv" ] && { echo "cached $name"; return 0; }
  if [ ! -f "$SEEDS/$name/manifest.json" ]; then
    rm -rf "$SEEDS/$name.tmp"
    docker run --rm --network none \
      -v "$ROOT/build/generator-target:/target:ro" -v "$SEEDS:/data" \
      rust:1-slim-bookworm /target/release/generate --seed "$seed" --events "$ev" --clicks "$cl" --output "/data/$name.tmp" >/dev/null
    rm -rf "$SEEDS/$name"; mv "$SEEDS/$name.tmp" "$SEEDS/$name"
  fi
  rm -rf "$out.tmp"
  "$PY" -I "$REF" "$SEEDS/$name" "$out.tmp" 2>/dev/null
  rm -rf "$out"; mv "$out.tmp" "$out"
  echo "done   $name"
}
export -f one; export ROOT PY REF SEEDS EXP

specs | xargs -P "$JOBS" -L1 bash -c 'one "$@"' _

# register the dev set with its published answer
mkdir -p "$EXP"
[ -e "$EXP/dev" ] || ln -s "$ROOT/ref/dev-answer/dev-answer" "$EXP/dev"
[ -e "$ROOT/data/dev/manifest.json" ] || echo "WARNING: data/dev missing" >&2
echo "make_data complete"
