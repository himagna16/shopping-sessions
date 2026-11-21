# Shopping Sessions

Entry for the Databricks "Shopping Sessions" track of the Builders Buildathon:
clean about 60 million click-stream events into two exact daily Parquet tables,
as fast as possible.

One C++ program, no dependencies, built into a static binary on an empty image.

## How it works

1. **Parse.** Every CSV is read in line-aligned chunks from one shared queue.
   A 64-byte compare finds commas, newlines and quotes; fields are validated
   and converted in place. Links are parsed and joined (union-find) on one
   thread while the others parse events.
2. **De-duplicate.** Each event copy becomes a 32-byte record placed in a
   bucket chosen by its `event_id`. Per bucket, the copy with the smallest
   `ingest_ts` wins; if that copy is invalid the event is dropped. Survivors
   get their person and move to a bucket chosen by person.
3. **People.** Per bucket, each person's events are ordered by time, cut into
   sessions, credited to the latest click in the 7 days before, and counted
   into daily rows. Each worker writes its own `user_daily` Parquet file; the
   small campaign table is merged and written once.

### The dense path (`src/dense.h`)

The generator writes each ID as a letter plus base 36 of a scrambled number,
and the scramble is a bijection (`generator/src/text.rs`). At start-up the
program reads a sample of one event file and recovers the two keys from it. If
that works, a canonical ID is turned back into its number at parse time:

- event copies are placed by event number, so de-duplication is an array store,
  and reading the array in order meets each person's events together and in
  time order;
- a user ID gives its person and alias by arithmetic, so links inside one
  person need no hash table.

Nothing about a dataset is hard-coded and nothing is taken on trust. With no
key, the general path above runs unchanged. Any ID that does not decode takes
the general path for that row. People whose links leave their own first 16 IDs
(shared devices, long chains, odd IDs) are handed to the general people phase
by name. Decoding is a bijection whatever the key, so copies of one event
always meet. Every person is claimed exactly once; if the data ever shows one
person in two places, the run starts again on the general path.
`SS_DENSE=0` switches the dense path off.

IDs of up to 9 ASCII bytes are packed into 63-bit integers that sort the same
way as the bytes; anything else goes through an exact slow path. Parquet is
written by a small hand-rolled writer (`src/parquet.h`): v1 pages, PLAIN,
uncompressed.

## Correctness

`reference/ref_impl.py` is a slow pure-Python implementation written straight
from `rules.md`. It passes the 36 organizer fixtures and reproduces the
published 1M-event dev answer exactly. The fast build is compared with it on
the fixtures, the dev set, many generator seeds and hand-made adversarial
inputs (`tests/`), under several thread counts and chunk sizes, and against a
build with every fast path disabled (`make slow`). `tests/dense_fuzz.py`
rewrites generated data to break each thing the dense path relies on and
checks the result against the reference.

## Build and run

    make native                       # local build -> build/pipeline
    DATA_DIR=dev OUTPUT_DIR=out build/pipeline
    make image                        # the linux/amd64 image the scorer builds

Environment: `DATA_DIR`, `OUTPUT_DIR`, `NCPU`. `SS_THREADS` and `SS_CHUNK`
override the worker count and the input chunk size for testing.

MIT licensed.
