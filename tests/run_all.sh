#!/usr/bin/env bash
# Usage: run_all.sh BIN [filter-regex] [extra run_all.py options]
# Runs BIN on all fixtures, dev, seed and adversarial/fuzz datasets under 4 configs
# (default; SS_THREADS=1; SS_THREADS=3 SS_CHUNK=4096; SS_THREADS=8 SS_CHUNK=777) and compares with the oracle.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PY="$HERE/../../build/venv/bin/python"; [ -x "$PY" ] || { "$HERE/setup_venv.sh"; }
[ -x "$1" ] || { echo "BIN not executable: $1" >&2; exit 2; }
exec "$PY" -I "$HERE/run_all.py" "$@"
