# Diagnostic image (probe branch): never scored, see probe/probe.sh.
FROM gcc:14-bookworm AS build
COPY src/ /src/
COPY tools/ /tools/
RUN if [ "$(uname -m)" = "x86_64" ]; then V3="-march=x86-64-v3 -mtune=skylake-avx512 -Wa,-mbranches-within-32B-boundaries"; else V3=""; fi \
 && B="g++ -std=c++20 -fno-plt -static -pthread -O3 $V3 /src/main.cpp" \
 && $B -o /p \
 && for l in 2 3 4 5 7; do $B -DSS_LVL=$l -o /pl$l || exit 1; done \
 && $B -DSS_PROBE=5 -o /pnc \
 && for x in 5 6 9; do $B -DSS_X=$x -o /px$x || exit 1; done \
 && g++ -std=c++20 -O2 -static -pthread -o /expand /tools/expand.cpp \
 && strip /p /pl2 /pl3 /pl4 /pl5 /pl7 /pnc /px5 /px6 /px9 /expand

# The same source under two other compilers. If either cannot be built, the
# default binary stands in for it, so the probe still runs.
FROM gcc:15-bookworm AS g15
COPY src/ /src/
RUN if [ "$(uname -m)" = "x86_64" ]; then V3="-march=x86-64-v3 -mtune=skylake-avx512 -Wa,-mbranches-within-32B-boundaries"; else V3=""; fi \
 && g++ -std=c++20 -fno-plt -static -pthread -O3 $V3 /src/main.cpp -o /p15 && strip /p15

FROM debian:trixie-slim AS clang
COPY src/ /src/
RUN (apt-get update && apt-get install -y --no-install-recommends clang-19 libstdc++-14-dev libc6-dev binutils) > /dev/null 2>&1; \
    if [ "$(uname -m)" = "x86_64" ]; then V3="-march=x86-64-v3 -mtune=skylake-avx512 -mbranches-within-32B-boundaries"; else V3=""; fi; \
    (clang++-19 -std=c++20 -fno-plt -static -pthread -O3 $V3 /src/main.cpp -o /pcl && strip /pcl) || echo missing > /pcl

FROM debian:bookworm-slim
COPY --from=build /p /pl2 /pl3 /pl4 /pl5 /pl7 /pnc /px5 /px6 /px9 /expand /
COPY --from=g15 /p15 /p15
COPY --from=clang /pcl /pcl
COPY probe/probe.sh /probe.sh
CMD ["/bin/sh", "/probe.sh"]
