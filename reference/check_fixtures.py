#!/usr/bin/env python3
"""Run ref_impl.run() on every fixture and compare with expected/ (exact, order-insensitive).
Usage: python3 -I check_fixtures.py FIXTURES_DIR"""
import csv
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ref_impl  # noqa: E402


def expected(path):
    with open(path, newline="", encoding="utf-8") as handle:
        rows = list(csv.reader(handle))
    out = []
    for row in rows[1:]:
        out.append(tuple(int(value) if value.isdigit() and index not in (0, 1) else value
                         for index, value in enumerate(row)))
    return sorted(out)


def typed(rows, table):
    # user_daily: user_id, day, ints..., top_category ; campaign_daily: id, channel, day, ints...
    return sorted(tuple(rows_) for rows_ in rows)


def main():
    root = sys.argv[1]
    failed = 0
    cases = sorted(name for name in os.listdir(root) if os.path.isdir(os.path.join(root, name)))
    for case in cases:
        users, campaigns = ref_impl.run(os.path.join(root, case, "input"))
        ok = True
        for table, actual in (("user_daily", users), ("campaign_daily", campaigns)):
            want = expected(os.path.join(root, case, "expected", f"{table}.csv"))
            got = sorted(tuple(str(v) if i in (0, 1) else v for i, v in enumerate(row)) for row in actual)
            want = sorted(tuple(row) for row in want)
            if [tuple(map(str, r)) for r in got] != [tuple(map(str, r)) for r in want]:
                ok = False
                print(f"FAIL {case} {table}\n  expected {want}\n  actual   {got}")
        print(("ok   " if ok else "FAIL ") + case)
        failed += not ok
    print(f"\n{len(cases) - failed}/{len(cases)} fixtures passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
