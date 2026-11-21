# Diagnostic image (probe branch): never scored, see probe/probe.sh.
FROM gcc:14-bookworm AS build
COPY src/ /src/
COPY tools/ /tools/
RUN if [ "$(uname -m)" = "x86_64" ]; then V3="-march=x86-64-v3 -mtune=skylake-avx512"; else V3=""; fi \
 && B="g++ -std=c++20 -fno-plt -static -pthread -O3 $V3 /src/main.cpp" \
 && $B -o /p \
 && for q in 1 2 3 5; do $B -DSS_PROBE=$q -o /q$q; done \
 && for x in 1 2 3 4 5 6 7; do $B -DSS_X=$x -o /x$x; done \
 && g++ -std=c++20 -O2 -static -pthread -o /expand /tools/expand.cpp \
 && strip /p /q1 /q2 /q3 /q5 /x1 /x2 /x3 /x4 /x5 /x6 /x7 /expand

FROM debian:bookworm-slim
COPY --from=build /p /q1 /q2 /q3 /q5 /x1 /x2 /x3 /x4 /x5 /x6 /x7 /expand /
COPY probe/probe.sh /probe.sh
CMD ["/bin/sh", "/probe.sh"]
