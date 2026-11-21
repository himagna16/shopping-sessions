#!/usr/bin/env bash
# Usage: run_one.sh BIN DATA_DIR EXPECTED_DIR   (EXPECTED_DIR holds user_daily.csv + campaign_daily.csv)
# SS_THREADS / SS_CHUNK from your environment are passed through.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PY="$HERE/../../build/venv/bin/python"
[ $# -eq 3 ] || { echo "usage: $0 BIN DATA_DIR EXPECTED_DIR" >&2; exit 2; }
exec "$PY" -I "$HERE/run_all.py" --one "$2" "$3" "$1"
