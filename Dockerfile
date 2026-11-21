# Diagnostic image (probe branch): never scored, see probe/probe.sh.
# lead/ is Team 67's public entry (MIT, see lead/LICENSE), built here unchanged to be measured.
FROM rust:1-slim-bookworm AS gen
COPY probe/generator /g
RUN cd /g && cargo build --release

FROM rust:1-alpine AS xl
RUN apk add --no-cache musl-dev
COPY lead /x
RUN cd /x && T=x86_64-unknown-linux-musl \
 && cargo build -q --release --target $T --target-dir /t/a && cp /t/a/$T/release/pipeline /xl \
 && XL_BB=14 cargo build -q --release --target $T --target-dir /t/b && cp /t/b/$T/release/pipeline /xl14 \
 && XL_BB=16 cargo build -q --release --target $T --target-dir /t/c && cp /t/c/$T/release/pipeline /xl16

FROM gcc:15-bookworm AS build
COPY src/ /src/
RUN if [ "$(uname -m)" = "x86_64" ]; then V3="-march=x86-64-v3 -mtune=skylake-avx512 -Wa,-mbranches-within-32B-boundaries"; else V3=""; fi \
 && B="g++ -std=c++20 -fno-plt -static -pthread -O3 -fno-tree-vectorize $V3 /src/main.cpp" \
 && $B -o /p \
 && strip /p

FROM python:3.13-slim
RUN pip install --no-cache-dir duckdb
COPY --from=gen /g/target/release/generate /generate
COPY --from=xl /xl /xl14 /xl16 /
COPY --from=build /p /
COPY probe/probe.sh probe/cmp.py /
CMD ["/bin/sh", "/probe.sh"]
