#!/usr/bin/env bash
# Stand-in BIN: runs the pure-Python reference and writes its tables as Parquet (day as Date).
# Env: DATA_DIR, OUTPUT_DIR, SCRATCH_DIR (SS_THREADS / SS_CHUNK ignored).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/../.."
PY="$ROOT/build/venv/bin/python"
tmp="${SCRATCH_DIR:?}/refbin"; mkdir -p "$tmp"
"$PY" -I "$ROOT/reference/ref_impl.py" "$DATA_DIR" "$tmp" 2>/dev/null
# REFBIN_BREAK=1 deliberately corrupts one row (to prove the harness notices)
exec "$PY" -I -c '
import os, sys
sys.path.insert(0, sys.argv[1])
import polars as pl
from run_fixtures import SCHEMAS, load_expected
tmp, out = sys.argv[2], os.environ["OUTPUT_DIR"]
for table, schema in SCHEMAS.items():
    df = load_expected(os.path.join(tmp, table + ".csv"), schema)
    df = df.with_columns([pl.col(c).cast(pl.Int64) for c, t in schema.items() if t == pl.Int128])
    if os.environ.get("REFBIN_BREAK") and df.height and table == "user_daily":
        df = df.with_columns(pl.when(pl.int_range(pl.len()) == df.height // 2).then(pl.col("revenue_cents") + 1).otherwise(pl.col("revenue_cents")).alias("revenue_cents"))
    os.makedirs(os.path.join(out, table), exist_ok=True)
    df.write_parquet(os.path.join(out, table, "part-0.parquet"))
' "$ROOT/ref/starter-duckdb/starter-duckdb/scripts" "$tmp" 
