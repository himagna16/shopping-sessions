#!/usr/bin/env bash
# Creates build/venv with polars, pyarrow, duckdb (reused if already present).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
VENV="$ROOT/build/venv"
if [ -x "$VENV/bin/python" ] && "$VENV/bin/python" -c 'import polars, pyarrow, duckdb' 2>/dev/null; then
  echo "venv ok: $VENV"; exit 0
fi
for py in python3.14 python3.13 python3.12 python3.11 python3; do
  command -v "$py" >/dev/null 2>&1 || continue
  rm -rf "$VENV"
  "$py" -m venv "$VENV"
  if "$VENV/bin/pip" install --quiet --upgrade pip && "$VENV/bin/pip" install --quiet polars pyarrow duckdb; then
    echo "venv created with $py: $VENV"; exit 0
  fi
done
echo "could not create venv" >&2; exit 1
