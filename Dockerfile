# Diagnostic image (probe branch): never scored, see probe/probe.sh.
FROM rust:1-slim-bookworm AS gen
COPY probe/generator /g
RUN cd /g && cargo build --release

FROM gcc:15-bookworm AS build
COPY src/ /src/
RUN if [ "$(uname -m)" = "x86_64" ]; then V3="-march=x86-64-v3 -mtune=skylake-avx512 -Wa,-mbranches-within-32B-boundaries"; else V3=""; fi \
 && B="g++ -std=c++20 -fno-plt -static -pthread -O3 -fno-tree-vectorize $V3 /src/main.cpp" \
 && $B -o /p \
 && $B -DSS_DW=14 -o /p14 \
 && $B -DSS_GROUP=1024 -o /pg1 \
 && $B -DSS_GROUP=4096 -o /pg4 \
 && $B -DSS_GROUP=8192 -o /pg8 \
 && $B -DSS_LPF=512 -o /pf5 \
 && $B -DSS_DIGEST -o /pdg \
 && strip /p /p14 /pg1 /pg4 /pg8 /pf5 /pdg

FROM debian:bookworm-slim
COPY --from=gen /g/target/release/generate /generate
COPY --from=build /p /p14 /pg1 /pg4 /pg8 /pf5 /pdg /
COPY probe/probe.sh /probe.sh
CMD ["/bin/sh", "/probe.sh"]
