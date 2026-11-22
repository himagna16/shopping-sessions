# Diagnostic image (probe branch): never scored, see probe/probe.sh.
FROM rust:1-slim-bookworm AS gen
COPY probe/generator /g
RUN cd /g && cargo build --release

FROM gcc:15-bookworm AS build
COPY src/ /src/
RUN if [ "$(uname -m)" = "x86_64" ]; then V3="-march=x86-64-v3 -mtune=skylake-avx512 -Wa,-mbranches-within-32B-boundaries"; else V3=""; fi \
 && B="g++ -std=c++20 -fno-plt -static -pthread -O3 -fno-tree-vectorize $V3 /src/main.cpp" \
 && $B -o /p \
 && $B -DSS_INTERN_LAZY -o /plz \
 && $B -DSS_PROBE=5 -o /pnc \
 && for l in 1 3 4 5 7; do $B -DSS_LVL=$l -o /l$l || exit 1; done \
 && strip /p /plz /pnc /l?
COPY probe/longids.cpp /longids.cpp
RUN g++ -std=c++17 -O2 -static -o /longids /longids.cpp

FROM debian:bookworm-slim
COPY --from=gen /g/target/release/generate /generate
COPY --from=build /p /plz /pnc /l1 /l3 /l4 /l5 /l7 /longids /
COPY probe/probe.sh /probe.sh
CMD ["/bin/sh", "/probe.sh"]
