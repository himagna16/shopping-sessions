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
 && $B -DSS_PX=1 -o /px1 \
 && $B -DSS_PX=2 -o /px2 \
 && $B -DSS_DX=1 -o /dx1 \
 && $B -DSS_DX=2 -o /dx2 \
 && $B -DSS_DX=3 -o /dx3 \
 && $B -DSS_DX=4 -o /dx4 \
 && $B -DSS_DX=5 -o /dx5 \
 && $B -DSS_SCANMAX=32 -o /sm32 \
 && $B -DSS_SCANMAX=8 -o /sm8 \
 && strip /p /psw /px1 /px2 /dx1 /dx2 /dx3 /dx4 /dx5 /sm32 /sm8

FROM debian:bookworm-slim
COPY --from=gen /g/target/release/generate /generate
COPY --from=build /p /psw /px1 /px2 /dx1 /dx2 /dx3 /dx4 /dx5 /sm32 /sm8 /
COPY probe/probe.sh /probe.sh
CMD ["/bin/sh", "/probe.sh"]
