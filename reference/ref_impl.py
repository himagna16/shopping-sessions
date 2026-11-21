#!/usr/bin/env python3
"""
Slow, obviously-correct reference for Shopping Sessions (rules.md), pure Python.

It is the oracle for differential testing of the fast native solution: every
step follows rules.md literally, on raw bytes, with no cleverness.

Usage:  python3 -I ref_impl.py DATA_DIR OUT_DIR
Writes  OUT_DIR/user_daily.csv and OUT_DIR/campaign_daily.csv (same layout as
the fixtures' expected/ files: day as YYYY-MM-DD, empty top_category for null).
"""
import csv
import datetime
import glob
import json
import os
import sys
from collections import defaultdict

DAY = 86_400_000
SESSION_GAP = 1_800_000
REACH = 7 * DAY
LATE = DAY
DIGITS = frozenset(b"0123456789")


# ---------------------------------------------------------------- CSV dialect
def read_rows(path, width):
    """Yield the fields of every well-formed data line. Only \\n ends a line;
    the first line is the header; a quote is legal only around a whole field."""
    with open(path, "rb") as handle:
        data = handle.read()
    lines = data.split(b"\n")
    if lines and lines[-1] == b"":
        lines.pop()                      # the text after the final \n is not a line
    for line in lines[1:]:               # lines[0] is the header
        if line == b"":
            continue                     # an empty line is malformed
        fields = line.split(b",")        # every comma separates fields
        if len(fields) != width:
            continue
        out = []
        for field in fields:
            if b'"' in field:
                if len(field) < 2 or field[:1] != b'"' or field[-1:] != b'"' or b'"' in field[1:-1]:
                    out = None
                    break
                field = field[1:-1]
            out.append(field)
        if out is not None:
            yield out


def files(data_dir, folder):
    return sorted(glob.glob(os.path.join(data_dir, folder, "*.csv")))


# ---------------------------------------------------------------- timestamps
def all_digits(text):
    return len(text) > 0 and all(byte in DIGITS for byte in text)


def days_from_civil(year, month, day):
    year -= month <= 2
    era = year // 400
    year_of_era = year - era * 400
    day_of_year = (153 * ((month + 9) % 12) + 2) // 5 + day - 1
    return era * 146_097 + year_of_era * 365 + year_of_era // 4 - year_of_era // 100 + day_of_year - 719_468


def days_in_month(year, month):
    if month == 2:
        leap = year % 4 == 0 and (year % 100 != 0 or year % 400 == 0)
        return 29 if leap else 28
    return 30 if month in (4, 6, 9, 11) else 31


def parse_civil(text):
    """`YYYY-MM-DDTHH:MM:SS` (19 bytes) to epoch ms of that wall-clock time, or None."""
    if len(text) != 19:
        return None
    if text[4:5] != b"-" or text[7:8] != b"-" or text[10:11] != b"T" or text[13:14] != b":" or text[16:17] != b":":
        return None
    parts = (text[0:4], text[5:7], text[8:10], text[11:13], text[14:16], text[17:19])
    if not all(all_digits(part) for part in parts):
        return None
    year, month, day, hour, minute, second = (int(part) for part in parts)
    if not 1 <= month <= 12 or not 1 <= day <= days_in_month(year, month):
        return None
    if hour > 23 or minute > 59 or second > 59:
        return None
    return days_from_civil(year, month, day) * DAY + (hour * 3600 + minute * 60 + second) * 1000


def parse_ts(text):
    """Epoch milliseconds for forms F1, F2, F3; None for a bad timestamp."""
    if all_digits(text):                                  # F3 (any length, leading zeros fine)
        return int(text)
    if len(text) == 20 and text[19:20] == b"Z":           # F1
        return parse_civil(text[:19])
    if len(text) == 25 and text[19:20] in (b"+", b"-") and text[22:23] == b":":   # F2
        local = parse_civil(text[:19])
        hours, minutes = text[20:22], text[23:25]
        if local is None or not all_digits(hours) or not all_digits(minutes):
            return None
        hours, minutes = int(hours), int(minutes)
        if hours > 23 or minutes > 59:
            return None
        offset = (hours * 60 + minutes) * 60_000
        return local - offset if text[19:20] == b"+" else local + offset
    return None


# ---------------------------------------------------------------- linked IDs
class Persons:
    def __init__(self):
        self.parent = {}

    def find(self, item):
        parent = self.parent
        root = item
        while parent.get(root, root) != root:
            root = parent[root]
        while parent.get(item, item) != root:
            parent[item], item = root, parent[item]
        return root

    def union(self, a, b):
        a, b = self.find(a), self.find(b)
        if a != b:
            # The root is always the byte-wise smallest ID, so find() is the name.
            if b < a:
                a, b = b, a
            self.parent[b] = a


def run(data_dir):
    with open(os.path.join(data_dir, "manifest.json"), "rb") as handle:
        manifest = json.load(handle)
    window_start = parse_ts(manifest["window_start"].encode())
    window_end = parse_ts(manifest["window_end"].encode())

    category = {row[0]: row[1] for row in read_rows(os.path.join(data_dir, "products.csv"), 2)}
    channel = {row[0]: row[1] for row in read_rows(os.path.join(data_dir, "campaigns.csv"), 2)}

    persons = Persons()
    for path in files(data_dir, "links"):
        for a, b in read_rows(path, 2):
            if a and b:
                persons.union(a, b)

    # Events: parse, dedup (smallest ingest_ts, before validation), validate, late.
    kept = {}
    for path in files(data_dir, "events"):
        for row in read_rows(path, 7):
            if not row[0]:
                continue
            ingest = parse_ts(row[3])
            if ingest is None:           # rules: ingest_ts is always valid F1
                raise ValueError(f"invalid ingest_ts {row[3]!r} in {path}")
            current = kept.get(row[0])
            if current is None or ingest < current[0]:
                kept[row[0]] = (ingest, row)

    by_person = defaultdict(list)        # person -> [(ts, price or None, product)]
    for ingest, (_, user, ts_text, _, kind, product, price_text) in kept.values():
        ts = parse_ts(ts_text)
        if not user or ts is None or not window_start <= ts < window_end:
            continue
        if kind not in (b"view", b"cart", b"purchase") or not product:
            continue
        price = None
        if kind == b"purchase":
            if not all_digits(price_text):
                continue
            price = int(price_text)
            if not 1 <= price <= 10_000_000:
                continue
        if ingest - ts > LATE:
            continue
        by_person[persons.find(user)].append((ts, price, product))

    # Clicks: parse, dedup, validate.
    clicks = {}
    for path in files(data_dir, "clicks"):
        for click_id, user, campaign, ts_text in read_rows(path, 4):
            if click_id and click_id not in clicks:
                clicks[click_id] = (user, campaign, ts_text)
    clicks_by_person = defaultdict(list)  # person -> [(ts, click_id, campaign)]
    for click_id, (user, campaign, ts_text) in clicks.items():
        ts = parse_ts(ts_text)
        if not user or ts is None or not window_start - REACH <= ts < window_end:
            continue
        if campaign not in channel:
            continue
        clicks_by_person[persons.find(user)].append((ts, click_id, campaign))

    user_daily = {}                      # (person, day) -> [sessions, events, revenue, {category: cents}]
    campaign_daily = defaultdict(lambda: [0, 0, 0, 0])   # (campaign, day) -> sessions, purchasing, revenue, assisted
    for person, events in by_person.items():
        events.sort(key=lambda event: event[0])
        sessions = []
        for event in events:
            if not sessions or event[0] - sessions[-1][-1][0] > SESSION_GAP:
                sessions.append([])
            sessions[-1].append(event)
        mine = clicks_by_person.get(person, ())
        for session in sessions:
            start = session[0][0]
            day = start // DAY
            revenue = 0
            row = user_daily.setdefault((person, day), [0, 0, 0, defaultdict(int)])
            for _, price, product in session:
                if price is not None:
                    revenue += price
                    row[3][category.get(product, b"unknown")] += price
            row[0] += 1
            row[1] += len(session)
            row[2] += revenue
            purchased = any(price is not None for _, price, _ in session)

            reach = [click for click in mine if start - REACH <= click[0] <= start]
            if reach:
                # latest ts, then the byte-wise smallest click_id
                best = min(reach, key=lambda click: (-click[0], click[1]))
                credited = best[2]
            else:
                credited = b"organic"
            totals = campaign_daily[(credited, day)]
            totals[0] += 1
            totals[1] += purchased
            totals[2] += revenue
            for campaign in {click[2] for click in reach}:
                campaign_daily[(campaign, day)][3] += 1

    def date(day):
        return (datetime.date(1970, 1, 1) + datetime.timedelta(days=day)).isoformat()

    users = []
    for (person, day), (count, events, revenue, sums) in user_daily.items():
        top = ""
        if sums:
            best = max(sums.values())
            top = min(name for name, cents in sums.items() if cents == best).decode()
        users.append((person.decode(), date(day), count, events, revenue, top))
    campaigns = []
    for (campaign, day), (count, purchasing, revenue, assisted) in campaign_daily.items():
        name = "organic" if campaign == b"organic" else channel[campaign].decode()
        campaigns.append((campaign.decode(), name, date(day), count, purchasing, revenue, assisted))
    return sorted(users), sorted(campaigns)


def main():
    data_dir, out_dir = sys.argv[1:3]
    users, campaigns = run(data_dir)
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "user_daily.csv"), "w", newline="") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(["user_id", "day", "sessions", "events", "revenue_cents", "top_category"])
        writer.writerows(users)
    with open(os.path.join(out_dir, "campaign_daily.csv"), "w", newline="") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(["campaign_id", "channel", "day", "sessions", "purchasing_sessions",
                         "revenue_cents", "assisted_sessions"])
        writer.writerows(campaigns)
    print(f"user_daily {len(users)} rows, campaign_daily {len(campaigns)} rows", file=sys.stderr)


if __name__ == "__main__":
    main()
