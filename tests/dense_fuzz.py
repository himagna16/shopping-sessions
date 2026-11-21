#!/usr/bin/env python3
"""Fuzzer for the dense path: takes a small generated dataset, reads the scramble
keys the binary recovered, and rewrites rows so that they break, one way or
another, what the dense path relies on (a person's events in one stretch of
numbers and in time order, links inside one person's first 16 IDs, IDs that
decode, ingest times near the window). Every case is checked against the
reference under each binary and thread setting.
Usage: dense_fuzz.py BASE_DATASET COUNT SEED BIN [BIN...]"""
import datetime, os, random, re, shutil, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "reference"))
sys.path.insert(0, os.path.join(HERE, "..", "..", "ref", "starter-duckdb", "starter-duckdb", "scripts"))
import ref_impl
from run_fixtures import SCHEMAS, load_actual

MASK = (1 << 40) - 1
M1, M2 = 0x9E3779B97F4B, 0xD6E8FEB86659
M1I, M2I = pow(M1, -1, 1 << 40), pow(M2, -1, 1 << 40)
DIG = "0123456789abcdefghijklmnopqrstuvwxyz"
EPOCH = datetime.datetime(1970, 1, 1, tzinfo=datetime.timezone.utc)
DAY = 86_400_000

def scramble(v, key):
    m = ((v * M1) ^ key) & MASK
    m ^= m >> 17
    return (m * M2) & MASK

def unscramble(x, key):
    y = (x * M2I) & MASK
    z = y ^ (y >> 17) ^ (y >> 34)
    return ((z ^ key) * M1I) & MASK

def b36(v):
    out = ""
    while True:
        out = DIG[v % 36] + out
        v //= 36
        if not v: return out

def canon(text, letter):
    """the number an ID spells, or None"""
    if len(text) < 2 or len(text) > 9 or text[0] != letter: return None
    body = text[1:]
    if any(c not in DIG for c in body) or (len(body) > 1 and body[0] == "0"): return None
    v = int(body, 36)
    return v if v <= MASK else None

def f1(ms):
    d = EPOCH + datetime.timedelta(milliseconds=ms - ms % 1000)
    return d.strftime("%Y-%m-%dT%H:%M:%SZ")

def parse_f1(text):
    try:
        d = datetime.datetime.strptime(text, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=datetime.timezone.utc)
    except ValueError:
        return None
    return int((d - EPOCH).total_seconds()) * 1000

class State:
    def __init__(self, data, ekey, ukey, rng):
        self.data, self.ekey, self.ukey, self.rng = data, ekey, ukey, rng
        self.files = {}
        for folder in ("events", "clicks", "links"):
            for name in sorted(os.listdir(os.path.join(data, folder))):
                raw = open(os.path.join(data, folder, name), "rb").read().decode("utf-8", "replace").split("\n")
                rows = [[f[1:-1] if len(f) >= 2 and f[0] == '"' and f[-1] == '"' else f for f in line.split(",")] for line in raw[1:] if line != ""]
                self.files[(folder, name)] = [raw[0], rows]
        import json
        man = json.load(open(os.path.join(data, "manifest.json")))
        self.ws, self.we = parse_f1(man["window_start"]), parse_f1(man["window_end"])
        self.ingests = {}                       # event id -> ingest times of its well-formed rows
        self.click_ids = set()
        self.users = []
        people = set()
        for (folder, name), (_, rows) in self.files.items():
            for r in rows:
                if folder == "events" and len(r) == 7:
                    self.ingests.setdefault(r[0], []).append(parse_f1(r[3]))
                    self.users.append(r[1])
                if folder == "clicks" and len(r) == 4: self.click_ids.add(r[0])
        for u in self.users:
            v = canon(u, "u")
            if v is not None: people.add(unscramble(v, ukey) >> 9)
        self.people = sorted(p for p in people if p < 1 << 22) or [0]
        self.events = sum(len(v) for v in self.ingests.values())

    def rows(self, folder, width):
        return [r for (f, _), (_, rows) in self.files.items() if f == folder for r in rows if len(r) == width]

    def any_file(self, folder):
        return self.rng.choice([rows for (f, _), (_, rows) in self.files.items() if f == folder])

    def user_of(self, person, alias):
        return "u" + b36(scramble(person * 512 + alias, self.ukey))

    def some_user(self):
        rng = self.rng
        r = rng.random()
        if r < 0.35: return rng.choice(self.users)
        person = rng.choice(self.people)
        if r < 0.60: return self.user_of(person, rng.randrange(0, 4))
        if r < 0.75: return self.user_of(person, rng.randrange(4, 16))
        if r < 0.85: return self.user_of(person, rng.randrange(16, 40))
        if r < 0.90: return self.user_of(max(self.people) + rng.randrange(1, 100000), rng.randrange(0, 3))
        if r < 0.93: return "u" + b36((1 << 39) + rng.randrange(1, 4))
        if r < 0.97: return rng.choice(self.users).upper()
        return rng.choice(["u0" + rng.choice(self.users)[1:], "x1", "U", "u" + "z" * 8, "u" + "z" * 9, " " + rng.choice(self.users)])

    def unique_event(self):
        rows = self.rows("events", 7)
        for _ in range(50):
            r = self.rng.choice(rows)
            if len(self.ingests.get(r[0], [])) == 1 and r[0] != "": return r
        return None

    def window_ts(self):
        rng = self.rng
        t = self.ws + rng.randrange(self.we - self.ws)
        return t - t % 1000 if rng.random() < 0.8 else t

    def ts_text(self, ms):
        return f1(ms) if ms % 1000 == 0 and self.rng.random() < 0.7 else str(ms)

    def set_event_id(self, row, new):
        old = row[0]
        ing = parse_f1(row[3])
        taken = self.ingests.setdefault(new, [])
        if ing in taken or ing is None:
            return False                         # would tie with a copy that differs
        self.ingests[old].remove(ing)
        taken.append(ing)
        row[0] = new
        return True

def m_event_user(st):
    r = st.unique_event()
    if r: r[1] = st.some_user(); return "event_user"

def m_event_alias(st):
    r = st.unique_event()
    if not r: return
    v = canon(r[1], "u")
    if v is None: return
    n = unscramble(v, st.ukey)
    r[1] = st.user_of(n >> 9, st.rng.randrange(0, 18))
    return "event_alias"

def m_event_ts(st):
    r = st.unique_event()
    if not r: return
    ts = st.window_ts()
    r[2] = st.ts_text(ts)
    r[3] = f1(ts + st.rng.choice([0, 1000, 60_000, 600_000, DAY, DAY + 1000, -5000]))
    st.ingests[r[0]] = [parse_f1(r[3])]
    return "event_ts"

def m_event_id(st):
    r = st.unique_event()
    if not r: return
    rng = st.rng
    k = rng.randrange(7)
    if k == 0: new = r[0].upper()
    elif k == 1: new = "e" + "".join(rng.choice(DIG) for _ in range(8))
    elif k == 2: new = "e0" + r[0][1:]
    elif k == 3: new = "e" + b36(scramble(rng.randrange(0, 4 * st.events + 10), st.ekey))     # another number, used or not
    elif k == 4: new = "e" + b36(scramble(rng.randrange(1 << 30, 1 << 40), st.ekey))
    elif k == 5: new = rng.choice(["x", "E", "e", "ez" + "9" * 8, " " + r[0]])
    else: new = "e" + b36(rng.randrange(MASK + 1, 36 ** 8))                                  # spells a value over 2^40
    if st.set_event_id(r, new): return "event_id"

def m_copy(st):
    rng = st.rng
    rows = st.rows("events", 7)
    src = rng.choice(rows)
    known = [t for t in st.ingests.get(src[0], []) if t is not None]
    if not known or src[0] == "": return
    new = list(src)
    r = rng.random()
    if r < 0.45: ing = min(known) - rng.randrange(1, 900) * 1000          # becomes the kept copy
    elif r < 0.9: ing = max(known) + rng.randrange(1, 900) * 1000
    elif r < 0.95: ing = parse_f1("1999-12-31T23:59:59Z") - rng.randrange(0, 1000) * 1000
    else: ing = parse_f1("2031-01-01T00:00:00Z") + rng.randrange(0, 1000) * 1000
    if ing in st.ingests[src[0]]: return
    new[3] = f1(ing)
    for _ in range(rng.randrange(0, 3)):
        k = rng.randrange(4)
        if k == 0: new[1] = st.some_user()
        elif k == 1: new[2] = st.ts_text(st.window_ts())
        elif k == 2: new[4] = rng.choice(["view", "cart", "purchase", "View", ""])
        else: new[6] = rng.choice(["100", "0", "", "2500", "x"])
    st.ingests[src[0]].append(ing)
    st.any_file("events").append(new)
    return "copy"

def m_link_add(st):
    rng = st.rng
    person = rng.choice(st.people)
    k = rng.randrange(8)
    if k == 0: a, b = st.user_of(person, rng.randrange(0, 16)), st.user_of(person, rng.randrange(0, 16))
    elif k == 1: a, b = st.user_of(person, rng.randrange(0, 8)), st.user_of(person, rng.randrange(16, 30))
    elif k == 2: a, b = st.user_of(person, rng.randrange(0, 4)), st.user_of(rng.choice(st.people), rng.randrange(0, 4))
    elif k == 3: a, b = st.user_of(person, rng.randrange(0, 4)), "u" + b36((1 << 39) + rng.randrange(1, 4))
    elif k == 4: a, b = st.user_of(person, rng.randrange(0, 4)), st.user_of(person, rng.randrange(0, 4)).upper()
    elif k == 5: a, b = rng.choice(st.users), rng.choice(st.users)
    elif k == 6: a, b = st.user_of(person, 0), st.user_of(person, rng.randrange(8, 16))
    else: a, b = st.some_user(), st.some_user()
    if rng.random() < 0.5: a, b = b, a
    st.any_file("links").append([a, b])
    return "link_add"

def m_link_del(st):
    rows = st.any_file("links")
    if rows: del rows[st.rng.randrange(len(rows))]; return "link_del"

def m_click_user(st):
    rows = st.rows("clicks", 4)
    if not rows: return
    r = st.rng.choice(rows)
    st.any_file("clicks").append([fresh_click(st), st.some_user(), r[2], r[3]])
    return "click_user"

def fresh_click(st):
    rng = st.rng
    while True:
        k = rng.randrange(8)
        if k == 0: cid = "k" + "".join(rng.choice(DIG) for _ in range(rng.randrange(1, 9)))
        elif k == 1: cid = "K" + "".join(rng.choice(DIG) for _ in range(rng.randrange(1, 5)))
        elif k == 2: cid = "k0" + "".join(rng.choice(DIG) for _ in range(rng.randrange(1, 5)))
        elif k == 3: cid = "k" + b36(rng.randrange(MASK + 1, 36 ** 8))
        elif k == 4: cid = "k" + "".join(rng.choice(DIG) for _ in range(rng.randrange(9, 12)))
        elif k == 5: cid = rng.choice(["a", "j", "l", "kz", "k~", "k "]) + str(rng.randrange(1000))
        else: cid = "k" + b36(rng.randrange(0, 36 ** rng.randrange(1, 8)))
        if cid not in st.click_ids:
            st.click_ids.add(cid)
            return cid

def m_click_tie(st):
    """a new click at the same instant and for the same user as an existing one, another campaign"""
    rows = st.rows("clicks", 4)
    if len(rows) < 2: return
    r, other = st.rng.choice(rows), st.rng.choice(rows)
    st.any_file("clicks").append([fresh_click(st), r[1], other[2], r[3]])
    return "click_tie"

def m_click_at_event(st):
    """a click exactly at, just before or just after one of a user's events"""
    ev = st.rng.choice(st.rows("events", 7))
    clicks = st.rows("clicks", 4)
    if not clicks: return
    try:
        ts = ref_impl.parse_ts(ev[2].encode())
    except Exception:
        ts = None
    if ts is None: return
    ts += st.rng.choice([0, 0, -1, 1, -7 * DAY, -7 * DAY - 1, -1000])
    st.any_file("clicks").append([fresh_click(st), ev[1] if st.rng.random() < 0.8 else st.some_user(), st.rng.choice(clicks)[2], str(ts)])
    return "click_at"

MUTATIONS = [m_event_user, m_event_user, m_event_alias, m_event_alias, m_event_ts, m_event_ts, m_event_id, m_copy, m_copy,
             m_link_add, m_link_add, m_link_del, m_click_user, m_click_tie, m_click_tie, m_click_at_event]

def write(st):
    for (folder, name), (header, rows) in st.files.items():
        rng = st.rng
        with open(os.path.join(st.data, folder, name), "wb") as f:
            out = [header]
            for r in rows:
                out.append(",".join('"' + x + '"' if rng.random() < 0.1 else x for x in r))
            f.write(("\n".join(out) + "\n").encode("utf-8"))

def keys_of(binary, data):
    out = tempfile.mkdtemp(prefix="df-keys-")
    p = subprocess.run([binary], env=dict(os.environ, DATA_DIR=data, OUTPUT_DIR=out), capture_output=True)
    shutil.rmtree(out, ignore_errors=True)
    m = re.search(rb"dense keys e ([0-9a-f]+) u ([0-9a-f]+)", p.stderr)
    if not m: sys.exit("the binary did not take the dense path on the base dataset")
    return int(m.group(1), 16), int(m.group(2), 16)

def main():
    base, count, seed, binaries = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4:]
    ekey, ukey = keys_of(binaries[0], base)
    rng = random.Random(seed)
    bad = 0
    stats = {}
    for i in range(count):
        work = tempfile.mkdtemp(prefix="df-")
        data = os.path.join(work, "data")
        shutil.copytree(base, data)
        st = State(data, ekey, ukey, rng)
        done = []
        for _ in range(rng.choice([1, 2, 3, 5, 8, 15, 30, 60])):
            name = rng.choice(MUTATIONS)(st)
            if name: done.append(name)
        write(st)
        try:
            users, campaigns = ref_impl.run(data)
        except Exception as exc:
            print(f"case {i}: reference refused the data ({type(exc).__name__}: {exc}); skipped")
            shutil.rmtree(work, ignore_errors=True)
            continue
        want_u = sorted((u, datetime.date.fromisoformat(d), s, e, r, t or None) for u, d, s, e, r, t in users)
        want_c = sorted((c, ch, datetime.date.fromisoformat(d), s, p2, r, a) for c, ch, d, s, p2, r, a in campaigns)
        key = lambda rows: sorted(tuple("" if v is None else str(v) for v in row) for row in rows)
        failed = False
        for binary in binaries:
            for threads, chunk in (("8", ""), ("3", "997"), ("1", "")):
                out = os.path.join(work, "out-" + os.path.basename(binary) + threads)
                env = dict(os.environ, DATA_DIR=data, OUTPUT_DIR=out, SS_THREADS=threads)
                if chunk: env["SS_CHUNK"] = chunk
                p = subprocess.run([os.path.abspath(binary)], env=env, capture_output=True)
                ok = p.returncode == 0
                if ok:
                    got_u = sorted(load_actual(os.path.join(out, "user_daily"), SCHEMAS["user_daily"]).rows())
                    got_c = sorted(load_actual(os.path.join(out, "campaign_daily"), SCHEMAS["campaign_daily"]).rows())
                    ok = key(got_u) == key(want_u) and key(got_c) == key(want_c)
                how = "abandoned" if b"dense path abandoned" in p.stderr else ("dense" if b"dense |" in p.stderr else "general")
                stats[how] = stats.get(how, 0) + 1
                m = re.search(rb"walked (\d+) split (\d+) sorted (\d+) stitched events (\d+) groups \d+ \| handed events (\d+)", p.stderr)
                if m and how == "dense":
                    for label, value in zip(("walked", "split", "sorted", "stitched", "handed"), m.groups()):
                        stats[label] = stats.get(label, 0) + int(value)
                if not ok:
                    failed = True
                    print(f"FAIL case {i} {binary} threads {threads} chunk {chunk!r} ({how}; {','.join(sorted(set(done)))}): kept at {work}")
                    break
            if failed: break
        if failed: bad += 1
        else: shutil.rmtree(work, ignore_errors=True)
    print(f"{count - bad}/{count} dense-fuzz cases passed; runs by path: {stats}")
    sys.exit(1 if bad else 0)

main()
