# Diagnostic image (probe branch): never scored, see probe/probe.sh.
FROM gcc:14-bookworm AS build
COPY src/ /src/
COPY tools/ /tools/
RUN if [ "$(uname -m)" = "x86_64" ]; then V3="-march=x86-64-v3 -mtune=skylake-avx512 -Wa,-mbranches-within-32B-boundaries"; else V3=""; fi \
 && B="g++ -std=c++20 -fno-plt -static -pthread -O3 $V3 /src/main.cpp" \
 && $B -o /p \
 && $B -DSS_MIXHASH -DSS_NOFORCEINLINE -DSS_NOSCANPF -o /pold \
 && $B -DSS_MIXHASH -o /pmix \
 && $B -DSS_NOFORCEINLINE -o /pnoinl \
 && $B -DSS_NOSCANPF -o /pnopf \
 && g++ -std=c++20 -O2 -static -pthread -o /expand /tools/expand.cpp \
 && strip /p /pold /pmix /pnoinl /pnopf /expand

FROM debian:bookworm-slim
COPY --from=build /p /pold /pmix /pnoinl /pnopf /expand /
COPY probe/probe.sh /probe.sh
CMD ["/bin/sh", "/probe.sh"]
