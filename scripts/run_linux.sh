#!/bin/sh
# Local stand-in for the scorer: run IMAGE on a dataset held in the Docker
# volume `ssdata` (so reads come from the Linux page cache, not a macOS share),
# with the scorer's flags. Usage: run_linux.sh DATASET [-e NAME=VALUE ...]
set -eu
IMAGE="${IMAGE:-shopping-sessions:arm64}"
DATASET="$1"; shift
docker volume create ssout >/dev/null
docker run --rm -v ssout:/output "${HELPER:-node:24-bookworm}" sh -c 'rm -rf /output/* && chmod 777 /output' >/dev/null 2>&1 || true
docker run --rm \
  --network none --read-only --tmpfs /tmp:rw,size=64m,mode=1777 \
  --cap-drop ALL --security-opt no-new-privileges \
  --pids-limit 4096 --cpus "${CPUS:-8}" --memory "${MEM:-6g}" --memory-swap "${MEM:-6g}" \
  --user 1000:1000 \
  -v ssdata:/vol:ro -v ssout:/output -v ssout:/scratch \
  -e DATA_DIR="/vol/$DATASET" -e OUTPUT_DIR=/output -e SCRATCH_DIR=/scratch -e HOME=/scratch \
  -e NCPU="${CPUS:-8}" -e MEM_GB=24 "$@" \
  "$IMAGE"
