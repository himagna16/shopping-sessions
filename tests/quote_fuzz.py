#!/usr/bin/env python3
"""Quote-placement fuzzer: rewrites the quoting of random fields of a small
generated dataset (well-formed and malformed, with random padding rows so
positions shift against 64-byte blocks), then checks BIN against the reference.
Usage: quote_fuzz.py BIN SEED_DATASET COUNT [SEED]"""
import os, random, shutil, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "reference"))
sys.path.insert(0, os.path.join(HERE, "..", "..", "ref", "starter-duckdb", "starter-duckdb", "scripts"))
import ref_impl
from run_fixtures import SCHEMAS, load_actual
import polars as pl

def requote(field, rng):
    r = rng.random()
    if r < 0.55: return field
    if r < 0.80: return b'"' + field + b'"'
    kind = rng.randrange(9)
    if kind == 0: return b'"'
    if kind == 1: return b'""' if not field else b'"' + field
    if kind == 2: return field + b'"'
    if kind == 3: return b'"' + field
    if kind == 4: return field[:len(field)//2] + b'"' + field[len(field)//2:]
    if kind == 5: return b'""' + field + b'"'
    if kind == 6: return b'"' + field + b'""'
    if kind == 7: return b'"""'
    return b'"' + field[:1] + b'"' + field[1:] + b'"'

def mutate(path, rng, width):
    with open(path, "rb") as f: lines = f.read().split(b"\n")
    out = [lines[0]]
    for line in lines[1:]:
        if line == b"": continue
        fields = line.split(b",")
        if len(fields) == width and b'"' not in line and rng.random() < 0.9:
            fields = [requote(x, rng) for x in fields]
        out.append(b",".join(fields))
        if rng.random() < 0.15:                       # a malformed filler row of random length shifts everything after it
            out.append(b"x" * rng.randrange(1, 90))
    with open(path, "wb") as f: f.write(b"\n".join(out) + b"\n")

def main():
    binary, seed_dir, count = sys.argv[1], sys.argv[2], int(sys.argv[3])
    rng = random.Random(int(sys.argv[4]) if len(sys.argv) > 4 else 1)
    bad = 0
    for i in range(count):
        work = tempfile.mkdtemp(prefix="qf-")
        data = os.path.join(work, "data")
        shutil.copytree(seed_dir, data)
        for folder, width in (("events", 7), ("clicks", 4), ("links", 2)):
            for name in os.listdir(os.path.join(data, folder)):
                mutate(os.path.join(data, folder, name), rng, width)
        # the reference raises on an unreadable ingest_ts; quoting never changes values, so it cannot arise here
        users, campaigns = ref_impl.run(data)
        for threads, chunk in (("8", ""), ("3", "97"), ("1", "4096")):
            out = os.path.join(work, "out" + threads)
            env = dict(os.environ, DATA_DIR=data, OUTPUT_DIR=out, SS_THREADS=threads)
            if chunk: env["SS_CHUNK"] = chunk
            p = subprocess.run([binary], env=env, capture_output=True)
            got_u = sorted(load_actual(os.path.join(out, "user_daily"), SCHEMAS["user_daily"]).rows()) if p.returncode == 0 else None
            got_c = sorted(load_actual(os.path.join(out, "campaign_daily"), SCHEMAS["campaign_daily"]).rows()) if p.returncode == 0 else None
            import datetime
            want_u = sorted((u, datetime.date.fromisoformat(d), s, e, r, t or None) for u, d, s, e, r, t in users)
            want_c = sorted((c, ch, datetime.date.fromisoformat(d), s, p2, r, a) for c, ch, d, s, p2, r, a in campaigns)
            key = lambda rows: sorted(tuple("" if v is None else str(v) for v in row) for row in rows)
            if got_u is None or key(got_u) != key(want_u) or key(got_c) != key(want_c):
                bad += 1
                print(f"FAIL case {i} threads {threads} chunk {chunk!r}: kept at {work}")
                break
        else:
            shutil.rmtree(work, ignore_errors=True)
    print(f"{count - bad}/{count} quote-fuzz cases passed")
    sys.exit(1 if bad else 0)

main()
