#!/usr/bin/env python3
"""Order-independent comparison of two output folders (any Parquet encodings):
row counts, duplicate keys and a sum of row hashes per table. Usage: cmp.py A B"""
import sys, duckdb
U = "hash(user_id, CAST(day AS DATE), CAST(sessions AS BIGINT), CAST(events AS BIGINT), CAST(revenue_cents AS BIGINT), top_category)"
C = "hash(campaign_id, channel, CAST(day AS DATE), CAST(sessions AS BIGINT), CAST(purchasing_sessions AS BIGINT), CAST(revenue_cents AS BIGINT), CAST(assisted_sessions AS BIGINT))"
def digest(folder):
    out = []
    for table, expr, key in (("user_daily", U, "(user_id, day)"), ("campaign_daily", C, "(campaign_id, day)")):
        try:
            n, d, h = duckdb.execute(f"SELECT count(*), count(DISTINCT {key}), sum({expr}) FROM read_parquet('{folder}/{table}/*.parquet')").fetchone()
            out.append((n, d, format(int(h or 0) & 0xFFFFFFFF, "08x")))
        except Exception as exc:
            out.append(("ERR", type(exc).__name__, str(exc)[:60]))
    return out
a, b = digest(sys.argv[1]), digest(sys.argv[2])
print("SAME" if a == b and "ERR" not in str(a) else "DIFF", a if a == b else (a, b))
