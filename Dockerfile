# Diagnostic image (probe branch): never scored, see probe/probe.sh.
# lead/ is Team 67's public entry (MIT, see lead/LICENSE), built here unchanged to be measured.
FROM rust:1-slim-bookworm AS gen
COPY probe/generator /g
RUN cd /g && cargo build --release

FROM rust:1-alpine AS xl
RUN apk add --no-cache musl-dev
COPY lead /x
COPY lead2 /y
RUN T=x86_64-unknown-linux-musl \
 && cd /x && cargo build --release --target $T --target-dir /t/a && cp /t/a/$T/release/pipeline /xl \
 && cd /y && cargo build --release --target $T --target-dir /t/b && cp /t/b/$T/release/pipeline /xn \
 && XL_UB=13 cargo build --release --target $T --target-dir /t/c && cp /t/c/$T/release/pipeline /xn13

FROM gcc:15-bookworm AS build
COPY src/ /src/
RUN if [ "$(uname -m)" = "x86_64" ]; then V3="-march=x86-64-v3 -mtune=skylake-avx512 -Wa,-mbranches-within-32B-boundaries"; else V3=""; fi \
 && B="g++ -std=c++20 -fno-plt -static -pthread -O3 -fno-tree-vectorize $V3 /src/main.cpp" \
 && $B -o /p \
 && strip /p

FROM python:3.13-slim
RUN pip install --no-cache-dir duckdb
COPY --from=gen /g/target/release/generate /generate
COPY --from=xl /xl /xn /xn13 /
COPY --from=build /p /
COPY probe/probe.sh probe/cmp.py /
COPY fx /fx
CMD ["/bin/sh", "/probe.sh"]
