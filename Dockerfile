# Diagnostic image (probe branch): never scored, see probe/probe.sh.
FROM gcc:15-bookworm AS build
COPY src/ /src/
COPY tools/ /tools/
RUN if [ "$(uname -m)" = "x86_64" ]; then V3="-march=x86-64-v3 -mtune=skylake-avx512 -Wa,-mbranches-within-32B-boundaries"; else V3=""; fi \
 && B="g++ -std=c++20 -fno-plt -static -pthread -O3 $V3 /src/main.cpp" \
 && $B -o /p \
 && $B -fno-tree-vectorize -o /pnv \
 && $B -fno-tree-slp-vectorize -o /pns \
 && $B -fno-tree-loop-vectorize -o /pnl \
 && $B -fno-tree-vectorize -falign-loops=64 -falign-functions=64 -o /pal \
 && $B -fno-tree-vectorize -fno-unroll-loops -fno-peel-loops -o /pnu \
 && g++ -std=c++20 -O2 -static -pthread -o /expand /tools/expand.cpp \
 && strip /p /pnv /pns /pnl /pal /pnu /expand

FROM debian:bookworm-slim
COPY --from=build /p /pnv /pns /pnl /pal /pnu /expand /
COPY probe/probe.sh /probe.sh
CMD ["/bin/sh", "/probe.sh"]
