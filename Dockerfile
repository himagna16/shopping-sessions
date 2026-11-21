# Diagnostic image (probe branch): never scored, see probe/probe.sh.
FROM gcc:14-bookworm AS build
COPY src/ /src/
COPY tools/ /tools/
RUN if [ "$(uname -m)" = "x86_64" ]; then V3="-march=x86-64-v3 -mtune=skylake-avx512 -Wa,-mbranches-within-32B-boundaries"; else V3=""; fi \
 && B="g++ -std=c++20 -fno-plt -static -pthread -O3 $V3 /src/main.cpp" \
 && $B -o /p \
 && $B -DSS_NOHOTINLINE -o /pnh \
 && $B -DSS_NOWALKINLINE -o /pnw \
 && $B -DSS_ID2 -o /pid2 \
 && $B -DSS_ASSIST1 -o /pa1 \
 && $B -DSS_NOBLOCKPF -o /pnb \
 && $B -DSS_SCANMAX=0 -o /ps0 && $B -DSS_SCANMAX=6 -o /ps6 \
 && for l in 2 3 4 5 6; do $B -DSS_CLVL=$l -o /pc$l || exit 1; done \
 && for d in 8 32 64; do $B -DSS_DIST=$d -o /pd$d || exit 1; done \
 && g++ -std=c++20 -O2 -static -pthread -o /expand /tools/expand.cpp \
 && strip /p /pnh /pnw /pid2 /pa1 /pnb /ps0 /ps6 /pc2 /pc3 /pc4 /pc5 /pc6 /pd8 /pd32 /pd64 /expand

FROM gcc:15-bookworm AS g15
COPY src/ /src/
RUN if [ "$(uname -m)" = "x86_64" ]; then V3="-march=x86-64-v3 -mtune=skylake-avx512 -Wa,-mbranches-within-32B-boundaries"; else V3=""; fi \
 && g++ -std=c++20 -fno-plt -static -pthread -O3 $V3 /src/main.cpp -o /p15 && strip /p15

FROM debian:bookworm-slim
COPY --from=build /p /pnh /pnw /pid2 /pa1 /pnb /ps0 /ps6 /pc2 /pc3 /pc4 /pc5 /pc6 /pd8 /pd32 /pd64 /expand /
COPY --from=g15 /p15 /p15
COPY probe/probe.sh /probe.sh
CMD ["/bin/sh", "/probe.sh"]
