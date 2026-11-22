# Shopping Sessions

Entry for the Databricks "Shopping Sessions" track of the Builders Buildathon:
clean about 60 million click-stream events into two exact daily Parquet tables,
as fast as possible.

One C++ program, no dependencies, built into a static binary on an empty image.

## How it works

1. **Parse.** Every CSV is read in line-aligned chunks from one shared queue,
   a line at a time (`src/lines.h`): the comma, newline and quote bits of two
   or three 64-byte windows give a usual line's fields directly, and any other
   line is decided byte by byte. Fields are validated and converted in place. Links are parsed and joined (union-find) on one
   thread while the others parse events.
2. **De-duplicate.** Each event copy becomes a 32-byte record placed in a
   bucket chosen by its `event_id`. Per bucket, the copy with the smallest
   `ingest_ts` wins; if that copy is invalid the event is dropped. Survivors
   get their person and move to a bucket chosen by person.
3. **People.** Per bucket, each person's events are ordered by time, cut into
   sessions, credited to the latest click in the 7 days before, and counted
   into daily rows. Each worker writes its own `user_daily` Parquet file; the
   small campaign table is merged and written once.

### IDs

IDs are opaque strings. One that is its column's letter plus 1 to 8 ASCII bytes
is packed into a 64-bit key that keeps the byte order. Any other ID (since
scorer version 5 one in eight is the letter plus 32 hexadecimal characters) is
interned: stored once in a table split in 256 parts, read without locks, and
keyed by the address of its copy, so comparing and writing it needs no look-up.
A look-up is one hash and one table slot; the slot holds the whole 64-bit hash
and a hit is taken on it (`-DSS_INTERN_VERIFY` compares the bytes as well: two
different long IDs with one hash are one chance in a few million per run).
A long `click_id` is not stored at all: it is only looked at to order two clicks
of one person with the same time, so its key says where its bytes are in the
input. An `event_id` is only ever compared for equality, so a long one is keyed by a
63-bit hash of its bytes instead of being stored (build with `-DSS_EVENT_INTERN`
to intern those too; two different long event IDs meeting in one key would take
about one run in 400,000 at the final's size).

`src/dense.h` holds an earlier experiment that turned the generator's short IDs
back into numbers. The rules now say IDs must not be treated that way, so it is
off; it only runs when asked for (`SS_DENSE=1`), and the tests still check it
against the reference.

Parquet is
written by a small hand-rolled writer (`src/parquet.h`): v1 pages, PLAIN,
uncompressed.

## Correctness

`reference/ref_impl.py` is a slow pure-Python implementation written straight
from `rules.md`. It passes the 36 organizer fixtures and reproduces the
published 1M-event dev answer exactly. The fast build is compared with it on
the fixtures, the dev set, many generator seeds and hand-made adversarial
inputs (`tests/`), under several thread counts and chunk sizes, and against a
build with every fast path disabled (`make slow`). `tests/make_long_ids.py`
rewrites one in eight IDs of any dataset to the long form, and every such copy
is checked against the reference as well.

## Build and run

    make native                       # local build -> build/pipeline
    DATA_DIR=dev OUTPUT_DIR=out build/pipeline
    make image                        # the linux/amd64 image the scorer builds

Environment: `DATA_DIR`, `OUTPUT_DIR`, `NCPU`. `SS_THREADS` and `SS_CHUNK`
override the worker count and the input chunk size for testing.

MIT licensed.
