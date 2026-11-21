# Diagnostic image (probe branch): never scored, see probe/probe.sh.
FROM gcc:14-bookworm AS build
COPY src/ /src/
COPY tools/ /tools/
RUN if [ "$(uname -m)" = "x86_64" ]; then V3="-march=x86-64-v3 -mtune=skylake-avx512 -Wa,-mbranches-within-32B-boundaries"; else V3=""; fi \
 && B="g++ -std=c++20 -fno-plt -static -pthread -O3 $V3 /src/main.cpp" \
 && $B -o /p \
 && $B -DSS_SCAN2 -o /ps2 \
 && $B -DSS_SCAN3 -o /ps3 \
 && $B -DSS_ING2 -o /pi2 \
 && $B -DSS_SCAN3 -DSS_ING2 -o /pc \
 && $B -DSS_SCAN3 -DSS_ING2 -DSS_ID2 -o /pcd \
 && $B -DSS_SCAN3 -DSS_ING2 -DSS_PROBE=5 -o /cq5 \
 && for l in 2 3 5 7; do $B -DSS_SCAN3 -DSS_ING2 -DSS_PROBE=5 -DSS_LVL=$l -o /cl$l; done \
 && g++ -std=c++20 -O2 -static -pthread -o /expand /tools/expand.cpp \
 && strip /p /ps2 /ps3 /pi2 /pc /pcd /cq5 /cl2 /cl3 /cl5 /cl7 /expand

FROM debian:bookworm-slim
COPY --from=build /p /ps2 /ps3 /pi2 /pc /pcd /cq5 /cl2 /cl3 /cl5 /cl7 /expand /
COPY probe/probe.sh /probe.sh
CMD ["/bin/sh", "/probe.sh"]
