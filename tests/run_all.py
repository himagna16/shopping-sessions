#!/usr/bin/env python3
"""
Differential-testing driver.  Usage:
  run_all.py BIN [FILTER] [--jobs N] [--timeout S] [--configs a,b,..] [--keep] [--list]
  run_all.py --one BIN DATA_DIR EXPECTED_DIR        (single run, inherits SS_* from the environment)

Datasets: fixtures (36 organizer cases), dev, data/seeds/*, data/adversarial/* (incl. fuzz*).
FILTER is a regex searched in "<kind>/<name>" (kinds: fixture, dev, seed, adv), e.g. 'fuzz', '^fixture/E1'.
Comparison = the organizers' scripts/compare.py (imported and called), expected side being CSV folders.
"""
import argparse, concurrent.futures as cf, contextlib, io, os, re, shutil, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DATA = os.path.join(ROOT, "data")
SCRIPTS = os.path.join(ROOT, "ref", "starter-duckdb", "starter-duckdb", "scripts")
FIXTURES = os.path.join(ROOT, "ref", "starter-duckdb", "starter-duckdb", "fixtures")

CONFIGS = {  # name -> env overrides; "tiny" configs set SS_CHUNK
    "default": {},
    "t1": {"SS_THREADS": "1"},
    "t3c4096": {"SS_THREADS": "3", "SS_CHUNK": "4096"},
    "t8c777": {"SS_THREADS": "8", "SS_CHUNK": "777"},
    # the dense path switched off; abandoned after the parse; abandoned after its output was written
    "nodense": {"SS_DENSE": "0"},
    "bail2": {"SS_DENSE_BAIL": "2", "SS_THREADS": "3"},
    "bail1": {"SS_DENSE_BAIL": "1", "SS_THREADS": "5", "SS_CHUNK": "65536"},
}
BIG_BYTES = 100 * 1024 * 1024


def dir_size(path):
    total = 0
    for dp, _, fns in os.walk(path):
        for fn in fns:
            try:
                total += os.path.getsize(os.path.join(dp, fn))
            except OSError:
                pass
    return total


def discover():
    out = []
    if os.path.isdir(FIXTURES):
        for case in sorted(os.listdir(FIXTURES)):
            d = os.path.join(FIXTURES, case)
            if os.path.isdir(os.path.join(d, "input")):
                out.append(("fixture", case, os.path.join(d, "input"), os.path.join(d, "expected")))
    if os.path.isdir(os.path.join(DATA, "dev")):
        out.append(("dev", "dev", os.path.join(DATA, "dev"), os.path.join(DATA, "expected", "dev")))
    for kind, sub, prefix in (("seed", "seeds", ""), ("adv", "adversarial", "adv_")):
        base = os.path.join(DATA, sub)
        if os.path.isdir(base):
            for name in sorted(os.listdir(base)):
                if os.path.isdir(os.path.join(base, name)):
                    out.append((kind, name, os.path.join(base, name), os.path.join(DATA, "expected", prefix + name)))
    return out


def compare_dirs(expected, actual):
    """Run the organizers' compare.main(expected, actual); returns (ok, text)."""
    sys.path.insert(0, SCRIPTS)
    import compare
    buf = io.StringIO()
    old = sys.argv
    sys.argv = ["compare.py", expected, actual]
    try:
        with contextlib.redirect_stdout(buf):
            rc = compare.main()
    except Exception as exc:  # unreadable/incorrectly typed output
        return False, f"output error: {type(exc).__name__}: {exc}"
    finally:
        sys.argv = old
    return rc == 0, buf.getvalue()


def run_once(binary, data, expected, cfg_env, timeout, tmp_root, keep, inherit=False):
    work = tempfile.mkdtemp(prefix="ss-", dir=tmp_root)
    out, scratch = os.path.join(work, "out"), os.path.join(work, "scratch")
    os.makedirs(out); os.makedirs(scratch)
    env = {k: v for k, v in os.environ.items() if not (k.startswith("SS_") and not inherit)}
    env.update(cfg_env)
    env.update(DATA_DIR=data, OUTPUT_DIR=out, SCRATCH_DIR=scratch)
    t0 = time.time()
    try:
        p = subprocess.run([binary], env=env, capture_output=True, timeout=timeout, cwd=work)
        secs = time.time() - t0
        if p.returncode != 0:
            ok, text = False, f"exit status {p.returncode}\n" + p.stderr.decode("utf-8", "replace")[-1500:]
        else:
            ok, text = compare_dirs(expected, out)
    except subprocess.TimeoutExpired:
        secs = time.time() - t0
        ok, text = False, f"timeout after {timeout}s"
    if ok and not keep:
        shutil.rmtree(work, ignore_errors=True)
    else:
        text += f"\n  (A = expected, B = BIN output; outputs kept in {work})" if not ok else ""
    return ok, secs, text


def task(args):
    binary, kind, name, data, expected, cfgname, timeout, tmp_root, keep = args
    ok, secs, text = run_once(binary, data, expected, CONFIGS[cfgname], timeout, tmp_root, keep)
    return kind, name, cfgname, ok, secs, text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary")
    ap.add_argument("filter", nargs="?", default="")
    ap.add_argument("--jobs", type=int, default=int(os.environ.get("RUN_ALL_JOBS", max(2, (os.cpu_count() or 4) // 2))))
    ap.add_argument("--timeout", type=float, default=float(os.environ.get("RUN_ALL_TIMEOUT", 900)))
    ap.add_argument("--configs", default=os.environ.get("RUN_ALL_CONFIGS", ",".join(CONFIGS)))
    ap.add_argument("--keep", action="store_true", help="keep outputs of passing runs too")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--tmp", default=os.environ.get("TMPDIR", "/tmp"))
    ap.add_argument("--one", nargs=2, metavar=("DATA_DIR", "EXPECTED_DIR"))
    a = ap.parse_args()
    binary = os.path.abspath(a.binary)
    if a.one:
        ok, secs, text = run_once(binary, os.path.abspath(a.one[0]), os.path.abspath(a.one[1]), {}, a.timeout, a.tmp, a.keep, inherit=True)
        print(text.rstrip())
        print(f"{'PASS' if ok else 'FAIL'} {secs:.2f}s")
        return 0 if ok else 1

    rx = re.compile(a.filter) if a.filter else None
    cfgs = a.configs.split(",")
    small, big, skipped = [], [], []
    for kind, name, data, exp in discover():
        if rx and not rx.search(f"{kind}/{name}"):
            continue
        if not os.path.exists(os.path.join(exp, "user_daily.csv")):
            skipped.append(f"{kind}/{name}"); continue
        size = dir_size(data)
        is_big = size >= BIG_BYTES
        for c in cfgs:
            if is_big and "SS_CHUNK" in CONFIGS[c]:
                continue
            (big if is_big else small).append((binary, kind, name, data, exp, c, a.timeout, a.tmp, a.keep))
    if a.list:
        for t in small + big:
            print(t[1], t[2], t[5])
        return 0
    total = len(small) + len(big)
    print(f"{total} runs ({len(small)} parallel x{a.jobs}, {len(big)} serial); BIN={binary}", flush=True)
    for s in skipped:
        print(f"SKIP {s}: no expected output (reference crash?)")
    fails, done, t_start = [], 0, time.time()

    def report(res):
        nonlocal done
        kind, name, cfg, ok, secs, text = res
        done += 1
        print(f"{'PASS' if ok else 'FAIL'} {kind}/{name} [{cfg}] {secs:.2f}s", flush=True)
        if not ok:
            fails.append(f"{kind}/{name} [{cfg}]")
            print("    " + text.strip().replace("\n", "\n    "), flush=True)

    if small:
        with cf.ProcessPoolExecutor(a.jobs) as ex:
            for fut in cf.as_completed([ex.submit(task, t) for t in small]):
                report(fut.result())
    for t in big:   # one at a time so timings are representative
        report(task(t))
    wall = time.time() - t_start
    print(f"\nSUMMARY: {total - len(fails)}/{total} passed, {len(fails)} failed, {len(skipped)} skipped; wall {wall:.1f}s")
    for f in fails[:50]:
        print("  FAILED", f)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
