# Diagnostic image (probe branch): never scored, see probe/probe.sh.
FROM rust:1-slim-bookworm AS gen
COPY probe/generator /g
RUN cd /g && cargo build --release

FROM gcc:15-bookworm AS build
COPY src/ /src/
RUN if [ "$(uname -m)" = "x86_64" ]; then V3="-march=x86-64-v3 -mtune=skylake-avx512 -Wa,-mbranches-within-32B-boundaries"; else V3=""; fi \
 && B="g++ -std=c++20 -fno-plt -static -pthread -O3 -fno-tree-vectorize $V3 /src/main.cpp" \
 && $B -o /p \
 && $B -DSS_B36_SWAR -o /psw \
 && for l in 1 2 3 4 5 6 7; do $B -DSS_PL=$l -o /pl$l || exit 1; done \
 && for l in 1 2 3 4; do $B -DSS_DX=$l -o /dx$l || exit 1; done \
 && strip /p /psw /pl? /dx?

FROM debian:bookworm-slim
COPY --from=gen /g/target/release/generate /generate
COPY --from=build /p /psw /pl1 /pl2 /pl3 /pl4 /pl5 /pl6 /pl7 /dx1 /dx2 /dx3 /dx4 /
COPY probe/probe.sh /probe.sh
CMD ["/bin/sh", "/probe.sh"]
