# Diagnostic image (probe branch): never scored, see probe/probe.sh.
FROM gcc:15-bookworm AS build
COPY src/ /src/
COPY tools/ /tools/
RUN if [ "$(uname -m)" = "x86_64" ]; then V3="-march=x86-64-v3 -mtune=skylake-avx512 -Wa,-mbranches-within-32B-boundaries"; CL="-march=cascadelake -mprefer-vector-width=256 -Wa,-mbranches-within-32B-boundaries"; else V3=""; CL=""; fi \
 && B="g++ -std=c++20 -fno-plt -static -pthread" \
 && $B -O3 $V3 /src/main.cpp -o /p \
 && $B -O3 $V3 -DSS_ALIAS1 /src/main.cpp -o /pa1 \
 && $B -O3 $V3 -DSS_NOSLICEPF /src/main.cpp -o /pns \
 && $B -O3 $V3 -DSS_ID1 /src/main.cpp -o /pid1 \
 && $B -O3 $V3 -DSS_NOHOTINLINE /src/main.cpp -o /pnh \
 && $B -O2 $V3 /src/main.cpp -o /po2 \
 && $B -O3 $CL /src/main.cpp -o /pcl \
 && $B -O3 $V3 -fno-tree-vectorize /src/main.cpp -o /pnv \
 && g++ -std=c++20 -O2 -static -pthread -o /expand /tools/expand.cpp \
 && strip /p /pa1 /pns /pid1 /pnh /po2 /pcl /pnv /expand

FROM debian:bookworm-slim
COPY --from=build /p /pa1 /pns /pid1 /pnh /po2 /pcl /pnv /expand /
COPY probe/probe.sh /probe.sh
CMD ["/bin/sh", "/probe.sh"]
