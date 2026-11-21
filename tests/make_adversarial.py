#!/usr/bin/env python3
"""
Hand-crafted and mutated datasets the generator never produces but rules.md defines.

  make_adversarial.py                 build all hand-crafted datasets (data/adversarial/<name>)
  make_adversarial.py --fuzz N        also build N mutation-fuzz datasets (data/adversarial/fuzz<k>)
  options: --fuzz-seed S  --jobs J  --force  --only SUBSTR

Expected output (reference) goes to data/expected/adv_<name>/.  A dataset is only
re-run through the reference when its input bytes changed.
Oracle limits respected (checked on disk with the reference's own parser): every
well-formed event line has a parseable ingest_ts, tied min-ingest copies are identical,
duplicate click_ids carry identical values.
"""
import argparse, concurrent.futures as cf, datetime, hashlib, importlib.util, json, os, random, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DATA = os.path.join(ROOT, "data")
REF = os.path.join(ROOT, "reference", "ref_impl.py")
spec = importlib.util.spec_from_file_location("ref_impl", REF)
R = importlib.util.module_from_spec(spec); spec.loader.exec_module(R)

DAY = 86_400_000
GAP = 1_800_000
HEADERS = {"events": "event_id,user_id,ts,ingest_ts,event_type,product_id,price_cents",
           "clicks": "click_id,user_id,campaign_id,ts", "links": "user_id_a,user_id_b"}
# A literal copy of the events header as a data row has ingest_ts "ingest_ts", which the reference
# rejects (ValueError), so every events header-copy row below keeps the header text but a valid ingest_ts.
HDR_EV = "event_id,user_id,ts,2026-10-01T00:00:00Z,event_type,product_id,price_cents"
WIDTH = {"events": 7, "clicks": 4, "links": 2}
EPOCH = datetime.datetime(1970, 1, 1, tzinfo=datetime.timezone.utc)


# ------------------------------------------------------------------ time text
def ms_of(iso):  # '2026-10-01T00:00:00Z' -> ms
    d = datetime.datetime.strptime(iso, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=datetime.timezone.utc)
    return int((d - EPOCH).total_seconds()) * 1000

def civil(ms):
    d = EPOCH + datetime.timedelta(milliseconds=ms)
    return d.strftime("%Y-%m-%dT%H:%M:%S")

def F1(ms):
    return civil(ms - ms % 1000) + "Z"

def F2(ms, off):  # off like '+02:00'
    sign = 1 if off[0] == "+" else -1
    mins = sign * (int(off[1:3]) * 60 + int(off[4:6]))
    return civil(ms - ms % 1000 + mins * 60000) + off

def F3(ms, zeros=0):
    return "0" * zeros + str(ms)

def ts_text(ms):
    return F1(ms) if ms % 1000 == 0 else F3(ms)


# ------------------------------------------------------------------ dataset
DEF_WIN = ("2026-10-01T00:00:00Z", "2026-10-15T00:00:00Z")
WIN_LEAP = ("2028-02-27T00:00:00Z", "2028-03-02T00:00:00Z")
WIN_NONLEAP = ("2027-02-27T00:00:00Z", "2027-03-02T00:00:00Z")
WIN_ODD = ("2026-10-01T06:30:00Z", "2026-10-04T18:00:00Z")
WIN_LONG = ("2026-11-10T00:00:00Z", "2026-11-30T00:00:00Z")
WINDOWS = {"leap": WIN_LEAP, "nonleap": WIN_NONLEAP, "odd": WIN_ODD, "long": WIN_LONG, "def": DEF_WIN}

CATS = ["shoes", "books", "garden", "toys", "apparel", "books", "shoes", "zebra", "apparel"]
PRODUCTS = [f"p{i+1},{c}" for i, c in enumerate(CATS)]
CHANNELS = ["search", "social", "email", "display", "affiliate"]
CAMPAIGNS = [f"c_{i},{CHANNELS[i % 5]}" for i in range(1, 10)]


class DS:
    def __init__(self, name, desc, win=DEF_WIN):
        self.name, self.desc, self.win = name, desc, win
        self.ws, self.we = ms_of(win[0]), ms_of(win[1])
        self.files = {k: {} for k in HEADERS}      # folder -> name -> list[str] | None (zero byte)
        self.products = list(PRODUCTS)
        self.campaigns = list(CAMPAIGNS)
        self.crlf = set()                           # (folder,name) or ("*","*")
        self.notrail = set()
        self.manifest_text = None

    # --- file handling
    def fname(self, f):
        return f if isinstance(f, str) else f"part-{f:05d}.csv"

    def lines(self, folder, f):
        return self.files[folder].setdefault(self.fname(f), [])

    def raw(self, folder, f, text):
        lst = self.lines(folder, f)
        if lst is None:
            raise ValueError("zero-byte file")
        lst.append(text)

    def row(self, folder, f, fields, quote=None, rng=None):
        fields = [str(x) for x in fields]
        if quote == "all":
            fields = [f'"{x}"' for x in fields]
        elif isinstance(quote, (set, frozenset, list, tuple)):
            fields = [f'"{x}"' if i in quote else x for i, x in enumerate(fields)]
        elif isinstance(quote, float):
            fields = [f'"{x}"' if rng.random() < quote else x for x in fields]
        self.raw(folder, f, ",".join(fields))

    def ev(self, eid, uid, ts, ing=None, typ="view", pid="p1", price="", f=0, quote=None, rng=None):
        if isinstance(ts, int):
            if ing is None:
                ing = ts - ts % 1000 + 5000
            ts = ts_text(ts)
        elif ing is None:
            ing = self.ws + 43_200_000
        ingt = ing if isinstance(ing, str) else F1(ing)
        self.row("events", f, [eid, uid, ts, ingt, typ, pid, price], quote, rng)

    def ck(self, cid, uid, camp, ts, f=0, quote=None, rng=None):
        if isinstance(ts, int):
            ts = ts_text(ts)
        self.row("clicks", f, [cid, uid, camp, ts], quote, rng)

    def lk(self, a, b, f=0, quote=None, rng=None):
        self.row("links", f, [a, b], quote, rng)

    # --- output
    def write(self, root):
        if os.path.exists(root):
            shutil.rmtree(root)
        os.makedirs(root)
        manifest = self.manifest_text or json.dumps({"window_start": self.win[0], "window_end": self.win[1]}, indent=2) + "\n"
        with open(os.path.join(root, "manifest.json"), "w") as h:
            h.write(manifest)
        with open(os.path.join(root, "products.csv"), "wb") as h:
            h.write(("product_id,category\n" + "".join(l + "\n" for l in self.products)).encode())
        with open(os.path.join(root, "campaigns.csv"), "wb") as h:
            h.write(("campaign_id,channel\n" + "".join(l + "\n" for l in self.campaigns)).encode())
        for folder, header in HEADERS.items():
            os.makedirs(os.path.join(root, folder))
            if not self.files[folder]:
                self.files[folder]["part-00000.csv"] = []
            for name, lines in self.files[folder].items():
                path = os.path.join(root, folder, name)
                if lines is None:
                    open(path, "wb").close()
                    continue
                key = (folder, name)
                eol = "\r\n" if (key in self.crlf or ("*", "*") in self.crlf or (folder, "*") in self.crlf) else "\n"
                body = eol.join([header] + lines)
                if not (key in self.notrail or ("*", "*") in self.notrail or (folder, "*") in self.notrail):
                    body += eol
                with open(path, "wb") as h:
                    h.write(body.encode("utf-8"))

    @staticmethod
    def load(path, name, desc):
        win = json.load(open(os.path.join(path, "manifest.json")))
        ds = DS(name, desc, (win["window_start"], win["window_end"]))
        ds.products = [l for l in open(os.path.join(path, "products.csv")).read().split("\n")[1:] if l]
        ds.campaigns = [l for l in open(os.path.join(path, "campaigns.csv")).read().split("\n")[1:] if l]
        for folder in HEADERS:
            for fn in sorted(os.listdir(os.path.join(path, folder))):
                text = open(os.path.join(path, folder, fn), "rb").read().decode("utf-8")
                parts = text.split("\n")
                if parts and parts[-1] == "":
                    parts.pop()
                ds.files[folder][fn] = parts[1:] if parts else []
        return ds


# ------------------------------------------------------------------ random world
def rand_world(rng, name, desc, win=DEF_WIN, people=10, sessions=45, nfiles=(3, 2, 2), header_only=True):
    ds = DS(name, desc, win)
    ws, we = ds.ws, ds.we
    ids = {}
    for p in range(people):
        k = rng.choice([1, 1, 2, 3, 4])
        ids[p] = [f"u{p}_{j}" if rng.random() < .8 else f"U{p}_{j}" for j in range(k)]
        for j in range(1, k):
            a, b = ids[p][rng.randrange(j)], ids[p][j]
            ds.lk(*(a, b) if rng.random() < .5 else (b, a), f=rng.randrange(nfiles[2]))
        if rng.random() < .15:
            ds.lk(ids[p][0], ids[p][0], f=rng.randrange(nfiles[2]))
    n_ev = n_ck = 0
    gaps = [1000, 5000, 60_000, 600_000, GAP, GAP, GAP + 1, GAP - 1, 3 * 3_600_000, 0]
    for _ in range(sessions):
        p = rng.randrange(people)
        start = rng.randrange(ws, we - 3_600_000)
        if rng.random() < .15:  # cross midnight
            start = (start // DAY + 1) * DAY - rng.choice([0, 1, 60_000, 600_000]) - 1000
            start = max(ws, min(start, we - 3_600_000))
        t = start
        for i in range(rng.randint(1, 6)):
            if i:
                t += rng.choice(gaps)
            typ = rng.choice(["view", "view", "view", "cart", "purchase"])
            pid = rng.choice([f"p{rng.randint(1, 9)}"] * 5 + ["p77", "P1"])
            price = rng.choice([str(rng.randint(1, 5000)), "0250", "19.99", "", "10000000", "10000001"]) if typ == "purchase" else rng.choice(["", "-5", "x"])
            if typ == "purchase" and rng.random() < .8:
                price = str(rng.randint(100, 50000))
            tt = t if rng.random() < .6 else (t - t % 1000)
            eid = f"e{n_ev}"; n_ev += 1
            ing = tt - tt % 1000 + rng.choice([1000, 60_000, 600_000, 600_000, DAY, DAY + 1000, 3 * DAY])
            u = rng.choice(ids[p])
            quote = .2
            f = rng.randrange(nfiles[0])
            ds.ev(eid, u, tt, ing, typ, pid, price, f=f, quote=quote, rng=rng)
            if rng.random() < .1:  # exact duplicate or later copy
                if rng.random() < .5:
                    ds.ev(eid, u, tt, ing, typ, pid, price, f=rng.randrange(nfiles[0]), quote=quote, rng=rng)
                else:
                    ds.ev(eid, rng.choice(ids[rng.randrange(people)]), tt, ing + rng.randint(1, 9) * 1000, typ, pid, price, f=rng.randrange(nfiles[0]))
            t = tt
        if rng.random() < .65:
            when = start - rng.choice([0, 1, 1000, 3_600_000, 7 * DAY, 7 * DAY + 1, 7 * DAY - 1, rng.randrange(7 * DAY), rng.randrange(7 * DAY)])
            if rng.random() < .1:
                when = start + 1
            cid = f"k{n_ck}"; n_ck += 1
            camp = rng.choice([f"c_{rng.randint(1, 9)}"] * 6 + ["c_99", "C_1"])
            cuid = rng.choice(ids[p])
            ds.ck(cid, cuid, camp, when, f=rng.randrange(nfiles[1]), quote=.2, rng=rng)
            if rng.random() < .1:
                ds.ck(cid, cuid, camp, when, f=rng.randrange(nfiles[1]), quote=.3, rng=rng)
    if header_only:
        ds.lines("events", "part-header-only.csv"); ds.lines("clicks", "part-header-only.csv"); ds.lines("links", "part-header-only.csv")
    return ds


# ------------------------------------------------------------------ scenarios
SCEN = []
def scenario(fn):
    SCEN.append(fn); return fn

def rng_for(name):
    return random.Random(int(hashlib.md5(name.encode()).hexdigest()[:8], 16))

@scenario
def world_variants():
    out = []
    for key, win in WINDOWS.items():
        rng = rng_for("world_" + key)
        out.append(rand_world(rng, f"world_{key}", f"random mini world, window '{key}' {win[0]}..{win[1]}: dups, odd prices, gap-boundary sessions, 7-day click edges", win,
                              people=12, sessions=60))
    return out

@scenario
def crlf():
    out = []
    ds = rand_world(rng_for("crlf"), "crlf_all", "every events/clicks/links file converted to CRLF (\\r is part of the last field: prices/ts/link sides get a \\r)")
    ds.crlf = {("*", "*")}
    out.append(ds)
    ds = rand_world(rng_for("crlf2"), "crlf_events_links", "events and links files CRLF, clicks LF (duplicate rows stay identical); CRLF links make the b-side carry \\r")
    ds.crlf = {("events", "*"), ("links", "*")}
    out.append(ds)
    ds = DS("crlf_semantics", "CRLF details: purchase price '1000\\r' is bad, view with '\\r' price is fine, click ts with \\r bad, link side with \\r is a distinct ID, event_id 'e3' vs 'e3\\r' distinct")
    ds.crlf = {("*", "*")}
    t = ds.ws + 10 * 3600_000
    ds.ev("e1", "u1", t, typ="view", price="")                      # last field is price -> '\r' ignored for view
    ds.ev("e2", "u1", t + 1000, typ="purchase", price="1000")        # price '1000\r' -> dropped
    ds.ev("e3", "u1", t + 2000, typ="view")
    ds.ev("e3\r", "u2", t + 5000, typ="view")
    ds.ck("k1", "u1", "c_1", F1(t - 3600_000) + "\r")                # bad: ts has \r
    ds.ck("k2", "u2", "c_2", t - 3600_000)                           # ts is last field -> has \r -> bad too
    ds.lk("u1", "u3"); ds.lk("u4", "u1")                             # sides 'u3\r' and 'u1\r'?? b-side only
    ds.ev("e4", "u3", t + 9000, typ="view"); ds.ev("e5", "u3\r", t + 10000, typ="view")
    out.append(ds)
    return out

@scenario
def trailing_newline():
    out = []
    ds = rand_world(rng_for("nt"), "notrail_all", "every file's last line has no trailing newline (incl. header-only files)")
    ds.notrail = {("*", "*")}
    out.append(ds)
    ds = rand_world(rng_for("nt2"), "notrail_mixed", "no-trailing-newline on a random half of files, plus CRLF on whole events/links folders at random")
    r = rng_for("nt2x")
    for folder in HEADERS:
        for fn in ds.files[folder]:
            if r.random() < .5: ds.notrail.add((folder, fn))
        if folder != "clicks" and r.random() < .5: ds.crlf.add((folder, "*"))
    out.append(ds)
    ds = DS("notrail_edge", "files whose final line is a valid row, a malformed row, a header row; chunk-boundary stress when SS_CHUNK is tiny")
    t = ds.ws + 3600_000
    ds.ev("e1", "u1", t); ds.ev("e2", "u1", t + 1000)
    ds.raw("events", 1, "e3,u1,"+F1(t+2000)); ds.notrail.add(("events", ds.fname(1)))      # malformed last line, no \n
    ds.ev("e4", "u2", t, f=2); ds.notrail.add(("events", ds.fname(2)))
    ds.raw("events", 3, HDR_EV); ds.notrail.add(("events", ds.fname(3)))          # header copy last
    ds.ck("k1", "u1", "c_1", t - 1000, f=0); ds.notrail.add(("clicks", ds.fname(0)))
    ds.lk("u1", "u9", f=0); ds.notrail.add(("links", ds.fname(0)))
    ds.lines("links", 1); ds.notrail.add(("links", ds.fname(1)))
    out.append(ds)
    return out

@scenario
def empty_files():
    ds = rand_world(rng_for("empty"), "empty_files", "zero-byte files and header-only files mixed among real ones in every folder; a file with header + a blank line")
    for folder in HEADERS:
        ds.files[folder]["part-zero-a.csv"] = None
        ds.files[folder]["part-zero-b.csv"] = None
        ds.files[folder]["part-zero-c.csv"] = None
        ds.files[folder]["part-hdr-a.csv"] = []
        ds.files[folder]["part-hdr-b.csv"] = []
        ds.files[folder]["part-hdr-blank.csv"] = [""]
    ds.notrail.add(("events", "part-hdr-a.csv")); ds.crlf.add(("links", "part-hdr-b.csv"))
    ds2 = DS("all_empty_inputs", "every events/clicks/links file is zero bytes or header only: output has no rows (empty Parquet tables)")
    for folder in HEADERS:
        ds2.files[folder] = {"a.csv": None, "b.csv": [], "c.csv": None}
    ds3 = DS("only_links_and_clicks", "no event rows at all (events header only), clicks and links present: no sessions")
    ds3.ck("k1", "u1", "c_1", ds3.ws + 1000); ds3.lk("u1", "u2")
    ds3.files["events"] = {"a.csv": []}
    ds4 = DS("only_events_no_links_no_clicks", "events only; links/clicks files zero byte")
    t = ds4.ws + 3600_000
    for i in range(5): ds4.ev(f"e{i}", "u1", t + i * 1000)
    ds4.files["links"] = {"a.csv": None}; ds4.files["clicks"] = {"a.csv": None}
    return [ds, ds2, ds3, ds4]

@scenario
def long_lines():
    ds = DS("long_lines", "300 KB malformed lines (events/clicks/links, incl. last-line-no-newline), valid lines with 2000-byte user/event/click ids and link sides, 300KB line made of quotes")
    t = ds.ws + 10 * 3600_000
    big = "x" * 300_000
    ds.raw("events", 0, "e_bad," + big)                                   # 2 fields -> malformed
    ds.ev("e1", "u1", t, f=0)
    ds.raw("events", 0, "e_bad2," + ",".join(["y" * 30] * 8000))           # thousands of fields
    ds.ev("e2", "U" * 2000, t + 1000, f=0, typ="purchase", price="500", pid="p2")
    ds.ev("E" * 2000, "u1", t + 2000, f=0)
    ds.ev("e4", "U" * 2000, t + 3000, f=1)
    ds.ev("e5", "u" * 1999 + "w", t + 4000, f=1)
    ds.raw("events", 1, '"' * 300_000)
    ds.raw("events", 2, "e_bad3,u1,2026-10-01T10:00:00Z,2026-10-01T10:00:01Z,view,p1," + big)  # 7 fields? price long -> valid view
    ds.ev("e6", "u1", t + 5000, f=2)
    ds.raw("events", 2, "e_bad4," + big)
    ds.notrail.add(("events", ds.fname(2)))
    ds.raw("clicks", 0, "k_bad," + big + ",c_1")
    ds.ck("k1", "U" * 2000, "c_1", t - 1000)
    ds.ck("K" * 2000, "u1", "c_2", t - 2000)
    ds.ck("k3", "u" * 1999 + "w", "c_3", t)
    ds.raw("clicks", 1, "k_bad2," + big)
    ds.notrail.add(("clicks", ds.fname(1)))
    ds.lk("U" * 2000, "u1")
    ds.raw("links", 0, big)
    ds.lk("a", "b" * 2000)
    ds.lk("u" * 1999 + "w", "u" * 1999 + "v")
    ds.raw("links", 1, "a," + big + ",c")
    ds.raw("links", 1, big)
    ds.notrail.add(("links", ds.fname(1)))
    return [ds]

ODD = ["x" * 12, "ünï", "用户", "é", "e", "😀", "￮", "a\tb", " lead", "trail ", "A", "a", "U1", "u1", "u10", "u9",
       "~x", "~", "0", "z", "abcdefghi", "abcdefghij", "abcdefghijk", "\x7f", "Ω", "ab", "ab̀", " ", "\t", "uu", "u"]

@scenario
def odd_ids():
    ds = DS("odd_ids", "event/user/click/link IDs: >9 bytes, non-ASCII, tab, leading/trailing space, case-only differences, 1 char; links join odd IDs with ordinary ones so the odd one is sometimes the byte-wise smallest, sometimes not")
    base = ds.ws + 24 * 3600_000
    groups = [
        ["u9", "u10", "u1", "~x"],                      # u1 < u10 < u9 < ~x
        ["~x2", "A", "zz"],                              # A smallest (0x41)
        ["abcdefghi", "abcdefghij", "abcdefghijk"],       # prefix 9 vs 10 vs 11
        ["\U0001F600", "￮", "～"],                # byte order EF BF AE < F0.. ; UTF-16 order differs
        ["ünï", "unï", "uni", "é", "e"],
        ["a\tb", "a b", " a", "a ", "a"],
        ["U1", "u1x", "u1 "],                             # U1 linked w/ others (U1 is smallest)
        ["x" * 12, "x" * 11, "x" * 10, "x" * 9],
        ["用户", "用户1", "\x7f"],
        ["0", "~", "z"],
        [" ", "\t", "ab", "ab̀"],
        ["uu", "u"],
        ["Q", "q", "Q "],                                 # linked to nothing: separate people
    ]
    rng = rng_for("odd_ids")
    for gi, g in enumerate(groups):
        t = base + gi * 3 * DAY // 4 // 10 * 10
        order = g[:]
        if gi != len(groups) - 1:
            rng.shuffle(order)
            for a, b in zip(order, order[1:]):
                ds.lk(a, b, f=rng.randrange(3)) if rng.random() < .5 else ds.lk(b, a, f=rng.randrange(3))
        t = base + gi * 3_600_000 * 5
        for j, u in enumerate(g):
            ds.ev(f"ev{gi}_{j}", u, t + j * 120_000, typ=["view", "cart", "purchase"][j % 3], pid=f"p{1 + (j * 3 + gi) % 9}", price=str(100 * (j + 1)), f=rng.randrange(3))
            ds.ck(f"ck{gi}_{j}", u, f"c_{1 + (gi + j) % 9}", t - 3_600_000 - j * 1000, f=rng.randrange(3))
    # odd event ids with dup copies (dedup keyed on odd ids)
    t = base + 20 * 3_600_000
    for j, eid in enumerate(ODD):
        ds.ev(eid, "dupuser", t + j * 1000, ing=t + j * 1000 + 1000 * (j + 2) // 1 // 1000 * 1000 if False else None, f=0)
        ds.ev(eid, "dupuser2", t + j * 1000, ing=(t // 1000) * 1000 + 600_000, f=1)
    # odd click ids at the same ms for different campaigns -> byte-wise tie-break on odd ids
    t2 = base + 40 * 3_600_000
    cids = ["k9", "k10", "K9", "k1", "~k", "é", "k\t", " k", "k ", "kk", "k" * 12]
    for j, cid in enumerate(cids):
        ds.ck(cid, "tieuser", f"c_{1 + j % 9}", t2 - 3600_000)
    ds.ev("tie1", "tieuser", t2, f=0)
    # linked-but-unused smallest ID names a person
    ds.lk("!lonely", "zlive"); ds.ev("lv1", "zlive", t2 + 5000)
    ds.lk(" ", "spaceuser"); ds.ev("lv2", "spaceuser", t2 + 6000)
    return [ds]

@scenario
def nul_byte():
    ds = DS("nul_byte", "IDs containing a NUL byte next to the same ID without it (C-string trap); also DEL byte")
    t = ds.ws + 3600_000 * 5
    for i, u in enumerate(["ab\x00cd", "ab", "ab\x00", "ab\x00cd"]):
        ds.ev(f"e{i}", u, t + i * 1000)
    ds.lk("ab\x00", "zz")
    return [ds]

@scenario
def link_traps():
    ds = DS("link_traps", "link edge cases: self links, empty sides, repeated/reversed, chain of 300 in shuffled order across files, case-variant sides, unused smallest, long cycle, one-field/three-field link lines, header-identical link row")
    rng = rng_for("link_traps")
    t = ds.ws + 3600_000 * 5
    n = 300
    chain = [f"ch{i:03d}" for i in range(n)]
    pairs = list(zip(chain, chain[1:]))
    rng.shuffle(pairs)
    for a, b in pairs:
        ds.lk(a, b, f=rng.randrange(4))
    for i in range(0, n, 3):
        ds.ev(f"ce{i}", chain[rng.randrange(n)], t + i * 60_000, f=rng.randrange(3))
    ds.lk("ch000", "ch299"); ds.lk("ch299", "ch000"); ds.lk("ch007", "ch007")
    ds.lk("", "ch010"); ds.lk("ch011", ""); ds.lk("", "")
    ds.raw("links", 0, "ch012"); ds.raw("links", 0, "ch012,ch013,ch014"); ds.raw("links", 0, "ch012,ch013,"); ds.raw("links", 0, "")
    ds.raw("links", 0, HEADERS["links"])        # a valid link: user_id_a <-> user_id_b
    ds.ev("hx1", "user_id_a", t, f=0); ds.ev("hx2", "user_id_b", t + 60_000, f=1)
    ds.lk("CH050", "ch051")                      # distinct upper-case ID, links 'ch051' chain with CH050 (smaller)
    ds.ev("hx3", "CH050", t + 3 * 3600_000)
    ds.lk('"quoted"', "ch020"); ds.lk('"ch021"', "ch022")  # first malformed (quote inside), second unquotes to ch021
    ds.lk("star", "star")                         # self link only
    ds.ev("sx", "star", t + 6 * 3600_000)
    ds.lk("Zlast", "aaa-unused"); ds.lk("aaa-unused", "zzlive"); ds.ev("zl", "zzlive", t + 7 * 3600_000)
    return [ds]

@scenario
def products_cases():
    ds = DS("products_odd", "products.csv: quoted fields, >20 categories, categories sorting before and after 'unknown', non-canonical product ids p007/P5/x1/p 5 next to p7/p5, ties incl. unknown, unicode/space categories")
    rng = rng_for("products")
    ds.products = ['"p1","shoes"', 'p5,apparel', '"p7",books', "p007,garden", "x1,zzz_last", "p 5,toys", "P5,aaa_first",
                   "p9,Unknown", "p10,unknow", "p11,unknown2", "p12,éclair", "p13,with space", " p14,lead", "p15,UPPER", "p16,upper"]
    for i in range(17, 40):
        ds.products.append(f"p{i},cat{i:02d}")
    ds.products += ["p40,zzzz", "p41,~tilde", "p42,用户"]
    t = ds.ws + 5 * 3600_000
    k = 0
    def purchase(user, pid, price, dt):
        nonlocal k
        ds.ev(f"e{k}", user, t + dt, typ="purchase", pid=pid, price=str(price), f=k % 3, quote=.3, rng=rng); k += 1
    cases = [
        ("a", [("p5", 100), ("p007", 100)]),                 # tie apparel vs garden -> apparel
        ("b", [("P5", 100), ("p5", 100)]),                   # aaa_first vs apparel
        ("c", [("x1", 500), ("pXX", 500)]),                  # zzz_last vs unknown -> unknown
        ("d", [("pXX", 500), ("p1", 500)]),                  # shoes vs unknown -> shoes
        ("e", [("p 5", 100), ("p5", 99)]),
        ("f", [("p9", 10), ("pXX", 10)]),                    # 'Unknown' vs 'unknown'
        ("g", [("p10", 10), ("p11", 10), ("pXX", 10)]),
        ("h", [("p12", 7), ("p13", 7), (" p14", 7), ("p15", 7), ("p16", 7), ("p41", 7), ("p42", 7), ("p40", 7)]),
        ("i", [("p%d" % i, 1) for i in range(17, 40)]),
        ("j", [("p7", 300), ("p7", 400), ("p5", 650), ("p5", 51)]),
        ("k", [("P7", 100)]),
        ("l", [("", 100), ("p1", 99)]),
    ]
    for gi, (user, ps) in enumerate(cases):
        for j, (pid, price) in enumerate(ps):
            purchase(user, pid, price, gi * 5 * 3_600_000 + j * 1000)
        ds.ev(f"v{gi}", user, t + gi * 5 * 3_600_000 + 500, typ="view", pid="p1")
    ds2 = DS("products_dup_whitespace", "products.csv with unusual but clean rows: quoted ids and categories, trailing spaces inside categories, category equal to a product id")
    ds2.products = ['"p1","a b"', '"p2","a b "', 'p3,p1', '"p4","unknownish"', "p5,über"]
    for i in range(1, 7):
        ds2.ev(f"e{i}", "u1", ds2.ws + 3600_000 * 5 + i * 100000 * 1000 // 1000 * i, typ="purchase", pid=f"p{i}", price=str(i * 100), f=i % 2)
        ds2.ev(f"w{i}", f"w{i}", ds2.ws + 3600_000 * 5, typ="purchase", pid=f"p{i}", price="100")
    return [ds, ds2]

@scenario
def campaigns_cases():
    ds = DS("campaigns_odd", "campaign ids c_007/camp1/C_1 next to c_7/c_1, clicks use all of them plus unknown case variants; the 'organic' id given as a click campaign (not in campaigns.csv)")
    ds.campaigns = ['c_1,search', 'c_7,social', 'c_007,email', 'camp1,display', 'C_1,affiliate', '"c_9","search"', "c_10,social", "c_1 ,email", " c_2,email"]
    t = ds.ws + 24 * 3600_000
    camps = ["c_1", "c_7", "c_007", "camp1", "C_1", "c_9", "c_10", "c_1 ", " c_2", "c_2", "organic", "ORGANIC", "c_01", "c_", ""]
    for i, c in enumerate(camps):
        u = f"cu{i}"
        ds.ev(f"e{i}", u, t + i * 3600_000)
        ds.ck(f"kc{i}", u, c, t + i * 3600_000 - 1000, f=i % 2)
    # one person with all campaign variants at distinct times -> assists
    for i, c in enumerate(camps):
        ds.ck(f"m{i}", "multi", c, t - 2 * DAY + i * 60_000)
    ds.ev("em", "multi", t + 10 * 3600_000 * 0 + DAY)
    # same-ms ties: different campaigns, k9/k10/K9
    for cid, camp in [("k9", "c_1"), ("k10", "c_7"), ("K9", "camp1")]:
        ds.ck(cid, "tie", camp, t - 5000)
    ds.ev("et", "tie", t)
    return [ds]

@scenario
def price_cases():
    ds = DS("prices", "purchase price edge cases: leading zeros, 10,000,000, 10,000,001, 00, 0, 1, spaces, signs, decimals, huge, non-ASCII digits; view/cart prices ignored")
    t = ds.ws + 5 * 3600_000
    prices = ["000000000000000000250", "10000000", "10000001", "00", "0", "1", " 5", "5 ", "+5", "-5", "5.0", "1e3", "٢٥٠", "２５０", "99999999999999999999999999",
              "0000000000000000000000000000000000000000010000000", "0000000000000000000000000000000000000000010000001", "", "0x10", "1_0", "\t7", "9999999", "007"]
    for i, p in enumerate(prices):
        ds.ev(f"p{i}", f"pu{i}", t + i * 1000, typ="purchase", pid="p2" if i % 2 else "p1", price=p, f=i % 3, quote=.2, rng=rng_for("pr"))
        ds.ev(f"v{i}", f"vu{i}", t + i * 1000, typ="view", pid="p1", price=p)
        ds.ev(f"c{i}", f"cu{i}", t + i * 1000, typ="cart", pid="p1", price=p)
    # one session with several priced purchases for the revenue sum / top category
    for i, p in enumerate(["10000000", "10000000", "1", "000000250"]):
        ds.ev(f"s{i}", "sum", t + i * 1000, typ="purchase", pid="p1" if i < 2 else "p2", price=p)
    return [ds]

def ts_cases(ds):
    """(label, text) pairs for event timestamps, relative to the dataset window."""
    ws, we = ds.ws, ds.we
    y = ds.win[0][:4]; mid = ws + (we - ws) // 2
    cs = [("ws", F1(ws)), ("we-1s", F1(we - 1000)), ("we", F1(we)), ("ws-1s", F1(ws - 1000)),
          ("F3 ws", F3(ws)), ("F3 we-1", F3(we - 1)), ("F3 we", F3(we)), ("F3 ws-1", F3(ws - 1)), ("F3 ws+1", F3(ws + 1)),
          ("F3 30 zeros", F3(mid, 30)), ("F3 40 digits", "1" * 40), ("F3 40 zeros+0", "0" * 40), ("F3 0", "0"), ("F3 86400000", "86400000"),
          ("F3 19 digits", "9" * 19), ("F3 20 digits", "9" * 20), ("F3 neg", "-5"), ("F3 float", F3(mid) + ".0"), ("F3 exp", "1.79e12"), ("F3 plus", "+" + F3(mid)),
          ("F3 space", F3(mid) + " "), ("F3 space lead", " " + F3(mid)),
          ("F2 +23:59", F2(mid, "+23:59")), ("F2 -23:59", F2(mid, "-23:59")), ("F2 -00:00", F2(mid, "-00:00")), ("F2 +00:00", F2(mid, "+00:00")),
          ("F2 +24:00", F2(mid, "+23:00")[:-6] + "+24:00"), ("F2 +0200", F1(mid)[:-1] + "+0200"), ("F2 +02:60", F1(mid)[:-1] + "+02:60"),
          ("F2 +02:59", F2(mid, "+02:59")), ("F2 -02:59", F2(mid, "-02:59")), ("F2 +05:60", F1(mid)[:-1] + "+05:60"), ("F2 +2:00", F1(mid)[:-1] + "+2:00"),
          ("F2 +02:0", F1(mid)[:-1] + "+02:0"), ("F2 +02 00", F1(mid)[:-1] + "+02 00"), ("F2 ws via +05:30", F2(ws, "+05:30")), ("F2 ws via -10:00", F2(ws, "-10:00")),
          ("F2 we via +12:00", F2(we, "+12:00")), ("F2 we-1s via +12:00", F2(we - 1000, "+12:00")), ("F2 ws-1s via +12:00", F2(ws - 1000, "+12:00")),
          ("F2 date change", F2(ws + 1000, "+14:00".replace("14", "13") if False else "+12:00")),
          ("F2 lower t", F2(mid, "+02:00").replace("T", "t")), ("F2 Z+ ", F1(mid)[:-1] + "Z+00:00"), ("F2 'z'", F1(mid)[:-1] + "z"),
          ("lower t", F1(mid).replace("T", "t")), ("lower z", F1(mid)[:-1] + "z"), ("space sep", F1(mid).replace("T", " ")),
          ("frac", F1(mid)[:-1] + ".5Z"), ("frac3", F1(mid)[:-1] + ".000Z"), ("frac F2", F2(mid, "+01:00")[:-6] + ".500+01:00"),
          ("no zone", F1(mid)[:-1]), ("empty", ""), ("space", " "), ("only Z", "Z"), ("T only", "T"),
          ("F1 trailing space", F1(mid) + " "), ("F1 leading space", " " + F1(mid)), ("F1 2 Z", F1(mid) + "Z"), ("F1 date only", F1(mid)[:10]),
          ("feb29 2028", "2028-02-29T12:00:00Z"), ("feb30 2028", "2028-02-30T12:00:00Z"), ("feb29 2027", "2027-02-29T12:00:00Z"), ("feb28 2027", "2027-02-28T12:00:00Z"),
          ("feb29 2024", "2024-02-29T12:00:00Z"), ("feb29 2100", "2100-02-29T12:00:00Z"), ("feb29 2000", "2000-02-29T12:00:00Z"), ("mar1 2028", "2028-03-01T00:00:00Z"),
          ("feb29 F2 2028", "2028-02-29T23:30:00-01:00"), ("feb28 F2 2027 -> mar1", "2027-02-28T23:30:00-01:00"), ("feb29 F2 2027", "2027-02-29T01:00:00+02:00"),
          ("month 00", y + "-00-10T10:00:00Z"), ("month 13", y + "-13-10T10:00:00Z"), ("day 00", y + "-10-00T10:00:00Z"), ("day 32", y + "-10-32T10:00:00Z"),
          ("apr 31", y + "-04-31T10:00:00Z"), ("hour 24", F1(mid)[:11] + "24:00:00Z"), ("minute 60", F1(mid)[:14] + "60:00Z"), ("second 60", F1(mid)[:17] + "60Z"),
          ("hour 25", F1(mid)[:11] + "25:00:00Z"), ("2-digit year", F1(mid)[2:]), ("5-digit year", "1" + F1(mid)),
          ("arabic F3", "٢٥٠"), ("arabic digit in F1", F1(mid)[:3] + "٢" + F1(mid)[4:]), ("fullwidth F1", F1(mid)[:3] + "２" + F1(mid)[4:]),
          ("F1 slash", F1(mid).replace("-", "/")), ("F1 no seconds", F1(mid)[:16] + "Z"), ("F1 hyphen sign", "-" + F1(mid)),
          ("F2 short", F1(mid)[:-1] + "+02:0X"), ("F2 letters", F1(mid)[:-1] + "+AB:CD"), ("F2 hour 24 -", F1(mid)[:-1] + "-24:00"),
          ("F2 neg ws edge", F2(ws, "-23:59")), ("F2 pos we edge", F2(we - 1000, "+23:59")),
          ("year 0001", "0001-01-01T00:00:00Z"), ("year 9999", "9999-12-31T23:59:59Z"), ("year 0000", "0000-01-01T00:00:00Z"),
          ("midnight ws+DAY", F1(ws + DAY)), ("midnight-1s", F1(((ws // DAY) + 1) * DAY - 1000)), ("midnight F3-1", F3(((ws // DAY) + 1) * DAY - 1)),
          ]
    return cs

@scenario
def ts_datasets():
    out = []
    for key in ("leap", "nonleap", "odd", "long", "def"):
        ds = DS(f"ts_{key}", f"timestamp forms for events and clicks (valid/invalid F1/F2/F3 incl. window edges, calendar edges) in window '{key}' {WINDOWS[key][0]}..{WINDOWS[key][1]}", WINDOWS[key])
        ws, we = ds.ws, ds.we
        rng = rng_for("ts" + key)
        for i, (label, text) in enumerate(ts_cases(ds)):
            u = f"t{i:03d}"
            ds.ev(f"e{i:03d}", u, text, ing=ws + 12 * 3600_000 + (i % 7) * 1000, typ="view", f=i % 3, quote=.15, rng=rng)
            # second event for same user in a known-valid slot (so valid/invalid first event changes session count)
            ds.ev(f"f{i:03d}", u, ws + 12 * 3600_000 + (i % 5) * 60_000, typ="purchase", price="100", pid="p1", f=i % 2)
            # click with the same ts text; probe session at ws and at ws+12h
            cu = f"c{i:03d}"
            ds.ck(f"k{i:03d}", cu, f"c_{1 + i % 9}", text, f=i % 2, quote=.15, rng=rng)
            ds.ev(f"g{i:03d}", cu, ws, typ="view", f=i % 3)                          # session at the first instant
            ds.ev(f"h{i:03d}", cu, ws + 12 * 3600_000, typ="view", f=i % 3)          # session mid
            ds.ev(f"j{i:03d}", cu, we - 1, typ="view", f=i % 3)                      # session at the last ms
        # click window edges exactly, with sessions on edges
        edge = [("ws-7d", ws - 7 * DAY), ("ws-7d-1ms", ws - 7 * DAY - 1), ("ws-7d+1ms", ws - 7 * DAY + 1), ("we-1ms", we - 1), ("we", we), ("ws", ws), ("ws-1", ws - 1)]
        for i, (label, ms) in enumerate(edge):
            u = f"edge{i}"
            ds.ck(f"ek{i}", u, "c_1", ms)
            ds.ck(f"ek{i}b", u, "c_2", F2(ms, "+05:30") if ms % 1000 == 0 else F3(ms))
            ds.ev(f"ee{i}", u, ws, typ="view"); ds.ev(f"ef{i}", u, we - 1, typ="view")
            ds.ev(f"eg{i}", u, max(ms + 1, ws), typ="view")
        # F2 offsets rolling the date around window edges
        for i, off in enumerate(["+12:00", "-10:00", "+14:00"[:1] + "05:45", "-23:59", "+23:59", "-00:00", "+00:00", "-12:30"]):
            for j, base in enumerate([ws, ws - 1000, we - 1000, we, ws + DAY - 1000]):
                ds.ev(f"of{i}_{j}", f"o{i}_{j}", F2(base, off), ing=ws + 12 * 3600_000)
        out.append(ds)
    return out

@scenario
def session_cases():
    ds = DS("sessions", "session edges: gaps exactly 1,800,000 / 1,800,001 / 1,799,999 ms, 50 events at one identical timestamp spread over files, equal ts either side of a gap, sessions crossing midnight (and the window start/end), person with 400 sessions, 3 IDs interleaving")
    rng = rng_for("sessions")
    ws, we = ds.ws, ds.we
    n = 0
    def e(u, t, typ="view", price="", pid="p1", f=None):
        nonlocal n
        n += 1
        ds.ev(f"e{n}", u, t, typ=typ, price=price, pid=pid, f=rng.randrange(4) if f is None else f, quote=.1, rng=rng)
    t0 = ws + 3 * 3600_000
    for gi, gap in enumerate([GAP, GAP + 1, GAP - 1, 0, 1, 2 * GAP, 1000, GAP + 1000]):
        u = f"gap{gi}"
        e(u, t0); e(u, t0 + gap, typ="purchase", price="700", pid="p2"); e(u, t0 + gap + gap, typ="cart")
    # many events at identical ts
    for i in range(50):
        e("same", t0 + 5 * DAY // 100, typ=["view", "cart", "purchase"][i % 3], price=str(10 + i), pid=f"p{1 + i % 9}")
    e("same", t0 + 5 * DAY // 100 + GAP); e("same", t0 + 5 * DAY // 100 + GAP, typ="purchase", price="5")
    e("same", t0 + 5 * DAY // 100 + GAP + GAP + 1)
    # equal ts on either side of the gap boundary
    for u, ts in [("eq1", [0, 0, GAP + 1, GAP + 1, GAP + 1, 2 * GAP + 2]), ("eq2", [0, GAP, GAP, GAP + 1, GAP + 1, GAP + 2 + GAP])]:
        for d in ts: e(u, t0 + 2 * DAY + d, typ="purchase" if d % 2 else "view", price="3")
    # sessions crossing midnight
    mid = (ws // DAY + 3) * DAY
    e("mid", mid - 600_000); e("mid", mid - 1); e("mid", mid + 1, typ="purchase", price="900", pid="p3"); e("mid", mid + 20 * 60_000)
    e("mid", mid + DAY - 1000); e("mid", mid + DAY - 1000 + GAP); e("mid", mid + DAY + GAP - 1000 + 1, typ="purchase", price="50")  # +GAP+1 -> new session
    e("midb", mid - 1); e("midb", mid + 2 * GAP)
    # session started exactly at midnight
    e("m0", mid); e("m0", mid + 1000, typ="purchase", price="12")
    # window start and end
    e("edge", ws); e("edge", ws + GAP); e("edge", we - 1); e("edge2", we - 1000); e("edge2", we - 1000 - GAP)
    # person with 400 sessions over the window, one event pair per session
    nses = 400 if (we - ws) >= 14 * DAY else 300
    step = (we - ws - 3600_000) // nses
    for i in range(nses):
        s = ws + i * step
        e("many", s, f=i % 4); e("many", s + 1000 + (i % 3) * 100_000, typ="purchase", price=str(1 + i), pid=f"p{1 + i % 9}")
    # linked ids interleave to one session
    ds.lk("il_c", "il_a"); ds.lk("il_b", "il_c")
    for i in range(12):
        e(["il_a", "il_b", "il_c"][i % 3], t0 + 7 * DAY // 2 + i * 600_000)
    for i in range(3):
        e(["il_a", "il_b", "il_c"][i % 3], t0 + 7 * DAY // 2 + 12 * 600_000 + GAP + 1 + i * 1000)
    # events unordered in file (descending times)
    for i in range(10, 0, -1):
        e("desc", t0 + 3 * DAY // 2 + i * 100_000 * 5)
    return [ds]

@scenario
def dedup_cases():
    ds = DS("dedup", "event dedup: 5 copies across files where the earliest-ingest copy is invalid / valid-but-late / names an (un)linked other user / is the only valid one; malformed copies never take part; equal-ingest identical copies; exact 24h boundary")
    rng = rng_for("dedup")
    ws = ds.ws
    t = ws + 4 * DAY
    ds.lk("la", "lb")            # linked pair
    def copies(eid, specs, base_ts=t, f0=0):
        # specs: list of (ingest_offset_seconds, overrides dict)
        for k, (off, ov) in enumerate(specs):
            args = dict(uid="a1", ts=base_ts, typ="view", pid="p1", price="")
            args.update(ov)
            ds.ev(eid, args["uid"], args["ts"], ing=(base_ts // 1000) * 1000 + off * 1000, typ=args["typ"], pid=args["pid"], price=args["price"], f=(f0 + k * 3 + int(off)) % 6, quote=.2, rng=rng)
    # 1 earliest invalid (bad ts / empty user / bad type / empty product / bad price), later valid -> dropped
    bad = [dict(ts="2026-10-05 10:00:00"), dict(uid=""), dict(typ="Purchase"), dict(pid=""), dict(typ="purchase", price="0"), dict(typ="purchase", price="10000001"),
           dict(ts=ws - 1), dict(ts=ds.we), dict(ts="")]
    for i, ov in enumerate(bad):
        u = f"inv{i}"
        ov = dict(ov); ov.setdefault("uid", u)
        copies(f"d1_{i}", [(10, ov), (20, dict(uid=u)), (30, dict(uid=u)), (40, dict(uid=u)), (50, dict(uid=u))], base_ts=t + i * 1000 * 100)
    # 2 earliest valid but late (ingest-ts > 24h); later copies on time -> dropped
    for i in range(3):
        u = f"late{i}"
        bt = t + 10 * DAY // 10 + i * 100_000
        ds.ev(f"d2_{i}", u, bt, ing=bt - bt % 1000 + DAY + 1000, f=0)
        ds.ev(f"d2_{i}", u, bt, ing=bt - bt % 1000 + DAY + 2000, f=2)
        # (earliest = +24h+1s is late; all later also late) and one case where earliest is late but a "rescuing" copy is *earlier*? no: smallest ingest wins
    # earliest copy is on time, later copies late -> kept
    for i in range(3):
        u = f"early{i}"; bt = t + 11 * DAY // 10 + i * 100_000
        ds.ev(f"d2e_{i}", u, bt, ing=bt - bt % 1000 + 1000, f=1)
        ds.ev(f"d2e_{i}", u, bt, ing=bt - bt % 1000 + 3 * DAY, f=3)
    # 3 copies naming different users: earliest wins; linked vs unlinked
    bt = t + 2 * 3600_000
    ds.ev("d3_a", "la", bt, ing=bt + 1000, f=0); ds.ev("d3_a", "xother", bt, ing=bt + 2000, f=1)
    ds.ev("d3_b", "lb", bt + 5000, ing=bt + 6000, f=2); ds.ev("d3_b", "la", bt + 5000, ing=bt + 7000, f=1)
    ds.ev("d3_c", "xearly", bt + 10_000, ing=bt + 11_000, f=3, typ="purchase", price="1234", pid="p4")
    ds.ev("d3_c", "la", bt + 10_000, ing=bt + 12_000, f=0, typ="purchase", price="1234", pid="p4")
    ds.ev("d3_c", "lb", bt + 10_000, ing=bt + 13_000, f=2, typ="purchase", price="1234", pid="p4")
    ds.ev("d3_c", "xearly", bt + 10_000, ing=bt + 14_000, f=5)
    ds.ev("d3_c", "xearly", bt + 10_000, ing=bt + 15_000, f=4)
    ds.ev("d3_ctrl", "xother", bt + 20_000, f=0)
    # different ts/type/product among later copies: earliest decides all fields
    ds.ev("d3_f", "dd1", bt + 30_000, ing=bt + 31_000, typ="view", f=0)
    ds.ev("d3_f", "dd1", bt + 40 * 60_000, ing=bt + 40 * 60_000 + 1000, typ="purchase", price="9999", pid="p2", f=3)
    # 4 earliest is the only valid one
    for i in range(3):
        u = f"only{i}"; bt2 = t + 3 * DAY // 2 + i * 70_000
        ds.ev(f"d4_{i}", u, bt2, ing=bt2 - bt2 % 1000 + 1000, f=0, typ="purchase", price="777", pid="p3")
        for k in range(1, 4):
            ds.ev(f"d4_{i}", u, "garbage" if k % 2 else "", ing=bt2 - bt2 % 1000 + 1000 + k * 1000, typ="view", f=k)
    # 5 malformed copies never take part
    bt3 = t + 2 * DAY
    ds.raw("events", 0, f"d5_a,mal,{F1(bt3)},{F1(bt3 - 3600_000)},view,p1,,")                 # 8 fields (earlier ingest)
    ds.ev("d5_a", "good5", bt3, ing=bt3 + 1000, f=1)
    ds.raw("events", 2, f'd5_b,mal,{F1(bt3)},{F1(bt3 - 3600_000)},view,p1,"')                  # unbalanced quote
    ds.ev("d5_b", "good5", bt3 + 1000, ing=bt3 + 5000, f=1)
    ds.raw("events", 3, f"d5_c,mal,{F1(bt3)},NOT_A_TS,view,p1")                               # 6 fields, garbage ingest
    ds.ev("d5_c", "good5", bt3 + 2000, ing=bt3 + 5000, f=2)
    ds.raw("events", 3, f"d5_d,mal,{F1(bt3)},,view,p1,")                                       # 7 fields, EMPTY ingest -> would crash ref; leave out
    ds.files["events"][ds.fname(3)].pop()
    ds.raw("events", 4, f'd5_e,"mal",{F1(bt3)},{F1(bt3 - 3600_000)},vi"ew,p1,')               # quote in the middle
    ds.ev("d5_e", "good5", bt3 + 3000, ing=bt3 + 5000, f=1)
    # 6 identical copies with same ingest, differently quoted/files
    for k in range(5):
        ds.ev("d6", "same6", bt3 + 100_000, ing=bt3 + 100_000 + 2000, f=k, quote=[None, "all", {0}, {1, 4}, {2, 5, 6}][k])
    # 7 exact 24h boundaries
    for i, (dt, name) in enumerate([(DAY, "d7_exact24"), (DAY + 1000, "d7_over1s"), (DAY - 1000, "d7_under"), (-1000, "d7_before"), (-3 * DAY, "d7_far_before"), (DAY - 500, "d7_ms_under"), (DAY + 500, "d7_ms_over")]):
        tt = bt3 + 200_000 + i * 100_000 + (500 if "ms" in name else 0)
        base = (tt // 1000) * 1000
        ing = base + dt if "ms" not in name else base + dt - 500 + 500 if False else base + dt
        # for ms cases ts has .500; ingest whole sec: ingest - ts = dt - 500 (under) / dt + 500 - 500...
        ds.ev(name, f"u_{name}", tt, ing=ing, f=i % 3)
    # 8 event ids differing by case/space
    for i, eid in enumerate(["EID", "eid", "eid ", " eid", "Eid"]):
        ds.ev(eid, "idcase", bt3 + 400_000 + i * 120_000, f=i % 3)
    # dup on the day of the window edge
    ds.ev("d9", "edge9", ds.ws + 500, ing=ds.ws + 1000, f=0)
    ds.ev("d9", "edge9", ds.ws + 500, ing=ds.ws + 2000, f=1)
    return [ds]

@scenario
def click_cases():
    out = []
    ds = DS("clicks", "click edges: same-ms clicks for different campaigns (k9/k10/K9 byte-wise tie), click exactly at session start / +1ms / 7d / 7d+1ms, linked ids, click-only person, assists, organic")
    ws = ds.ws
    t = ws + 10 * DAY
    ds.lk("ua", "ub")
    n = 0
    def sess(u, s, extra=()):
        nonlocal n
        n += 1; ds.ev(f"e{n}", u, s, typ="purchase", price="100", pid="p1")
        for d in extra: n += 1; ds.ev(f"e{n}", u, s + d, f=1)
    # tie-break cases
    for gi, ids in enumerate([("k9", "k10", "K9"), ("k10", "k9"), ("K9", "k9"), ("k", "k0", "K"), ("a", "B", "b", "~")]):
        u = f"tie{gi}"
        for j, cid in enumerate(ids):
            ds.ck(cid + ("" if gi == 0 else f"_{gi}") if False else (cid if gi == 0 else cid + f"#{gi}"), u, f"c_{1 + j}", t - 3_600_000 - gi * 0)
        sess(u, t)
    # edge offsets: click at -(offset) from session start
    offs = [("exact", 0), ("plus1", -1), ("plus1s", -1000), ("minus1", 1), ("7d", 7 * DAY), ("7d+1", 7 * DAY + 1), ("7d-1", 7 * DAY - 1), ("14d", 14 * DAY), ("1h", 3_600_000)]
    for i, (lab, d) in enumerate(offs):
        u = f"off_{lab}"
        ds.ck(f"o{i}", u, f"c_{1 + i % 9}", t - d)
        ds.ck(f"o{i}x", u, "c_9", t - d - 100 if d != 0 else t - 1)
        sess(u, t)
        sess(u, t + 2 * DAY)
        sess(u, t + 9 * DAY, extra=(1000,)) if t + 9 * DAY < ds.we else None
    # clicks of linked IDs
    ds.ck("l1", "ub", "c_3", t - 5 * DAY); ds.ck("l2", "ua", "c_4", t - 4 * DAY)
    sess("ua", t + DAY); sess("ub", t + 3 * DAY)
    # click-only person, no events
    ds.ck("co1", "ghost", "c_1", t); ds.ck("co2", "ghost2", "c_2", t - 1)
    # unknown campaign and empty user
    ds.ck("bad1", "badc", "c_404", t - 1000); ds.ck("bad2", "", "c_1", t - 1000); ds.ck("", "badc", "c_1", t - 1000)
    sess("badc", t)
    # organic only
    sess("noclick", t)
    # clicks as very latest wins vs older
    for j in range(6):
        ds.ck(f"w{j}", "wins", f"c_{1 + j}", t - 6 * DAY + j * DAY - j)
    sess("wins", t); sess("wins", t + 3 * DAY)
    # a click between events within a session (does not split / credits later)
    for j in range(4): n += 1; ds.ev(f"e{n}", "mid", t + j * 600_000)
    ds.ck("m1", "mid", "c_5", t + 700_000)
    n += 1; ds.ev(f"e{n}", "mid", t + 5 * 3_600_000)
    out.append(ds)

    ds = DS("clicks_whale", "one person with 5,000 clicks over 300 campaigns and 200 sessions, plus duplicated clicks (identical values, different quoting) in different files; campaign ids sort/ties heavy")
    rng = rng_for("whale")
    ws, we = ds.ws, ds.we
    ds.campaigns = [f"c_{i},{CHANNELS[i % 5]}" for i in range(1, 301)]
    ds.lk("whale_a", "whale_b"); ds.lk("whale_c", "whale_b")
    sessions = sorted(rng.randrange(ws, we - 1000) for _ in range(200))
    for i, s in enumerate(sessions):
        ds.ev(f"we{i}", rng.choice(["whale_a", "whale_b", "whale_c"]), s, typ="purchase" if i % 3 == 0 else "view", price="250", pid=f"p{1 + i % 9}", f=i % 4)
        ds.ev(f"wf{i}", "whale_a", s + 60_000, f=i % 4)
    for i in range(5000):
        ts = rng.randrange(ws - 7 * DAY, we)
        if rng.random() < .2:
            ts = rng.choice(sessions) - rng.choice([0, 1, 7 * DAY, 7 * DAY + 1, 1000])
        c = rng.randint(1, 300)
        cid = f"w{i}"
        uid = rng.choice(["whale_a", "whale_b", "whale_c"])
        ds.ck(cid, uid, f"c_{c}", ts, f=i % 6, quote=.2, rng=rng)
        if rng.random() < .05:
            ds.ck(cid, uid, f"c_{c}", ts, f=(i + 1) % 6, quote=.5, rng=rng)
    # same-ms ties among whale clicks
    for j in range(20):
        s = sessions[j * 7]
        for k in range(4):
            ds.ck(f"t{rng.choice('abkK')}{rng.randint(0, 99)}_{j}_{k}", "whale_c", f"c_{rng.randint(1, 300)}", s - 5000, f=k)
    out.append(ds)
    return out

@scenario
def quoting_cases():
    out = []
    rng = rng_for("quoting")
    ds = DS("quoting_all", "every field quoted in every row of every file; same data as a random world")
    base = rand_world(rng_for("quoting_all"), "x", "x")
    for folder in HEADERS:
        for fn, lines in base.files[folder].items():
            ds.files[folder][fn] = [",".join('"' + x.strip('"') + '"' for x in l.split(",")) if l.count('"') == 0 or True else l for l in lines] if lines else []
    out.append(ds)

    ds = DS("quoting_weird", "empty fields in every position (\"\" and bare), lone quote, \"\"\", quote in the middle, only at start, only at end; ingest_ts position only gets syntactically broken variants (malformed lines are dropped before parse)")
    ws = ds.ws; t = ws + 6 * 3600_000
    template = ["EID", "usr", F1(t), F1(t + 1000), "purchase", "p1", "500"]
    variants = ['""', "", '"', '"""', 'a"b', '"ab', 'ab"', '" "', '"a""b"', '""""', '"a,b"', "'x'", '\\"']
    n = 0
    for pos in range(7):
        for v in variants:
            if pos == 3 and v in ("", '""', '" "', "'x'"):    # empty/blank ingest_ts: not parseable -> oracle crash, excluded by rules
                continue
            f = list(template)
            f[pos] = v
            n += 1
            if pos != 0:
                f[0] = f"w{n}"
            elif v not in ('""', ""):
                pass
            f[1] = f[1] if pos == 1 else f"usr{n}"
            ds.raw("events", n % 3, ",".join(f))
    # the same with views (price ignored)
    for v in variants:
        n += 1
        ds.raw("events", 1, ",".join([f"w{n}", f"vv{n}", F1(t), F1(t + 1000), "view", "p1", v]))
    # clicks and links
    for pos in range(4):
        for v in variants:
            f = [f"qk{pos}", "cuser", "c_1", F1(t - 1000)]
            f[pos] = v
            f[0] = f"ck{pos}_{variants.index(v)}" if pos != 0 else f[0]
            ds.raw("clicks", pos % 2, ",".join(f))
    ds.ev("sess", "cuser", t, f=0)
    for pos in range(2):
        for v in variants:
            f = ["lk_a", "lk_b"]; f[pos] = v
            ds.raw("links", pos, ",".join(f))
    ds.ev("lka", "lk_a", t + 5000); ds.ev("lkb", "lk_b", t + 5 * 3600_000)
    # quoting the header row itself in a data position / header with quotes
    ds.raw("events", 0, '"event_id","user_id","ts","2026-10-01T00:00:00Z","event_type","product_id","price_cents"')
    # nested identity: "u1" == u1 == "u1"
    for i, q in enumerate([None, "all", {1}, {1, 2}]):
        ds.ev(f"qq{i}", "qu", t + 6 * 3600_000 + i * 1000, quote=q)
    out.append(ds)
    return out

@scenario
def field_counts():
    ds = DS("field_counts", "rows with 1..9 fields, trailing commas, consecutive commas, blank lines (mid and at EOF), a data row identical to the header in each folder, whitespace-only line, \\r-only line")
    t = ds.ws + 6 * 3600_000
    good = [f"e0,u1,{F1(t)},{F1(t + 1000)},view,p1,"]
    for n in range(0, 10):
        fields = ["ex", "u1", F1(t + 5000), F1(t + 6000), "view", "p1", "", "z", "z"][:n]
        ds.raw("events", 0, ",".join(fields) if n else "")
        if n == 7:
            ds.raw("events", 0, ",".join(fields) + ",")
    ds.raw("events", 0, f"e1,u1,{F1(t)},{F1(t + 1000)},view,p1,,")             # trailing comma -> 8 fields
    ds.raw("events", 0, f"e2,u1,{F1(t + 100000)},{F1(t + 101000)},view,p1")       # 6 fields
    ds.raw("events", 1, f"e3,u1,{F1(t + 200000)},{F1(t + 201000)},view,p1,,,")
    ds.raw("events", 1, f",,,,,,"); ds.raw("events", 1, ","); ds.raw("events", 1, " "); ds.raw("events", 1, "\r"); ds.raw("events", 1, "")
    ds.raw("events", 1, f"e4,u1,{F1(t + 300000)},{F1(t + 301000)},view,p1,")      # good
    ds.raw("events", 1, "")
    ds.raw("events", 2, HDR_EV); ds.raw("events", 0, HDR_EV); ds.raw("events", 1, HDR_EV)
    ds.raw("events", 2, f"e5,u1,{F1(t + 400000)},{F1(t + 401000)},view,p1,"); ds.raw("events", 2, ""); ds.raw("events", 2, "")
    ds.raw("clicks", 0, HEADERS["clicks"]); ds.raw("clicks", 1, HEADERS["clicks"])
    for n in range(0, 7):
        fields = ["kx", "u1", "c_1", F1(t - 1000), "c_1", "z"][:n]
        ds.raw("clicks", 0, ",".join(fields))
    ds.raw("clicks", 0, f"k1,u1,c_1,{F1(t - 1000)},"); ds.raw("clicks", 0, f"k2,u1,c_1,{F1(t - 2000)}"); ds.raw("clicks", 1, "")
    ds.raw("clicks", 1, f"k3,u1,c_2,{F1(t - 500_000)},,"); ds.raw("clicks", 1, f"k4,u1,{F1(t - 3000)}")
    ds.raw("links", 0, HEADERS["links"])
    ds.raw("links", 0, "ua,ub"); ds.raw("links", 0, "ua,ub,"); ds.raw("links", 0, "uc"); ds.raw("links", 0, "uc,ud,"); ds.raw("links", 0, ",,"); ds.raw("links", 0, "")
    ds.raw("links", 1, "ud,ue"); ds.raw("links", 1, ""); ds.raw("links", 1, "")
    ds.ev("ea", "ua", t + 3 * 3_600_000); ds.ev("eb", "ub", t + 3 * 3_600_000 + 1000)
    ds.ev("ec", "uc", t + 4 * 3_600_000); ds.ev("ee", "ue", t + 4 * 3_600_000 + 1000)
    ds.ev("ef", "user_id_a", t + 5 * 3_600_000)
    ds.ev("eg", "user_id_b", t + 5 * 3_600_000 + 1000)
    return [ds]


# ------------------------------------------------------------------ oracle-limit checks
def violations(root):
    """ids whose lines break the oracle limits (checked with the reference's own parser)."""
    bad_ev, bad_ck = set(), set()
    seen = {}
    folder = os.path.join(root, "events")
    for fn in sorted(os.listdir(folder)):
        for row in R.read_rows(os.path.join(folder, fn), 7):
            if not row[0]:
                continue
            ing = R.parse_ts(row[3])
            if ing is None:
                bad_ev.add(row[0]); continue
            seen.setdefault(row[0], []).append((ing, row))
    for eid, lst in seen.items():
        m = min(i for i, _ in lst)
        tops = [tuple(r) for i, r in lst if i == m]
        if len(set(tops)) > 1:
            bad_ev.add(eid)
    seen = {}
    folder = os.path.join(root, "clicks")
    for fn in sorted(os.listdir(folder)):
        for row in R.read_rows(os.path.join(folder, fn), 4):
            if row[0]:
                seen.setdefault(row[0], set()).add(tuple(row[1:]))
    for cid, vals in seen.items():
        if len(vals) > 1:
            bad_ck.add(cid)
    return bad_ev, bad_ck

def repair(root):
    """delete lines of violating ids (fuzz only). returns number of lines dropped."""
    bad_ev, bad_ck = violations(root)
    dropped = 0
    for folder, bad in (("events", bad_ev), ("clicks", bad_ck)):
        if not bad:
            continue
        for fn in os.listdir(os.path.join(root, folder)):
            path = os.path.join(root, folder, fn)
            data = open(path, "rb").read().split(b"\n")
            keep = [data[0]]
            for line in data[1:]:
                first = line.split(b",")[0].strip(b'"') if line else None
                if first is not None and first in bad:
                    dropped += 1; continue
                keep.append(line)
            open(path, "wb").write(b"\n".join(keep))
    return dropped

def dir_hash(root):
    h = hashlib.sha1()
    for dp, dn, fns in sorted(os.walk(root)):
        dn.sort()
        for fn in sorted(fns):
            p = os.path.join(dp, fn)
            h.update(os.path.relpath(p, root).encode()); h.update(open(p, "rb").read())
    return h.hexdigest()


# ------------------------------------------------------------------ fuzz
EVT = ["view", "cart", "purchase", "Purchase", "cart ", "", " view", "VIEW", "purchase\r", "refund"]
PRICES = ["000000000000000000250", "10000000", "10000001", "00", "0", "1", " 5", "5 ", "", "-1", "1.5", "٢٥٠", "99999999999999999999"]
PIDS = ["p7", "p5", "p007", "P5", "x1", "p 5", "", "p1", " p1", "p9999", "P1"]
CAMPS = ["c_1", "c_7", "c_007", "camp1", "C_1", "", "c_1 ", "organic", "c_9999", "c_2"]
QUOTE_MUT = ['"', '"""', 'a"b', '"ab', 'ab"', '""', '" "']

def ts_pool(ws, we, mid):
    d = DS("x", "x", (F1(ws), F1(we)))
    return [t for _, t in ts_cases(d)]

def fuzz_one(base_dir, out_dir, rng, k, tag):
    ds = DS.load(base_dir, f"fuzz{k}", "")
    ws, we = ds.ws, ds.we
    pool_ts = ts_pool(ws, we, (ws + we) // 2)
    users = set()
    for folder, col in (("events", 1), ("clicks", 1), ("links", 0), ("links", 1)):
        for lines in ds.files[folder].values():
            for l in lines:
                p = l.split(",")
                if len(p) > col:
                    users.add(p[col].strip('"'))
    users = sorted(users) or ["u1"]
    ids = users[:200] + ODD
    nm = rng.randint(3, 40)
    desc = []

    def pick_file(folder):
        names = sorted(ds.files[folder])
        n = rng.choice(names)
        return n if ds.files[folder][n] is not None else None

    for _ in range(nm):
        folder = rng.choices(["events", "clicks", "links"], [5, 3, 2])[0]
        fn = pick_file(folder)
        if fn is None:
            continue
        lines = ds.files[folder][fn]
        m = rng.choice(["field", "field", "field", "quote", "dupline", "fields", "eol", "blank", "header", "newlink", "newrow", "newclick", "ts"])
        if m in ("field", "quote", "fields", "eol", "dupline", "ts") and not lines:
            continue
        if m in ("field", "ts"):
            i = rng.randrange(len(lines))
            p = lines[i].split(",")
            if len(p) != WIDTH[folder]:
                continue
            if folder == "events":
                col = rng.choice([0, 1, 1, 2, 4, 5, 6]) if m == "field" else 2
                pool = {0: ODD, 1: ids, 2: pool_ts, 4: EVT, 5: PIDS, 6: PRICES}[col]
            elif folder == "clicks":
                col = rng.choice([0, 1, 2, 3]) if m == "field" else 3
                pool = {0: ODD, 1: ids, 2: CAMPS, 3: pool_ts}[col]
            else:
                col = rng.choice([0, 1]); pool = ids
                if m == "ts":
                    continue
            p[col] = rng.choice(pool) if not (folder == "events" and col == 6) else rng.choice(pool)
            if rng.random() < .3:
                p[col] = '"' + p[col] + '"'
            lines[i] = ",".join(p)
        elif m == "quote":
            i = rng.randrange(len(lines)); p = lines[i].split(",")
            col = rng.randrange(len(p))
            if rng.random() < .5:
                p[col] = rng.choice(QUOTE_MUT if not (folder == "events" and col == 3) else QUOTE_MUT[:5])
            else:
                p[col] = '"' + p[col].strip('"') + '"'
            lines[i] = ",".join(p)
        elif m == "dupline":
            i = rng.randrange(len(lines))
            target = pick_file(folder)
            if target is None:
                continue
            line = lines[i]
            if folder == "events" and rng.random() < .5:
                p = line.split(",")
                if len(p) == 7:
                    ing = R.parse_ts(p[3].strip('"').encode())
                    if ing is None:
                        continue
                    p[3] = F1(ing + rng.randint(1, 600) * 1000)
                    if rng.random() < .5:
                        p[1] = rng.choice(ids)
                    line = ",".join(p)
            ds.files[folder][target].append(line)
        elif m == "fields":
            i = rng.randrange(len(lines)); p = lines[i].split(",")
            r = rng.random()
            if r < .33: p.append("")
            elif r < .66 and len(p) > 1: p.pop()
            else: p.insert(rng.randrange(len(p) + 1), rng.choice(["", "x", '""']))
            lines[i] = ",".join(p)
        elif m == "eol":
            i = rng.randrange(len(lines)); lines[i] += "\r"
        elif m == "blank":
            lines.insert(rng.randint(0, len(lines)), rng.choice(["", "", " ", "\r", ","]))
        elif m == "header":
            lines.insert(rng.randint(0, len(lines)), HDR_EV if folder == "events" else HEADERS[folder])
        elif m == "newlink":
            a, b = rng.choice(ids), rng.choice(ids)
            ds.files["links"].setdefault(pick_file("links") or "part-new.csv", []).append(f"{a},{b}")
        elif m == "newrow":
            t = rng.choice([ws, we - 1, ws + rng.randrange(we - ws), ws - 1, we, ws + rng.randrange(we - ws)])
            typ = rng.choice(EVT); u = rng.choice(ids)
            f = pick_file("events") or "part-new.csv"
            ds.ev(f"fz{k}_{rng.randrange(10**6)}", u, t, ing=t - t % 1000 + rng.choice([1000, 60000, DAY, DAY + 1000, 2 * DAY]), typ=typ, pid=rng.choice(PIDS), price=rng.choice(PRICES), f=f, quote=rng.choice([None, "all", .3]), rng=rng)
        elif m == "newclick":
            t = rng.choice([ws - 7 * DAY, ws - 7 * DAY - 1, ws, we - 1, we, ws + rng.randrange(we - ws), ws + rng.randrange(we - ws)])
            f = pick_file("clicks") or "part-new.csv"
            ds.ck(rng.choice(["k9", "k10", "K9", "k1", f"fk{rng.randrange(10**6)}", ODD[rng.randrange(len(ODD))]]), rng.choice(ids), rng.choice(CAMPS), t, f=f, quote=rng.choice([None, "all", .3]), rng=rng)
        desc.append(m)

    # file level
    for _ in range(rng.randint(0, 4)):
        folder = rng.choice(list(HEADERS))
        m = rng.choice(["crlf", "notrail", "zero", "hdr", "crlf_all", "split"])
        names = [n for n in sorted(ds.files[folder])]
        if m == "crlf": ds.crlf.add((folder, rng.choice(names)))
        elif m == "crlf_all": ds.crlf.add((folder, "*"))
        elif m == "notrail": ds.notrail.add((folder, rng.choice(names)))
        elif m == "zero": ds.files[folder][f"part-zero-{rng.randrange(99)}.csv"] = None
        elif m == "hdr": ds.files[folder][f"part-hdr-{rng.randrange(99)}.csv"] = []
        elif m == "split":
            n = rng.choice(names)
            ls = ds.files[folder][n]
            if ls:
                cut = rng.randrange(len(ls)); ds.files[folder][n] = ls[:cut]; ds.files[folder][f"part-split-{rng.randrange(99)}.csv"] = ls[cut:]
        desc.append(m)
    ds.write(out_dir)
    dropped = repair(out_dir)
    return f"fuzz seed-{tag} base={os.path.basename(base_dir)} {nm} mutations ({','.join(sorted(set(desc)))}), repaired {dropped} lines"


# ------------------------------------------------------------------ run reference
def run_ref(name, in_dir, force=False):
    exp = os.path.join(DATA, "expected", "adv_" + name)
    stamp = os.path.join(exp, ".inputhash")
    h = dir_hash(in_dir)
    if not force and os.path.exists(stamp) and open(stamp).read() == h and os.path.exists(os.path.join(exp, "user_daily.csv")):
        return name, "cached", ""
    shutil.rmtree(exp, ignore_errors=True)
    p = subprocess.run([sys.executable, "-I", REF, in_dir, exp], capture_output=True, text=True)
    if p.returncode != 0:
        os.makedirs(exp, exist_ok=True)
        open(os.path.join(exp, "REF_CRASH.txt"), "w").write(p.stderr)
        return name, "CRASH", p.stderr.strip().splitlines()[-1] if p.stderr.strip() else "?"
    open(stamp, "w").write(h)
    return name, "ok", p.stderr.strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fuzz", type=int, default=0)
    ap.add_argument("--fuzz-seed", type=int, default=1)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--only", default="")
    a = ap.parse_args()
    adv = os.path.join(DATA, "adversarial")
    os.makedirs(adv, exist_ok=True)
    os.makedirs(os.path.join(DATA, "expected"), exist_ok=True)
    readme = []
    names = []
    for fn in SCEN:
        for ds in fn():
            if a.only and a.only not in ds.name:
                continue
            out = os.path.join(adv, ds.name)
            ds.write(out)
            bad = violations(out)
            if bad[0] or bad[1]:
                print(f"ERROR hand-crafted {ds.name} violates oracle limits: {sorted(bad[0])[:5]} {sorted(bad[1])[:5]}", file=sys.stderr)
                sys.exit(2)
            names.append(ds.name); readme.append(f"- `{ds.name}` (window {ds.win[0]}..{ds.win[1]}): {ds.desc}")
    bases = sorted(d for d in os.listdir(os.path.join(DATA, "seeds")) if d.startswith("small")) if os.path.isdir(os.path.join(DATA, "seeds")) else []
    if a.fuzz:
        if not bases:
            sys.exit("fuzz needs data/seeds/small* (run make_data.sh)")
        for k in range(a.fuzz):
            name = f"fuzz{a.fuzz_seed}_{k:04d}"
            if a.only and a.only not in name:
                continue
            rng = random.Random(a.fuzz_seed * 1_000_003 + k)
            base = os.path.join(DATA, "seeds", bases[k % len(bases)])
            out = os.path.join(adv, name)
            info = fuzz_one(base, out, rng, f"{a.fuzz_seed}_{k:04d}", f"{a.fuzz_seed}/{k}")
            names.append(name); readme.append(f"- `{name}`: {info}")
    if not a.only:   # stale hand-crafted datasets from earlier versions of this script
        for d in os.listdir(adv):
            if d not in names and not d.startswith("fuzz") and os.path.isdir(os.path.join(adv, d)):
                shutil.rmtree(os.path.join(adv, d)); shutil.rmtree(os.path.join(DATA, "expected", "adv_" + d), ignore_errors=True)
    # README index (hand-crafted + fuzz)
    if not a.only:
        prev = os.path.join(adv, "README.md")
        with open(prev, "w") as h:
            h.write("# Adversarial datasets\n\nEach dataset's expected output (from reference/ref_impl.py) is in data/expected/adv_<name>/.\n\n" + "\n".join(readme) + "\n")
    # reference
    crashes = []
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        futs = [ex.submit(run_ref, n, os.path.join(adv, n), a.force) for n in names]
        counts = {}
        for f in cf.as_completed(futs):
            n, st, msg = f.result()
            counts[st] = counts.get(st, 0) + 1
            if st == "CRASH":
                crashes.append((n, msg)); print(f"REFERENCE CRASH {n}: {msg}", file=sys.stderr)
    print(f"datasets {len(names)}: {counts}")
    sys.exit(1 if crashes else 0)


if __name__ == "__main__":
    main()
