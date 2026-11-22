#!/usr/bin/env python3
"""
Scorer version 5 data: one in eight distinct event_id, user_id and click_id values is the
letter plus 32 hexadecimal characters instead of short base 36 (rules.md, generator section).

make_long_ids.py SRC DST   copies dataset SRC to DST, rewriting one in eight of the distinct
canonical IDs (letter + 6 to 8 of 0-9a-z, as a whole field, quoted or not) to that long form.
The choice and the new spelling depend only on the ID, so every file agrees. Everything else,
malformed lines included, is copied byte for byte.
"""
import hashlib, os, re, shutil, sys

FIELD = re.compile(rb'(?<![^,"\n])([euk])([0-9a-z]{6,8})(?![^,"\r\n])')

def swap(m):
    digest = hashlib.md5(m.group(0)).hexdigest().encode()
    if int(digest[:2], 16) >= 32:           # seven in eight stay as they are
        return m.group(0)
    return m.group(1) + hashlib.md5(b"long:" + m.group(0)).hexdigest().encode()

def main(src, dst):
    for folder, _, names in os.walk(src):
        out = os.path.join(dst, os.path.relpath(folder, src))
        os.makedirs(out, exist_ok=True)
        for name in names:
            a, b = os.path.join(folder, name), os.path.join(out, name)
            top = os.path.relpath(folder, src).split(os.sep)[0]
            if top in ("events", "clicks", "links"):
                with open(a, "rb") as f:
                    head, _, body = f.read().partition(b"\n")     # the header row is left alone
                with open(b, "wb") as f:
                    f.write(head + b"\n" + FIELD.sub(swap, body) if _ else head)
            else:
                shutil.copyfile(a, b)

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
