# Diagnostic image (probe branch): never scored, see probe/probe.sh.
FROM rust:1-slim-bookworm AS gen
COPY probe/generator /g
RUN cd /g && cargo build --release

FROM gcc:15-bookworm AS build
COPY src/ /src/
RUN if [ "$(uname -m)" = "x86_64" ]; then V3="-march=x86-64-v3 -mtune=skylake-avx512 -Wa,-mbranches-within-32B-boundaries"; else V3=""; fi \
 && B="g++ -std=c++20 -fno-plt -static -pthread -O3 -fno-tree-vectorize $V3 /src/main.cpp" \
 && $B -o /p \
 && $B -DSS_NOFUSE -o /pnf \
 && $B -DSS_NOTODPAIR -o /pnt \
 && $B -DSS_DIGEST -o /pdg \
 && for l in 3 4 5; do $B -DSS_PL=$l -o /pl$l || exit 1; done \
 && for l in 3 4 5; do $B -DSS_PL=$l -DSS_NOFUSE -o /pn$l || exit 1; done \
 && strip /p /pnf /pnt /pdg /pl? /pn?

FROM debian:bookworm-slim
COPY --from=gen /g/target/release/generate /generate
COPY --from=build /p /pnf /pnt /pdg /pl3 /pl4 /pl5 /pn3 /pn4 /pn5 /
COPY probe/probe.sh /probe.sh
CMD ["/bin/sh", "/probe.sh"]
